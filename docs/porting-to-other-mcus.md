# Porting auto-damper to a non-Raspberry-Pi host MCU

Auto-damper targets Raspberry Pi Pico 2W (RP2350A + on-package CYW43439).
If you want to build it on a different host MCU (for example an ESP32),
the wireless side has licensing constraints because the CYW43439
firmware blobs in `modules/lib/zephyr-cyw43-bt/firmware/43439/` are
dual-licensed.

## Short answer

You have two options for the wireless front end. Pick one.

### Option 1: Use a Raspberry Pi Radio Module 2 (RM2)

The Raspberry Pi Radio Module 2 (part number `RMC20452T`, commonly
"RM2") packages the same Infineon CYW43439 used on the Pico W and
Pico 2 W into a 16.5 mm x 14.5 mm SPI-attached, modular-certified
wireless module that Raspberry Pi sells for use with their own
microcontrollers. Pairing the RM2 with a non-Raspberry-Pi host MCU
keeps you under the permissive Raspberry Pi license terms
(`LICENSE.cyw43-driver.RP`) and preserves commercial use rights. The
existing zephyr-cyw43-bt module's BT HCI driver and WHD patches
should work with the RM2 since the silicon is the same; the SPI bus
wiring and pin assignments will need to be adapted to your host MCU.

This is the path to take if you want to keep using auto-damper's
existing wireless stack on a non-RPi MCU.

### Option 2: Use a different wireless chip and driver entirely

If your host platform already has its own wireless silicon (ESP32's
integrated radio, an STM32 with an external nRF, etc.), you cannot
relicense the cyw43-driver blobs. Their alternate license
(`LICENSE.cyw43-driver`, the George Robotics terms) restricts use to
personal projects only with no commercial purpose, with commercial
licensing available through George Robotics directly.

In this case you would need to replace the wireless stack with the
driver and HAL appropriate to your chip - ESP-IDF's WiFi/BT stack on
an ESP32, Zephyr's nRF Connect drivers on a Nordic chip, and so on.
The damper, BLE heater protocols, NVS, REST API, and web UI are
chip-agnostic and would carry over; only the radio integration would
need new work.

### What does NOT work

Soldering a bare CYW43439 chip onto a custom non-Raspberry-Pi board
is not Option 1 territory. The Raspberry Pi license terms grant rights
in conjunction with semiconductor devices produced by Raspberry Pi -
that covers their MCUs and the RM2 module, not raw Infineon silicon
sourced elsewhere. Going that route forces you onto the personal-use
George Robotics license.

## Where the legal detail lives

The blob licenses and the full dual-license explanation are at
`modules/lib/zephyr-cyw43-bt/firmware/43439/THIRD_PARTY_NOTICES.md`.
Read that file (and the upstream license files it points at) in full
before shipping a commercial product based on this code.
