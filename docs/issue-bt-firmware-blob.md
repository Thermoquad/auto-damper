# Issue: MURATA-1YN BT firmware blob does not boot on CYW43439 (Pico W / Pico 2W)

> Draft issue for filing against zephyrproject-rtos/hal_infineon or
> zephyrproject-rtos/zephyr.

---

**Title:** Bluetooth: MURATA-1YN HCD firmware blob incomplete — BT core fails to start on CYW43439 (Pico W / Pico 2W)

## Bug Description

The Infineon-provided BT firmware blob for the MURATA-1YN module
(`blobs/img/bluetooth/firmware/COMPONENT_43439/COMPONENT_MURATA-1YN/bt_firmware.hcd`)
does not contain enough patchram data to boot the BT core on the CYW43439 as
found on Raspberry Pi Pico W and Pico 2W boards.

After downloading the firmware via Write_RAM HCI commands and issuing
Launch_RAM, the BT core never sets the `FW_RDY` bit (BIT 24 in
`BT_CTRL_REG` 0x18000c7c). The register reads as `0x00000000` indefinitely.

Replacing the blob with the equivalent firmware from the Pico SDK
(`cyw43_btfw_43439.h` in [georgerobotics/cyw43-driver](https://github.com/georgerobotics/cyw43-driver/tree/main/firmware))
results in a fully operational BT 5.2 stack.

## Firmware Comparison

| | Infineon blob (hal_infineon) | Pico SDK firmware (cyw43-driver) |
|---|---|---|
| File size | 4,857 bytes | 6,963 bytes (converted to HCD) |
| HCD records | 212 Write_RAM + 1 Launch_RAM | 30 Write_RAM + 1 Launch_RAM |
| Total patch data written | 3,366 bytes | 6,746 bytes |
| `0x000Dxxxx` region (code patches) | 860 bytes | 3,472 bytes |
| `0x0021xxxx` region (config/BRCMcfgS) | 2,506 bytes | 3,266 bytes |
| BT_CTRL_REG after download | `0x00000000` (FW_RDY never set) | FW_RDY set, BT 5.2 operational |

Both blobs are in Broadcom HCD format, write to the same address ranges
(`0x0021E000` config starting with "BRCMcfgS", `0x000Dxxxx` code patches),
and use the same HCI opcodes (Write_RAM 0xFC4C, Launch_RAM 0xFC4E). The
Infineon blob simply has less data — roughly half the patchram content.

The Pico SDK firmware version string is:
`CYW4343A2_001.003.016.0065.0000_Generic_SDIO_37MHz_wlbga_BU_RPI_dl_signed`

## Steps to Reproduce

1. Build a Zephyr application targeting `rpi_pico2/rp2350a/m33/w` (or
   `rpi_pico/rp2040/w`) with `CONFIG_BT=y` and a BT HCI driver that uses the
   shared-bus (SPI backplane) transport
2. Fetch blobs: `west blobs fetch hal_infineon`
3. The BT firmware from `COMPONENT_43439/COMPONENT_MURATA-1YN/bt_firmware.hcd`
   is compiled into the image
4. On boot, the driver downloads the firmware to the BT core via backplane
   Write_RAM commands
5. Poll `BT_CTRL_REG` (0x18000c7c) for `FW_RDY` (BIT 24)
6. **Result:** `FW_RDY` never asserts. BT core does not start.

## Expected Behavior

The BT firmware blob should contain sufficient patchram data for the CYW43439
to boot its BT core and assert `FW_RDY`.

## Workaround

Convert the Pico SDK BT firmware (`cyw43_btfw_43439.h` from
[georgerobotics/cyw43-driver](https://github.com/georgerobotics/cyw43-driver/blob/main/firmware/cyw43_btfw_43439.h))
from Intel HEX binary record format to Broadcom HCD format, and replace the
generated include file (`build/zephyr/include/generated/bt_firmware.hcd.inc`)
after the initial build.

This firmware is MIT-licensed in the cyw43-driver repo and powers all Pico W /
Pico 2W Bluetooth in the Pico SDK, MicroPython, and CircuitPython ecosystems.

## Suggested Fix

Add the Pico SDK BT firmware as a blob in `hal_infineon` (or as a separate
module) with proper hashing, selectable via Kconfig for CYW43439 on Raspberry
Pi boards. This follows the precedent set by
[beechwoods-software/zephyr-cyw43-driver](https://github.com/beechwoods-software/zephyr-cyw43-driver),
which successfully used the Pico SDK WiFi firmware as a Zephyr blob before
Infineon provided an official WiFi driver to mainline.

Alternatively, update the existing MURATA-1YN BT blob to a version that
includes the full patchram content needed for CYW43439.

## Environment

- Board: Raspberry Pi Pico 2W (`rpi_pico2/rp2350a/m33/w`)
- SoC: RP2350A + Infineon CYW43439 (MURATA-1YN module)
- Zephyr version: v4.1.0
- hal_infineon: current main branch
- WiFi: working (AIROC driver via WHD)
- BT transport: shared-bus (SPI backplane circular buffer protocol)
