# Auto-Damper - AI Assistant Guide

> **Note:** This file documents the Auto-Damper project specifically.
> Always read the [Thermoquad Organization CLAUDE.md](../../CLAUDE.md) first
> for organization-wide structure and conventions.

## Project Overview

**Auto-Damper** is a non-invasive BLE accessory for CC (Chinese Clone) diesel camping heaters. It communicates with heaters via BLE using protocols reverse-engineered from Dcloud apps, replacing spyware-laden CCP data collection apps with local-only control. It also routes the heater's hot air output between inside a tent and outside via a servo-driven diverter, based on duct temperature.

**Problem:** Well-insulated camping tent + heater auto mode = temperature swings from 17°C to 30°C. During startup/cooldown cycles, the heater blows undesirable air (cold/lukewarm) into the tent, then overshoots when running. The stock Dcloud BLE apps are spyware.

**Solution:** A two-way diverter routes heater output:
- **INSIDE** when duct temperature is high (heater producing useful heat)
- **OUTSIDE** during startup/cooldown (undesirable air vented out)

Plus full BLE heater control (power, temp, mode) and REST API — all local, no cloud.

**Hardware:**
- **Board:** Raspberry Pi Pico 2W (RP2350A + CYW43439 WiFi/BT)
- **Temperature Sensor:** MAX6675 K-type thermocouple at duct inlet (SPI0)
- **Actuator:** 270° servo (PWM, 500-2500µs pulse width)
- **Connectivity:** WiFi (HTTP REST API, mDNS) + BLE central (heater control)

**License:** Apache-2.0

**Status:** Ported from ESP32 to Pico 2W. BLE heater protocols (BYD + CC), WiFi, HTTP REST API, NVS config persistence, shell commands all implemented. Building and ready for hardware testing.

---

## Architecture

### Design Patterns

1. **Zbus Message Bus:** Command/data channel pairs for inter-component communication (same pattern as Helios)
2. **State Machine:** Zephyr SMF for damper routing decisions
3. **Zbus Listeners:** Callback-based observers (matching Helios pattern, not message subscribers)
4. **Shell Commands:** Runtime configuration via Zephyr shell over serial console
5. **Protocol Vtable:** `struct heater_protocol` with match/decode/encode methods for multi-protocol BLE support
6. **HTTP REST API:** JSON endpoints for remote control via WiFi

### Zbus Channels

| Channel | Message Type | Purpose |
|---------|-------------|---------|
| `temperature_data_chan` | `struct temperature_data` | Thermocouple readings (published every 500ms) |
| `damper_command_chan` | `struct damper_command` | Configuration and override commands |
| `damper_data_chan` | `struct damper_data` | Current damper state/route (published on changes) |
| `heater_data_chan` | `struct heater_data` | BLE heater telemetry (published on notify) |

### Threading Model

| Thread | Entry Point | Purpose | Rate | Priority |
|--------|-------------|---------|------|----------|
| `temperature_thread_id` | `temperature_thread()` | MAX6675 SPI reads | 500ms | 5 |
| `damper_thread_id` | `damper_thread()` | State machine loop | 250ms | 4 |
| `wifi_thread` | `wifi_thread()` | WiFi connect/reconnect | 1s | 7 |

### State Machine

| State | Purpose | Entry Action |
|-------|---------|-------------|
| IDLE | Startup, waiting for first reading | Route outside |
| ROUTING_INSIDE | Duct hot, air going into tent | Servo to inside position |
| ROUTING_OUTSIDE | Duct cool, air vented outside | Servo to outside position |

**Hysteresis:** Transitions use two thresholds (default 80°C high, 70°C low) to prevent flapping near the boundary.

