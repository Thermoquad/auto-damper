# CC Diesel Heater BLE Protocol

Reverse-engineered from `AirHeaterCC.apk` (package: `com.chchi.app.zcjr`, version 1.0.1).

## Connection

| Parameter | Value |
|-----------|-------|
| Device name filter | Contains `"Heater"` (also `BT_` or `@` prefix patterns) |
| Service UUID | `0000FFF0-0000-1000-8000-00805F9B34FB` |
| Write Characteristic | `0000FFF2-0000-1000-8000-00805F9B34FB` |
| Notify Characteristic | Auto-discovered (any characteristic with notify property on FFF0) |
| BLE pairing | None |
| Authentication | None — no passkey |
| Heartbeat | `BA AB 04 CC 00 00 00 35` every 2 seconds |
| MTU | 247 (Android only) |

### Connection Flow

1. Scan for devices with name containing `"Heater"`
2. Connect
3. Set MTU to 247 (Android)
4. Discover service `FFF0`, find write characteristic `FFF2`, enable notify on all notify characteristics
5. After 1 second: sync clock (send current time)
6. Send heartbeat every 2 seconds — device stops responding after 5s without heartbeat

---

## Command Frame (Host -> Heater, variable length)

```
Byte 0:   0xBA           (magic)
Byte 1:   0xAB           (magic)
Byte 2:   LEN            (bytes from CMD to end of data, inclusive)
Byte 3:   CMD            (command byte)
Bytes 4+: DATA           (0 or more data bytes)
Last:     CHECKSUM       (sum of all preceding bytes) & 0xFF
```

Response frames use reversed magic: `0xAB 0xBA`.

### Command Table

| Hex Payload (before checksum) | Full Packet | Description |
|-------------------------------|-------------|-------------|
| `BA AB 04 CC 00 00 00` | `...35` | Heartbeat / poll |
| `BA AB 04 BB A1 00 00` | `...C5` | Start heating mode |
| `BA AB 04 BB A4 00 00` | `...C8` | Start blowing (ventilation) mode |
| `BA AB 04 BB A2 00 00` | `...C6` | Temp/gear increase |
| `BA AB 04 BB A3 00 00` | `...C7` | Temp/gear decrease |
| `BA AB 04 BB AC 00 00` | `...D0` | Switch to manual mode |
| `BA AB 04 BB AD 00 00` | `...D1` | Switch to constant-temp mode |
| `BA AB 04 BB A5 00 00` | `...C9` | Enable high-altitude mode |
| `BA AB 04 BB A6 00 00` | `...CA` | Toggle auto start/stop |
| `BA AB 04 BB A7 00 00` | `...CB` | Set unit: Celsius |
| `BA AB 04 BB A8 00 00` | `...CC` | Set unit: Fahrenheit |
| `BA AB 04 BB A9 00 00` | `...CD` | Set altitude unit: meters |
| `BA AB 04 BB AA 00 00` | `...CE` | Set altitude unit: feet |
| `BA AB 04 EC 00 00 00` | `...55` | Get timing schedule |
| `BA AB 04 DC 00 00 00` | `...45` | Query auto start/stop config |
| `BA AB 05 EA HH MM SS WW` | variable | Sync clock (hour, min, sec, day-of-week) |
| `BA AB 04 DA ST SP UN` | variable | Set auto start/stop temps |
| `BA AB 24 ED` + 35 bytes | variable | Save 7-day timing schedule |

**Two-step write pattern:** After every command, send the heartbeat again 500ms later, then wait another 500ms.

---

## Telemetry Frame (Heater -> Host, 19 bytes)

Prefix: `AB BA 11 CC`

