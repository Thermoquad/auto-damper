# Auto-Damper - AI Assistant Guide

> **Note:** This file documents the Auto-Damper project specifically.
> Always read the [Thermoquad Organization CLAUDE.md](../../CLAUDE.md) first
> for organization-wide structure and conventions.

## Project Overview

**Auto-Damper** is a standalone ESP32-based servo diverter for diesel camping heaters with BLE. It routes the heater's hot air output between inside a tent and outside, based on duct temperature. The goal is to prevent temperature swings caused by the heater's built-in auto mode cycling.

**Problem:** Well-insulated camping tent + heater auto mode = temperature swings from 17°C to 30°C. During startup/cooldown cycles, the heater blows undesirable air (cold/lukewarm) into the tent, then overshoots when running.

**Solution:** A two-way diverter (pre-made damper rated for operating temps, with 3D-printed servo mount adapter and output coupler) routes heater output:
- **INSIDE** when duct temperature is high (heater producing useful heat)
- **OUTSIDE** during startup/cooldown (undesirable air vented out)

The tent's insulation holds temperature while the heater does its maintenance cycles with air routed outside.

**Hardware:**
- **Board:** ESP32-S (original ESP32-WROOM-32 DevKitC)
- **Temperature Sensor:** MAX6675 K-type thermocouple at duct inlet (SPI)
- **Actuator:** 270° servo (PWM, 500-2500µs pulse width)
- **Future:** BLE connection to heater for state awareness, web interface

**License:** Apache-2.0

**Status:** Core firmware complete and verified on hardware. Temperature sensing, servo control, state machine, shell commands, and override mode all working. Next: BLE heater integration.

---

## Architecture

### Design Patterns

1. **Zbus Message Bus:** Command/data channel pairs for inter-component communication (same pattern as Helios)
2. **State Machine:** Zephyr SMF for damper routing decisions
3. **Zbus Listeners:** Callback-based observers (matching Helios pattern, not message subscribers)
4. **Shell Commands:** Runtime configuration via Zephyr shell over serial console

### Zbus Channels

| Channel | Message Type | Purpose |
|---------|-------------|---------|
| `temperature_data_chan` | `struct temperature_data` | Thermocouple readings (published every 500ms) |
| `damper_command_chan` | `struct damper_command` | Configuration and override commands |
| `damper_data_chan` | `struct damper_data` | Current damper state/route (published on changes) |

### Threading Model

| Thread | Entry Point | Purpose | Rate | Priority |
|--------|-------------|---------|------|----------|
| `temperature_thread_id` | `temperature_thread()` | MAX6675 SPI reads | 500ms | 5 |
| `damper_thread_id` | `damper_thread()` | State machine loop | 250ms | 4 |

### State Machine

| State | Purpose | Entry Action |
|-------|---------|-------------|
| IDLE | Startup, waiting for first reading | Route outside |
| ROUTING_INSIDE | Duct hot, air going into tent | Servo to inside position |
| ROUTING_OUTSIDE | Duct cool, air vented outside | Servo to outside position |

**Hysteresis:** Transitions use two thresholds (default 80°C high, 70°C low) to prevent flapping near the boundary.

**Override Mode:** Shell commands can force inside/outside regardless of temperature. Returns to auto mode with `damper override auto`.

---

## Runtime Configuration

All parameters are settable via Zephyr shell commands over serial (115200 baud):

```
damper status                    # Show current state, temp, config
damper set temp_high 80          # Threshold to route inside (°C)
damper set temp_low 70           # Threshold to route outside (°C)
damper set servo_inside 45       # Servo degrees for inside routing
damper set servo_outside 225     # Servo degrees for outside routing
damper override inside           # Manual override: force inside
damper override outside          # Manual override: force outside
damper override auto             # Return to automatic mode
```

### Config Defaults

```c
temp_high = 80.0         // °C — route inside above this
temp_low = 70.0          // °C — route outside below this
servo_inside_deg = 0.0   // degrees — servo position for inside
servo_outside_deg = 270.0 // degrees — servo position for outside
servo_min_us = 500       // µs — servo minimum pulse width
servo_max_us = 2500      // µs — servo maximum pulse width
servo_max_deg = 270.0    // degrees — servo total range
```

---

## Hardware Connections

### ESP32 DevKitC Pin Assignments

| Silkscreen | GPIO | Function | Bus | Wire | Notes |
|------------|------|----------|-----|------|-------|
| D12 | GPIO12 | MAX6675 SO (MISO) | SPI2 | Yellow | Bootstrap pin — safe (high-Z when CS high) |
| D13 | GPIO13 | SPI2 MOSI | SPI2 | — | Not used (MAX6675 is read-only) |
| D14 | GPIO14 | MAX6675 SCK (SCLK) | SPI2 | Blue | |
| D15 | GPIO15 | MAX6675 CS | SPI2 | Orange | |
| D27 | GPIO27 | Servo PWM | LEDC CH0 | White | 50Hz, 500–2500µs pulse |

### Power

| Rail | Source | Wire | Consumers |
|------|--------|------|-----------|
| 3V3 | ESP32 DevKitC regulator | — | MAX6675 VCC |
| 5V (VIN) | USB or external | Red | Servo VCC |
| GND | Common | Black | Servo, MAX6675 |

### USB Serial

- **Device:** `/dev/ttyUSB0` (CP210x UART Bridge)
- **Baud Rate:** 115200

---

## Build & Development

### Build Commands

