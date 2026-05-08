# CYW43439 BT HCI Shared-Bus Bringup Notes

Notes from bringing up the CYW43439 BT HCI shared-bus driver on Raspberry Pi
Pico 2W running Zephyr RTOS. The driver lives in
`zephyr/drivers/bluetooth/hci/hci_cyw43_sbus.c` on the
`feat/bt-hci-cyw43-shared-bus` branch.

## Current Status (2026-05-08)

**WiFi + BLE: WORKING simultaneously.**

| Feature | Status | Notes |
|---------|--------|-------|
| BT firmware download | Working | Pico SDK blob via `CONFIG_AIROC_CUSTOM_FIRMWARE_HCD_BLOB` |
| HCI transport | Working | BT 5.2, manufacturer 0x0131 (Infineon/Cypress) |
| BLE scanning | Working | Finds CC heater "Heater5587" |
| BLE connection + GATT | Working | Service/characteristic discovery, notification subscription |
| Heater telemetry | Working | CC protocol decode (temp, voltage, power state, mode) |
| WiFi connection | Working | DHCP, mDNS hostname, HTTP REST API |
| WiFi + BLE coexistence | Working | Simultaneous operation, no data path degradation |

**Build size:** 657KB flash (15.68%), 199KB RAM (37.33%)

### Stability Test Results

Three ping tests from host to Pico 2W (192.168.8.239):

| Test | Conditions | Sent | Received | Loss | Avg RTT | Max RTT |
|------|-----------|------|----------|------|---------|---------|
| Baseline | WiFi only, no BLE | 60 | 59 | 1.67% | 193ms | 702ms |
| BLE telemetry | BLE connected, receiving heartbeat + telemetry | 120 | 118 | 1.67% | 143ms | 591ms |
| BLE full cycle | BLE disconnect + scan + reconnect via HTTP API | 60 | 59 | 1.67% | 157ms | 727ms |

BLE activity adds zero additional packet loss. The ~1.67% loss and high latency
variance are inherent to the CYW43439 shared SPI bus (single radio, single SPI
transport, time-division multiplexing between WiFi and BLE).

During the "BLE full cycle" test, the following HTTP API calls completed
successfully while pings continued uninterrupted:
1. `POST /api/ble/disconnect` — disconnected from heater
2. `POST /api/ble/scan` — started BLE scan (5s timeout)
3. `GET /api/ble/devices` — found Heater5587 at RSSI -49
4. `POST /api/ble/connect` — reconnected to heater
5. `GET /api/ble/status` — returned telemetry (22°C exhaust, 12V, OFF)

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

### WiFi Firmware Blob Requirement

**The CYW43439 WiFi firmware must be BT-aware for BLE to work.** This was the
root cause of 15+ hours of debugging (see Bug 4 and Bug 5 below).

The Infineon-provided WHD firmware (`43439A0.bin` v7.95.88) is WiFi-only. It
does not reserve BT shared memory in WLAN RAM. When this firmware is loaded:
- `WLAN_RAM_BASE_REG_ADDR` (0x18000d68) reads as `0x00000000`
- BT circular buffers are placed at WLAN RAM offset 0, on top of WiFi code
- WiFi firmware code is corrupted; WiFi data path dies immediately

The Pico SDK firmware (`wb43439A0_7_95_49_00_combined.h` v7.95.49) is
BT-aware. It reserves BT shared memory at the end of WLAN RAM:
- `WLAN_RAM_BASE_REG_ADDR` reads as `0x0006861c` (~417KB into 512KB WLAN RAM)
- BT circular buffers are safely placed above WiFi firmware
- WiFi and BLE operate simultaneously without corruption

**Firmware replacement procedure:** See "Firmware Blob Swap Procedure" section.

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

The build system now handles this cleanly:
- `AIROC_CUSTOM_FIRMWARE_HCD_BLOB` was moved outside the `if BT_AIROC` block
  in the Kconfig, making it visible when `BT_CYW43_SBUS` is selected.
- `prj.conf` sets
  `CONFIG_AIROC_CUSTOM_FIRMWARE_HCD_BLOB="bt_firmware_pico.hcd"` directly.

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

## Bug 3: Missing BT Interrupt Toggle

### Symptom

BT_AWAKE timeout — the BT controller never signaled awake after firmware
download.

### Root Cause

The `sbus_toggle_bt_intr()` call was missing from the init sequence. After
setting `SW_RDY`, the host must toggle the `DATA_VALID` bit in `HOST_CTRL_REG`
to signal the BT controller that the host is ready.

### Fix

Added `sbus_toggle_bt_intr(data)` after `sbus_reg_write(SW_RDY)` in
`sbus_open()`.

## Bug 4: WiFi Dies After BLE — The WiFi Firmware Blob

