# CYW43439 BT HCI Shared-Bus Bringup Notes

Notes from bringing up the CYW43439 BT HCI shared-bus driver on Raspberry Pi
Pico 2W running Zephyr RTOS. The driver lives in
`zephyr/drivers/bluetooth/hci/hci_cyw43_sbus.c` on the
`feat/bt-hci-cyw43-shared-bus` branch.

## Architecture

The CYW43439 exposes BT via a shared-memory circular buffer protocol over the
SPI backplane — the same bus WiFi uses. There is no dedicated BT UART on the
Pico W/2W; the BT UART pins on the CYW43439 are not routed to the RP2350.

The driver uses WHD (WiFi Host Driver) bus APIs to access the CYW43439
backplane. This means the AIROC WiFi driver must be initialized first —
BT piggybacks on the existing SPI transport.

### Init Sequence

Matches the Pico SDK `cyw43_btbus_init()`:

1. Get WHD driver handle from AIROC WiFi
2. Power up BT core (`BT2WLAN_PWRUP_ADDR = 0x03`)
3. Download BT firmware (HCD records → backplane RAM writes)
4. Wait for `FW_RDY` (BIT 24 in `BT_CTRL_REG 0x18000c7c`)
5. Read `WLAN_RAM_BASE` (0x18000d68) → compute circular buffer addresses
6. Wait for `BT_AWAKE` (BIT 8 in `BT_CTRL_REG`)
7. Set `SW_RDY` in `HOST_CTRL_REG` (0x18000d6c)
8. Toggle `DATA_VALID` bit to signal BT controller
9. Start RX polling thread

## Bug 1: Dead Bus Reads (0xFFFFFFFF)

### Symptom

All backplane register reads returned `0xFFFFFFFF`. `BT_CTRL_REG` read as
`0xFFFFFFFF`, so `FW_RDY` (BIT 24) and `BT_AWAKE` (BIT 8) both appeared set
(all bits high). Init proceeded past the ready/awake checks but crashed with a
bus fault when trying to access circular buffers at addresses derived from a
`WLAN_RAM_BASE` of `0xFFFFFFFF`.

### Root Cause

The driver was using `whd_bus_transfer_backplane_bytes()` for all backplane
access. This is a **low-level** WHD function that does NOT call
`whd_ensure_wlan_bus_is_up()`. When the CYW43439 SPI bus is in sleep mode,
reads return all-ones.

### Fix

Switch to higher-level WHD APIs that include bus wakeup:

| Operation | Before (broken) | After (working) |
|-----------|-----------------|-----------------|
| Register read | `whd_bus_transfer_backplane_bytes(BUS_READ)` | `whd_bus_read_reg_value()` |
| Register write | `whd_bus_transfer_backplane_bytes(BUS_WRITE)` | `whd_bus_write_reg_value()` |
| Memory read | `whd_bus_transfer_backplane_bytes(BUS_READ)` | `whd_bus_mem_bytes(drv, 0, ...)` |
| Memory write | `whd_bus_transfer_backplane_bytes(BUS_WRITE)` | `whd_bus_mem_bytes(drv, 1, ...)` |

All three replacement functions call `whd_ensure_wlan_bus_is_up()` before the
transfer. `whd_bus_mem_bytes` also handles 4KB backplane window boundary
splitting internally, so the manual chunking loop was removed.

**Header change:** Replaced `#include <whd_bus_common.h>` with
`#include <whd_bus_protocol_interface.h>`.

### WHD Bus API Hierarchy

```
whd_bus_read_reg_value()          ← calls whd_ensure_wlan_bus_is_up()
whd_bus_write_reg_value()         ← calls whd_ensure_wlan_bus_is_up()
whd_bus_mem_bytes()               ← calls whd_ensure_wlan_bus_is_up()
  └─ whd_bus_transfer_backplane_bytes()  ← NO bus wakeup (raw access)
```

Source: `modules/hal/infineon/whd-expansion/WHD/COMMON/src/whd_bus_common.c`
and `WHD/COMPONENT_WIFI5/src/bus_protocols/whd_bus.c`.

## Bug 2: BT Firmware Blob Mismatch

### Symptom

After fixing the bus-wake issue, `BT_CTRL_REG` read as `0x00000000` — real
data, but `FW_RDY` (BIT 24) never set. The BT firmware downloaded without
errors but the BT core didn't start.

### Root Cause