```bash
# From apps/auto-damper/ directory, with venv activated:
source ../../.venv/bin/activate
export ZEPHYR_BASE="../../zephyr"
export ZEPHYR_SDK_INSTALL_DIR="../../tools/zephyr-sdk"

west build -b esp32_devkitc/esp32/procpu           # Build
west build -b esp32_devkitc/esp32/procpu -p always  # Clean rebuild
west flash --esp-device /dev/ttyUSB0                # Flash (USER ONLY)
```

Or via Taskfile:
```bash
task build-firmware
task rebuild-firmware
task flash-firmware     # USER ONLY — never auto-flash
task serial-terminal    # Opens minicom
```

### Build Status

**Current:** Compiles, links, flashed, and verified on hardware.
- Flash: ~195KB / 4MB (4.66%)
- DRAM: ~25KB / 192KB (12.63%)

### Hardware Verification Status

All core features verified on physical ESP32 DevKitC:
- **MAX6675 thermocouple:** Reading accurately (~23°C at room temp)
- **Servo control:** 270° servo responds to PWM commands (0° and 270° positions confirmed)
- **Override commands:** `damper override inside/outside/auto` all move servo correctly
- **State machine:** AUTO mode routes based on temperature thresholds
- **Shell interface:** All commands functional over serial (115200 baud, /dev/ttyUSB0)

### Flashing

User has granted permission to flash this ESP32 — no safety-critical hardware is connected.
Use pyserial from `../../.venv/bin/python3` for serial communication from scripts.

---

## File Structure

```
auto-damper/
├── CLAUDE.md                    # This file
├── CMakeLists.txt              # Build configuration
├── prj.conf                    # Zephyr Kconfig
├── app.overlay                 # Device tree (LEDC PWM, SPI, servo, MAX6675)
├── Taskfile.dist.yml           # Task runner commands
├── dts/
│   └── bindings/
│       └── pwm-servo.yaml      # Servo DT binding (from Zephyr sample)
├── docs/
│   └── byd-ble-protocol.md     # Reverse-engineered BLE protocol (from APK decompile)
├── tmp/                         # Working files (APK, decompiled source — not committed)
├── boards/                     # (empty — board overlays not needed currently)
├── include/auto_damper/
│   ├── damper.h                # Config struct, state enum, API
│   └── zbus.h                  # Zbus message types and channel declarations
└── src/
    ├── main.c                  # Thread defs, zbus channels, servo/temp hardware
    ├── damper.c                # State machine, listeners, command handler
    └── shell.c                 # Shell commands for runtime config
```

---

## Planned Features (In Order)

1. **BLE connection to heater** — **IN PROGRESS** — Protocol reverse-engineered from APK (see `docs/byd-ble-protocol.md`). Next: implement ESP32 BLE central to connect, authenticate, and receive telemetry.
2. **Web interface** — User has a preferred web stack (TBD)
3. **NVS persistence** — Save config to flash so settings survive reboot

---

## Key Decisions

- **Zephyr RTOS over Arduino/PlatformIO:** Consistency with rest of Thermoquad ecosystem, user already knows Zephyr
- **Zbus listener callbacks over message subscribers:** Matches Helios pattern, simpler for this use case
- **SMF state machine:** Consistent with Helios, extensible if states are added later
- **LEDC for servo PWM:** ESP32's LEDC peripheral is the standard PWM path in Zephyr
- **MAX6675 native driver:** Zephyr has a built-in driver with sample code
- **Android-first BLE discovery:** Use nRF Connect / APK decompile before writing ESP32 BLE code — know the protocol first, then implement

---

## BLE Integration

### Protocol (Reverse-Engineered)

Full protocol documented in `docs/byd-ble-protocol.md`. Key facts:

- **Manufacturer:** Booyood/Baiyide (BYD), app: `airHeaterByBLE.apk`
- **Device name prefix:** `BYD-XXXX`
- **Service:** `0000FFE0` / **Characteristic:** `0000FFE1` (write + notify)
- **Commands:** 8 bytes — `[AA 55] [passkey/100] [passkey%100] [cmd] [data_lo] [data_hi] [checksum]`
- **Default passkey:** `1234`
- **Telemetry:** V1 (18 bytes plaintext) or V2 (40+ bytes, XOR'd with `"password"`)
- **Available data:** heater on/off state, run step (idle/preheat/heating/cooling), exhaust temp, ambient temp, voltage, error codes, gear level, target temp
- **No BLE pairing** — passkey is embedded in every command packet

### ESP32 BLE Implementation (Next Phase)

Once the protocol is known:

- **Role:** BLE central — scan, connect, discover services, read/subscribe to characteristics
- **BLE version:** ESP32-S supports BLE 4.2 (no BLE 5 extended features — hardware limitation of original ESP32)
- **Zephyr BLE stack:** Requires `CONFIG_BT=y`, `CONFIG_BT_CENTRAL=y`, `CONFIG_BT_GATT_CLIENT=y`
- **Binary blob:** Must fetch ESP32 BT blob first: `west blobs fetch hal_espressif`
- **Heap:** ESP32 BT driver reserves ~25.6KB heap; may need `CONFIG_HEAP_MEM_POOL_SIZE=65536`
- **Key constraint:** Must call `bt_le_scan_stop()` before `bt_conn_le_create()` — cannot scan and connect simultaneously
- **Listener callbacks:** Use `zbus_chan_const_msg()` pattern (NOT `zbus_chan_read()`) — same fix that resolved the override bug

### Known Bugs Fixed

- **zbus listener deadlock:** `zbus_chan_read()` inside a `ZBUS_LISTENER` callback deadlocks because the channel mutex is already held by `zbus_chan_pub()`. Fix: use `zbus_chan_const_msg(chan)` to access the message directly. This applies to ALL future listener callbacks including BLE data channels.

---

**Last Updated:** 2026-05-01