### Symptom

WiFi connects successfully (carrier ON, DHCP bound, IP assigned) but after any
BLE activity (scan or connect), the WiFi data path dies. Pings fail in both
directions. The MAC/IP layer appears healthy while the data plane is broken.

### Investigation (15+ hours)

Extensive investigation identified and fixed multiple real issues that were
ultimately **not the root cause**:

1. **`whd_bus_spi_init_stats` overwrite** — `whd_bus_bt_attach()` was
   overwriting `SDIO_INT_HOST_MASK` with only `I_HMB_FC_CHANGE`, erasing WiFi
   interrupt bits. Fixed with read-modify-write. **Real bug, fixed, but not
   root cause.**

2. **Backplane window race** — `whd_bus_spi_read/write_backplane_value()` had
   no locking between `set_backplane_window()` and register transfer. The BT
   RX thread (K_PRIO_COOP(7)) could preempt the WiFi thread mid-operation.
   Added `whd_bus_backplane_mutex`. **Real bug, fixed, but not root cause.**

3. **`whd_bus_transfer_backplane_bytes` race** — Same window race in the
   `whd_bus_mem_bytes` code path. Added same mutex (recursive). **Real bug,
   fixed, but not root cause.**

4. **WHD thread bus mutex (Bug 5)** — Wrapped entire WHD thread processing
   cycle in `whd_bus_backplane_mutex`. **Reverted — wrong approach, and
   unnecessary with correct firmware.**

5. **NVRAM parameters** — Tested `btc_mode=1`, `muxenab=0x100`, and full Pico
   SDK NVRAM replacement. No effect because NVRAM was already correct
   (`btc_mode=1` was set, `muxenab` is module-specific and the MURATA-1YN
   value is correct for the actual Murata module on the Pico 2W).

6. **BT RX thread pause** — Pausing all BT bus access after BLE scan didn't
   restore WiFi. This proved the damage was permanent, not from ongoing bus
   contention.

7. **WiFi power management** — Disabled PM2 (WHD default). No effect.

8. **SDPCM credit/flow analysis** — Added diagnostic dumps showing TX credits
   healthy (13-16 available), tx_fail=0, but rx_total frozen at 69. The
   CYW43439 was accepting TX packets but delivering zero RX packets.

### Root Cause

**The WHD WiFi firmware blob does not support BT shared memory.**

The Infineon-provided `43439A0.bin` (v7.95.88, 249066 bytes) is WiFi-only. The
Pico SDK's `wb43439A0_7_95_49_00_combined.h` (v7.95.49, 231077 bytes for WiFi
portion) is BT-aware.

The critical difference: when the BT-aware firmware loads, it writes the BT
shared memory base address to `WLAN_RAM_BASE_REG_ADDR` (0x18000d68). The WiFi-
only firmware writes `0x00000000`.

The BT HCI driver reads `WLAN_RAM_BASE_REG_ADDR` to compute circular buffer
addresses. With the WiFi-only firmware:
- `WLAN_RAM_BASE` = 0x00000000
- Write buffer at offset 0 = WLAN RAM address 0 (start of WiFi firmware code)
- Read buffer at offset 0x1000 = WLAN RAM address 0x1000 (still WiFi code)
- **Writing to these buffers corrupts the running WiFi firmware**

With the BT-aware firmware:
- `WLAN_RAM_BASE` = 0x0006861c (~417KB)
- Write buffer at 0x0006861c = safely above WiFi firmware (~231KB)
- Read buffer at 0x0006961c
- Control block at 0x0006a61c
- **WiFi firmware and BT buffers coexist in the 512KB WLAN RAM**

### Fix

Replace the WiFi firmware blob and CLM blob with BT-aware versions from the
Pico SDK. See "Firmware Blob Swap Procedure" below.

### Why the Bus Races Didn't Matter