**Override Mode:** Shell commands or REST API can force inside/outside regardless of temperature. Returns to auto mode with `damper override auto`.

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
damper ble scan [timeout]        # Scan for BLE heaters
damper ble stop                  # Stop scanning, show results
damper ble connect <index>       # Connect to scanned heater
damper ble disconnect            # Disconnect from heater
damper ble protocol <byd|cc|auto> # Force protocol or auto-detect
damper ble status                # Show heater telemetry
damper wifi save <ssid> <pw>     # Save WiFi credentials and connect
damper wifi status               # Show WiFi/IP status
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

### Pico 2W Pin Assignments

| GPIO | Function | Bus | Notes |
|------|----------|-----|-------|
| GP0 | UART0 TX | UART | Console/shell |
| GP1 | UART0 RX | UART | Console/shell |
| GP2 | Servo PWM | PWM 1A | 50Hz, 500–2500µs pulse |
| GP16 | MAX6675 SO (MISO) | SPI0 | |
| GP17 | MAX6675 CS | GPIO | Software chip select |
| GP18 | MAX6675 SCK | SPI0 | |
| GP19 | SPI0 TX (MOSI) | SPI0 | Not connected (MAX6675 is read-only) |
| GP23 | CYW43439 WL_REG_ON | — | WiFi/BT module power |
| GP24 | CYW43439 SPI DATA/IRQ | PIO0 | Shared bus select + host wake |
| GP25 | CYW43439 SPI CS | PIO0 | WiFi/BT chip select |
| GP29 | CYW43439 SPI CLK | PIO0 | WiFi/BT clock |

### Power

| Rail | Source | Consumers |
|------|--------|-----------|
| 3V3 | Pico 2W regulator | MAX6675 VCC, CYW43439 |
| VSYS (5V) | USB or external | Servo VCC |
| GND | Common | Servo, MAX6675 |

### USB Serial

- **Device:** `/dev/ttyACM0` (USB CDC)
- **Baud Rate:** 115200

---

## Build & Development

### Build Commands

```bash
# From apps/auto-damper/ directory, with venv activated:
source ../../.venv/bin/activate
export ZEPHYR_BASE="../../zephyr"
export ZEPHYR_SDK_INSTALL_DIR="../../tools/zephyr-sdk-1.0.1"

west build -b rpi_pico2/rp2350a/m33/w              # Build
west build -b rpi_pico2/rp2350a/m33/w -p always     # Clean rebuild
```

### Flashing

Drag-and-drop `build/zephyr/zephyr.uf2` via Pico 2W's USB bootloader (hold BOOTSEL while plugging in).

User has granted permission to flash — no safety-critical hardware is connected.

### Build Status

**Current:** Compiles and links for Pico 2W.
- Flash: 668KB / 4MB (15.94%)
- RAM: 188KB / 520KB (35.39%)
- UF2 output: 1.3MB

### Prerequisites

The Zephyr repo must be on branch `feat/bt-hci-cyw43-shared-bus` (contains the CYW43439 BT HCI shared-bus driver). Infineon blobs must be fetched: `west blobs fetch hal_infineon`.

---

## File Structure

```
auto-damper/
├── CLAUDE.md                    # This file
├── CMakeLists.txt              # Build configuration
├── prj.conf                    # Zephyr Kconfig
├── app.overlay                 # Device tree (RP2350 PWM, SPI0, MAX6675, storage partition)
├── sections-rom.ld             # Linker section for HTTP resources
├── Taskfile.dist.yml           # Task runner commands
├── dts/
│   └── bindings/
│       └── pwm-servo.yaml      # Servo DT binding (from Zephyr sample)
├── docs/
│   ├── byd-ble-protocol.md     # Reverse-engineered BYD BLE protocol
│   └── rest-api.md             # REST API design doc
├── web/
│   └── index.html              # Web UI (gzip-compressed into firmware)
├── include/auto_damper/
│   ├── damper.h                # Config struct, state enum, API
│   ├── zbus.h                  # Zbus message types and channel declarations
│   ├── config.h                # NVS storage API
│   ├── heater.h                # Heater protocol vtable and data types
│   ├── wifi.h                  # WiFi public API
│   └── wifi_config.h           # WiFi credentials NVS API
└── src/
    ├── main.c                  # Thread defs, servo/temp hardware
    ├── damper.c                # State machine, listeners, command handler
    ├── shell.c                 # Shell commands for runtime config
    ├── heater_ble.c            # BLE central: scan, connect, GATT, heartbeat
    ├── heater_byd.c            # BYD protocol: decode/encode (0xFFE0/0xFFE1)
    ├── heater_cc.c             # CC protocol: decode/encode (0xFFF0/0xFFF2)
    ├── http_api.c              # REST API endpoints (status, config, BLE, override)
    ├── config/
    │   ├── nvs_storage.c       # NVS flash storage backend
    │   └── wifi_config.c       # WiFi credentials persistence
    └── network/
        └── wifi.c              # WiFi thread, connect/reconnect, hostname
```