The Infineon-provided HCD firmware blob
(`hal_infineon/zephyr/blobs/img/bluetooth/firmware/COMPONENT_43439/COMPONENT_MURATA-1YN/bt_firmware.hcd`)
is **incomplete** for the Pico 2W's CYW43439 variant:

| | Infineon blob | Pico SDK blob |
|---|---|---|
| File size | 4,857 bytes | 6,963 bytes (converted to HCD) |
| Patch data written | 3,366 bytes | 6,746 bytes |
| `0x000Dxxxx` region (code) | 860 bytes | 3,472 bytes |
| `0x0021xxxx` region (config) | 2,506 bytes | 3,266 bytes |
| Result | FW_RDY never set | FW_RDY set, BT 5.2 operational |

Both blobs write to the same address ranges (`0x0021E000` config starting with
"BRCMcfgS", `0x000Dxxxx` code patches) and are in the same HCD record format.
The Infineon blob simply has less data — about half the patchram content.

### Fix

Convert the Pico SDK firmware (`cyw43_btfw_43439.h` from
`pico-sdk/lib/cyw43-driver/firmware/`) from Intel HEX binary record format to
Broadcom HCD format. The conversion script (`bt_firmware_pico.hcd`) is checked
in.

The Zephyr build system verifies blob hashes (`zephyr_blobs_verify`), so the
converted file can't replace the Infineon blob directly. Instead, the generated
include file (`build/zephyr/include/generated/bt_firmware.hcd.inc`) is
overwritten after the initial build, followed by an incremental rebuild. This
is a temporary workaround until the Infineon blob is updated or a proper
Kconfig bypass is added.

### Firmware Format Comparison

**Infineon HCD format** (what our driver parses):
```
[opcode_lo][opcode_hi][param_len][params...]
Write_RAM (0xFC4C): params = [4-byte dest addr LE][data...]
Launch_RAM (0xFC4E): params = [4-byte addr LE] (0xFFFFFFFF = default)
```

**Pico SDK format** (Intel HEX binary records, with header):
```
[version_len][version_string][record_count]
[num_bytes][addr_hi][addr_lo][type][data...]
type 0 = data, type 4 = extended linear address
```

The conversion script in the repo handles the format translation.

### Build Workaround

After a clean `west build -p always`:
```bash
python3 -c "
with open('bt_firmware_pico.hcd', 'rb') as f:
    data = f.read()
lines = []
for i in range(0, len(data), 8):
    chunk = data[i:i+8]
    lines.append(', '.join(f'0x{b:02x}' for b in chunk) + ',')
with open('build/zephyr/include/generated/bt_firmware.hcd.inc', 'w') as f:
    f.write('\n'.join(lines) + '\n')
"
touch ../../zephyr/modules/hal_infineon/btstack-integration/w_bt_firmware_controller.c
west build
```

## Bug 3: Missing BT Interrupt Toggle

### Symptom

(Fixed in a prior session.) BT_AWAKE timeout — the BT controller never
signaled awake after firmware download.

### Root Cause

The `sbus_toggle_bt_intr()` call was missing from the init sequence. After
setting `SW_RDY`, the host must toggle the `DATA_VALID` bit in `HOST_CTRL_REG`
to signal the BT controller that the host is ready.

### Fix

Added `sbus_toggle_bt_intr(data)` after `sbus_reg_write(SW_RDY)` in
`sbus_open()`.

## Upstream Implications

The Infineon-provided MURATA-1YN BT firmware blob in `hal_infineon` doesn't
work on Pico W/2W hardware. The Zephyr build system verifies blob hashes
(`zephyr_blobs_verify`), so a PR cannot simply swap the blob — CI would reject
it.

**Options for upstreaming the driver:**

1. **Report the Infineon blob** — file an issue to get Infineon to update the
   MURATA-1YN BT firmware to a version that actually boots on CYW43439.

2. **Add the Pico SDK firmware as a new blob** — register the RPi-provided
   firmware (`cyw43_btfw_43439.h` from the cyw43-driver repo) in `hal_infineon`
   with proper hashing. This firmware is publicly available and powers all
   Pico W/2W Bluetooth in the Pico SDK ecosystem.

3. **Ungate `AIROC_CUSTOM_FIRMWARE_HCD_BLOB`** — this Kconfig option exists
   but is inside `if BT_AIROC` (Kconfig.infineon:26). Our driver uses
   `BT_CYW43_SBUS`, not `BT_AIROC`, so the option is invisible. Moving it
   outside the `if` block would let users supply a working firmware.

Option 2 is the most realistic path. The firmware is MIT-licensed in the
cyw43-driver repo and is the de facto standard for this hardware.