The backplane window races (items 2-3 above) are real concurrency bugs, and the
mutex fixes are correct. However, they were masked by the firmware blob issue —
WiFi was dying from memory corruption, not from bus races. With the correct
firmware, the existing SPI driver bus mutex (Zephyr's per-bus SPI mutex) plus
the backplane_value mutex provide sufficient protection.

The Bug 5 WHD thread mutex was reverted because it's unnecessary with correct
firmware and adds latency to all bus operations. The Pico SDK's single-threaded
model (`CYW43_THREAD_ENTER/EXIT`) was never needed — it was compensating for
their own lack of fine-grained locking, not for a fundamental CYW43439
requirement.

## Bug 5: L2CAP Buffer Exhaustion

### Symptom

`"Not enough buffer space for L2CAP data"` errors every ~2 seconds during
active BLE connection to the CC heater. The heater sends frequent notifications
(heartbeat + telemetry). Errors caused connection drops.

### Root Cause

The CYW43439 BT controller sends ACL fragments larger than Zephyr's default
buffer size (73 bytes). This is a SIZE issue, not a COUNT issue — increasing
buffer counts had no effect.

The error path in `zephyr/subsys/bluetooth/host/conn.c:432`: ACL continuation
(ACL_CONT) fragments exceeded buffer tailroom.

### Fix

Added to `prj.conf`:

```
# BT buffers — shared-bus transport is slower than UART, need more headroom
CONFIG_BT_L2CAP_TX_BUF_COUNT=12
CONFIG_BT_BUF_ACL_TX_COUNT=12
CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA=8
CONFIG_BT_BUF_EVT_RX_COUNT=16
CONFIG_BT_BUF_ACL_RX_SIZE=200
```

`CONFIG_BT_BUF_ACL_RX_SIZE=200` was the critical fix. The buffer count
increases (`L2CAP_TX_BUF_COUNT`, `ACL_TX_COUNT`, `ACL_RX_COUNT_EXTRA`,
`EVT_RX_COUNT`) provide headroom for the shared-bus transport which is slower
than dedicated UART and needs more in-flight buffers.

## Firmware Blob Swap Procedure

### What Gets Replaced

Three blob files in `modules/hal/infineon/`:

| File | Original | Replacement | Size |
|------|----------|-------------|------|
| `zephyr/blobs/img/whd/resources/firmware/COMPONENT_43439/43439A0.bin` | WHD v7.95.88 (249066 B) | Pico SDK v7.95.49 (231077 B) | WiFi firmware |
| `zephyr/blobs/img/whd/resources/clm/COMPONENT_43439/COMPONENT_MURATA-1YN/43439A0.clm_blob` | WHD CLM (4752 B) | Pico SDK CLM (984 B) | CLM data |
| `zephyr/blobs/img/whd/resources/clm/COMPONENT_43439/COMPONENT_CYW943439M2IPA1/43439A0.clm_blob` | WHD CLM (4752 B) | Pico SDK CLM (984 B) | CLM data |

### Extraction from Pico SDK Combined Header

The Pico SDK stores WiFi firmware + CLM in a single C header
(`wb43439A0_7_95_49_00_combined.h`) as a byte array:

```
[WiFi FW: CYW43_WIFI_FW_LEN bytes][padding to 512 alignment][CLM: CYW43_CLM_LEN bytes]
```

- `CYW43_WIFI_FW_LEN` = 231077 bytes
- CLM offset = `((231077 + 511) // 512) * 512` = 231424
- `CYW43_CLM_LEN` = 984 bytes

### Hash Updates

The Zephyr build system verifies blob SHA256 hashes via `zephyr_blobs_verify`
(hashes defined in `modules/hal/infineon/zephyr/module.yml`). After replacing
blobs, update the three SHA256 entries:

```yaml
# In module.yml, under blobs:
- path: zephyr/blobs/img/whd/resources/firmware/COMPONENT_43439/43439A0.bin
  sha256: 5555e0261da2610a500d68c18d895cace0152bbefbf76f4aa683ebce77e3d7eb
  description: "Wi-Fi+BT Firmware for CYW43439 (Pico SDK v7.95.49, BT-aware)"

- path: zephyr/blobs/img/whd/resources/clm/COMPONENT_43439/COMPONENT_MURATA-1YN/43439A0.clm_blob
  sha256: e712b3d218e8b1e2747b092e03b8b0afcb8c8c8e355d2a4a0d47b493800f3f89

- path: zephyr/blobs/img/whd/resources/clm/COMPONENT_43439/COMPONENT_CYW943439M2IPA1/43439A0.clm_blob
  sha256: e712b3d218e8b1e2747b092e03b8b0afcb8c8c8e355d2a4a0d47b493800f3f89
```

### NVRAM

The MURATA-1YN NVRAM (`cyfmac43439-1YN.txt`) already has `btc_mode=1`
(BT coexistence arbiter enabled). No NVRAM changes were needed.

### CLM Auto-Detection

CLM blob size is auto-detected at build time via `file(SIZE ...)` in
`zephyr/modules/hal_infineon/whd-expansion/CMakeLists.txt`. No size constants
need updating when the CLM blob changes size.

## Upstream Implications

### Firmware Blobs

The Infineon-provided WiFi and BT firmware blobs in `hal_infineon` don't work
for BLE on Pico W/2W:

- **WiFi blob** (`43439A0.bin`): WiFi-only, no BT shared memory support.
  Must be replaced with the Pico SDK BT-aware version for BLE to work.
- **BT blob** (`bt_firmware.hcd`): Incomplete patchram, only ~50% of required
  data. `FW_RDY` never sets. Must use Pico SDK BT firmware.

**Options for upstream:**

1. **Register Pico SDK firmware as new blobs** — The RPi-provided firmware
   from `georgerobotics/cyw43-driver` is publicly available and MIT-licensed.
   Register it in `hal_infineon` with proper hashing, gated by a Kconfig for
   Pico W/2W boards.

2. **Report to Infineon** — File issues for both incomplete BT firmware and
   WiFi-only firmware blob. The CYW43439 supports BT; the firmware should too.

### Precedent: beechwoods-software/zephyr-cyw43-driver

[beechwoods-software/zephyr-cyw43-driver](https://github.com/beechwoods-software/zephyr-cyw43-driver)
proved this approach for **WiFi**: they wrapped the Pico SDK cyw43-driver as a
Zephyr module, eventually leading Infineon to provide an official WiFi driver.
The BT side needs the same treatment.

### WHD Bus Fixes

The `whd_bus_spi_init_stats` read-modify-write fix (Bug 4, item 1) and the
backplane_value mutex (Bug 4, item 2) are legitimate bug fixes that should be
upstreamed regardless of firmware blob choice. They protect against:
- Interrupt mask corruption when BT attaches to the bus
- Backplane window races between WiFi and BT threads

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

### Pico SDK Threading Model

The Pico SDK is effectively single-threaded for CYW43439 access.
`cyw43_thread_enter()/exit()` acquires a global lock (`async_context_acquire_
lock_blocking`) that wraps ALL bus operations. WiFi polling and BT operations
never execute concurrently.

Zephyr's multi-threaded approach (WHD thread + BT stack thread + BT RX thread)
requires finer-grained locking. The key protections:
- Per-bus SPI mutex (Zephyr SPI driver) — serializes individual SPI transfers
- `whd_bus_backplane_mutex` — serializes backplane register access (window +
  transfer atomic)
- `whd_bus_spi_init_stats` read-modify-write — prevents interrupt mask
  corruption during BT attach

## Investigation Log (Bug 4, 2026-05-08)

Preserved for reference — this was a 15+ hour investigation before the firmware
blob root cause was identified.

### What Didn't Fix It (with WiFi-only firmware)

1. **Backplane mutex on `_backplane_value` functions** — Protected
   `set_backplane_window` + register transfer. Correct fix, but not root cause.
2. **Backplane mutex on `whd_bus_transfer_backplane_bytes`** — Same mutex on
   `whd_bus_mem_bytes` path. Correct fix, but not root cause.
3. **`btc_mode=1` in NVRAM** — Already set. No effect.
4. **`muxenab=0x100` in NVRAM** — Pico SDK value. No effect.
5. **Full Pico SDK NVRAM** — Replaced entire NVRAM. No effect.
6. **Paused BT RX thread** — Proved damage was permanent (not ongoing races).
7. **Disabled `whd_bus_spi_bt_packet_available_to_read`** — Broke BT.
8. **WiFi PM disable (PM2→OFF)** — No effect.
9. **WHD thread bus mutex** — Wrapped entire WHD processing cycle in mutex.
   WiFi still died. Reverted.

### Key Diagnostic Finding

SDPCM analysis showed:
- TX credits healthy (13-16 available), no_credit=0, flow_control=0
- tx_seq increased normally (packets sent to CYW43439)
- rx_total frozen (CYW43439 delivered zero RX packets after BLE init)
- `whd_wifi_is_ready_to_transceive: YES` but `whd_wifi_get_bssid: FAILED`

The WiFi firmware was corrupted in place — accepting TX but unable to generate
RX responses. This is consistent with BT buffer writes at WLAN RAM offset 0
overwriting WiFi firmware code.

### NVRAM Comparison (Investigated but Not Root Cause)

The MURATA-1YN NVRAM differs from Pico SDK NVRAM in several parameters. These
differences were investigated as a potential root cause but turned out to be
irrelevant — the MURATA-1YN NVRAM is correct for the actual hardware module.

| Parameter | MURATA-1YN | Pico SDK | Impact |
|-----------|-----------|----------|--------|
| `btc_mode` | 1 | 1 | Same — BT coex enabled |
| `muxenab` | 0x11 | 0x100 | GPIO mux — module-specific |
| `boardflags3` | 0x08000000 | 0x04000000 | Board feature flags |
| `maxp2ga0` | 74 | 84 | Max TX power |
| `pa2ga0` | -168,6777,-789 | -168,7161,-820 | PA calibration |

These are legitimate hardware differences between the MURATA-1YN module and
the Raspberry Pi RM2 module (which uses Pico SDK NVRAM). Both modules use the
CYW43439 but have different RF layouts and calibration.