---

## BLE Heater Integration

### Supported Protocols

| Protocol | Manufacturer | Device Prefix | Service UUID | Characteristics | Auth |
|----------|-------------|---------------|-------------|-----------------|------|
| BYD | Booyood/Baiyide | `BYD-XXXX` | `0xFFE0` | `0xFFE1` (write+notify) | Passkey in packet |
| CC | Generic Chinese Clone | `CC-XXXX` | `0xFFF0` | `0xFFF2` (write), auto-notify | None |

### Protocol Vtable

New heater protocols are added by implementing `struct heater_protocol` (see `include/auto_damper/heater.h`):
- `match()` — identify device by BLE advertised name
- `decode()` — parse telemetry notifications
- `encode_ping/power/set_temp/set_mode()` — build command packets

### BLE Architecture

- **Auto-detect:** Scan results are matched against registered protocols
- **GATT discovery:** Finds service → discovers characteristics → subscribes to notify
- **Heartbeat:** Periodic ping keeps connection alive (protocol-specific interval)
- **Telemetry:** Decoded notifications published to `heater_data_chan` via zbus

### Known Bugs Fixed

- **zbus listener deadlock:** `zbus_chan_read()` inside a `ZBUS_LISTENER` callback deadlocks because the channel mutex is already held by `zbus_chan_pub()`. Fix: use `zbus_chan_const_msg(chan)` to access the message directly. This applies to ALL future listener callbacks including BLE data channels.

---

## REST API

See `docs/rest-api.md` for full specification. Key endpoints:

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/status` | Damper state, temperature, config |
| POST | `/api/config` | Set damper parameters |
| POST | `/api/override` | Override damper mode |
| GET | `/api/ble/status` | BLE connection + heater telemetry |
| POST | `/api/ble/scan` | Start BLE scan |
| GET | `/api/ble/devices` | List scan results |
| POST | `/api/ble/connect` | Connect to heater |
| POST | `/api/ble/disconnect` | Disconnect |
| POST | `/api/ble/power` | Heater on/off |
| POST | `/api/ble/temp` | Set target temperature |
| POST | `/api/ble/mode` | Set run mode (manual/automatic/fan) |
| POST | `/api/ble/power-level` | Set power level (1-10) |

---

## Key Decisions

- **Pico 2W over ESP32:** RP2350 has 520KB flat SRAM (vs ESP32's 233KB usable DRAM), 4MB flash with room for BT+WiFi+web UI+OTA, consistent with Thermoquad RP2350 ecosystem, RM2 module avoids FCC certs, Python-friendly for hackers
- **Zephyr RTOS:** Consistency with rest of Thermoquad ecosystem
- **Zbus listener callbacks over message subscribers:** Matches Helios pattern, simpler for this use case
- **SMF state machine:** Consistent with Helios, extensible if states are added later
- **Protocol vtable pattern:** Extensible multi-protocol BLE support without modifying core BLE code
- **CYW43439 shared-bus BT HCI:** Custom Zephyr driver (branch `feat/bt-hci-cyw43-shared-bus`) enables BT on Pico 2W — previously unsupported in Zephyr mainline

---

**Last Updated:** 2026-05-07