### Precedent: beechwoods-software/zephyr-cyw43-driver

[beechwoods-software/zephyr-cyw43-driver](https://github.com/beechwoods-software/zephyr-cyw43-driver)
proved this approach works for **WiFi**: instead of the Infineon-provided WiFi
blobs, they wrapped the [georgerobotics/cyw43-driver](https://github.com/georgerobotics/cyw43-driver)
(the same driver and firmware that ships in the Pico SDK) as a Zephyr
out-of-tree module. Their work eventually led to Infineon providing an official
WiFi driver to Zephyr mainline.

The BT side never got the same treatment — the Infineon BT blob in
`hal_infineon` is still the incomplete one that doesn't boot on Pico W/2W. The
same pattern applies: take `cyw43_btfw_43439.h` from the georgerobotics
cyw43-driver firmware directory and register it as a proper blob in
`hal_infineon` (or a dedicated module) with correct hashing.

## Known Issues

### WiFi Breaks After BT Init

WiFi connection fails permanently after BT initialization with
`whd_wifi_prepare_join failed checkres = 101580800` (0x060e0000). WiFi was
working before BT init; subsequent retries all fail.

**Likely cause:** The driver doesn't call `whd_bus_bt_attach()` to register BT
with the WHD bus layer. Without this registration:
- WHD doesn't know BT is active on the shared bus
- F1_INTR is never enabled in the SPI interrupt mask (needed for BT→host
  interrupt delivery)
- Bus coordination between WiFi and BT is absent

**Pico SDK equivalent:** The cyw43-driver's `cyw43_ll.c` enables `F1_INTR` in
`SPI_INTERRUPT_ENABLE_REGISTER` when `CYW43_ENABLE_BLUETOOTH` is defined.
The WHD SPI protocol driver (`whd_bus_spi_protocol.c`) has an
`whd_bus_spi_init_stats()` function that enables F1_INTR when
`whd_driver->bt_dev->intr` is set — this is triggered by `whd_bus_bt_attach()`.

**Next step:** Call `whd_bus_bt_attach()` from `sbus_open()` with a BT
interrupt callback, and investigate whether the WiFi prepare_join failure is
caused by missing bus coordination or CYW43439 firmware-level coexistence
state.

### L2CAP Buffer Exhaustion

`"Not enough buffer space for L2CAP data"` errors appear during active BLE
connection. The heater sends notifications frequently (heartbeat + telemetry)
and the default Zephyr BT buffer configuration is too small.

**Fix (not yet applied):** Increase `CONFIG_BT_L2CAP_TX_BUF_COUNT`,
`CONFIG_BT_BUF_ACL_TX_COUNT`, and/or `CONFIG_BT_BUF_ACL_RX_COUNT` in
`prj.conf`.

### WLAN RAM Base Reads as 0x00000000

`WLAN_RAM_BASE_REG_ADDR` (0x18000d68) returns 0. The circular buffer addresses
are computed as `0 + offset`, placing them at the start of the backplane
address space. Despite this, HCI communication works correctly — the BT core
and host agree on the buffer locations. This may be the expected value for the
CYW43439.

## Reference Implementation

The Pico SDK implementation in
`tools/pico-sdk-2.2.0/src/rp2_common/pico_cyw43_driver/cybt_shared_bus/` was
the primary reference:

- `cybt_shared_bus.c` — `cyw43_btbus_init()` (init sequence)
- `cybt_shared_bus_driver.c` — register definitions, `cybt_fw_download()`,
  `cybt_set_host_ready()`, `cybt_toggle_bt_intr()`, `cybt_init_buffer()`

Key addresses (same in both Pico SDK and our driver):
- `BTFW_MEM_OFFSET`: 0x19000000
- `BT_CTRL_REG_ADDR`: 0x18000c7c
- `HOST_CTRL_REG_ADDR`: 0x18000d6c
- `WLAN_RAM_BASE_REG_ADDR`: 0x18000d68
- `BT2WLAN_PWRUP_ADDR`: 0x640894

## Current Status

**Working (2026-05-08):**
- BT firmware download and init
- HCI transport (BT 5.2, manufacturer 0x0131 Infineon/Cypress)
- BLE scanning (found CC heater "Heater5587")
- BLE connection, GATT discovery, notification subscription
- Heater telemetry decode (CC protocol)

**Not working:**
- WiFi + BLE coexistence (WiFi breaks after BT init)
- L2CAP buffer sizing (intermittent buffer exhaustion)