```
Byte  0:  0xAB           (magic)
Byte  1:  0xBA           (magic)
Byte  2:  0x11           (length = 17)
Byte  3:  0xCC           (command echo)
Byte  4:  status
Byte  5:  mode / fault indicator
Byte  6:  gear or setpoint value
Byte  7:  (reserved)
Byte  8:  auto start/stop enabled (0x01 = yes)
Byte  9:  voltage (raw byte)
Byte 10:  temperature unit (0x00 = °C, 0x01 = °F)
Byte 11:  ambient temperature (raw)
Byte 12:  exhaust/body temp high byte
Byte 13:  exhaust/body temp low byte
Byte 14:  altitude unit (0x00 = meters, 0x01 = feet)
Byte 15:  high-altitude mode (0x01 = enabled)
Byte 16:  altitude high byte
Byte 17:  altitude low byte
Byte 18:  checksum
```

### Temperature Decoding

- **Ambient (°C):** `byte[11] - 30` (range: -30°C to 225°C)
- **Ambient (°F):** `byte[11] - 22`
- **Exhaust/body:** `(byte[12] << 8) | byte[13]` as uint16

### Status (byte 4)

| Value | State |
|-------|-------|
| 0x00 | Off / shutdown |
| 0x01 | Heating |
| 0x02 | Cooling down |
| 0x04 | Blowing (ventilation) |
| 0x06 | Paused |

### Mode (byte 5)

| Value | Mode |
|-------|------|
| 0x00 | Manual |
| 0x01 | Constant temperature |
| 0xFF | Fault — byte 6 contains fault code |

### Fault Codes (when byte 5 = 0xFF)

| Code | Meaning |
|------|---------|
| 2 | Voltage fault |
| 3 | Glow plug fault |
| 4 | Fuel pump fault |
| 5 | High temperature alarm (intake >50°C or case >230°C) |
| 6 | Fan/motor fault |
| 7 | Communication fault |
| 8 | Flame-out |
| 9 | Temperature sensor fault |
| 10 | Failed to start |
| 11 | CO alarm |

---

## Timing Schedule Response (40 bytes)

Prefix: `AB BA 24 EC`

7 days x 5 bytes, starting at byte 4:

```
For day i (0=Sunday ... 6=Saturday):
  byte[i*5 + 4] = boot hour
  byte[i*5 + 5] = boot minute
  byte[i*5 + 6] = shutdown hour
  byte[i*5 + 7] = shutdown minute
  byte[i*5 + 8] = flags (bit7 = enabled, bit0 = repeat)
```

---

## Auto Start/Stop Config Response (8 bytes)

Prefix: `AB BA 04 DC`

```
Byte 0: 0xAB           (magic)
Byte 1: 0xBA           (magic)
Byte 2: 0x04           (length)
Byte 3: 0xDC           (command echo)
Byte 4: startup offset temperature (3-10 °C, 5-18 °F)
Byte 5: shutdown offset temperature (3-10 °C, 5-18 °F)
Byte 6: unit (0x00 = °C, 0x01 = °F)
Byte 7: checksum
```

Offsets are relative to the target temperature setpoint. The heater starts heating when ambient drops `startup_offset` degrees below target, and stops when it rises `shutdown_offset` degrees above target. Query with `0xDC`; the heater does **not** echo a response after receiving `0xDA` (set). The `0xCC` telemetry frame does not include offset values — only the auto-enabled flag (byte 8).

---

## Protocol Comparison: CC vs BYD

| Feature | BYD | CC |
|---------|-----|-----|
| Service UUID | `FFE0` | `FFF0` |
| Write Characteristic | `FFE1` | `FFF2` |
| Command magic | `AA 55` | `BA AB` |
| Response magic | `AA 55/66` | `AB BA` |
| Command length | Fixed 8 bytes | Variable |
| Passkey | Yes (default 1234) | None |
| Telemetry encryption | V2: XOR with "password" | None |
| Ambient temp encoding | Direct signed16 | Raw byte - 30 |
| Exhaust temp encoding | Signed16 little-endian (V1) / big-endian (V2) | Uint16 big-endian |
| Heartbeat interval | 2-3 seconds | 2 seconds |
| Clock sync | CMD 10 | CMD EA with HH MM SS WW |
| Scheduling | Not supported | 7-day schedule |
