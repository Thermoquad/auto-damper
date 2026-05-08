# BYD Diesel Heater BLE Protocol

Reverse-engineered from `airHeaterByBLE.apk` (Booyood/Baiyide, version 2.0.0).

## Connection

| Parameter | Value |
|-----------|-------|
| Device name prefix | `BYD-` (OTA mode: `BYDOTA-`) |
| Service UUID | `0000FFE0-0000-1000-8000-00805F9B34FB` |
| Characteristic UUID | `0000FFE1-0000-1000-8000-00805F9B34FB` |
| Characteristic ops | Write + Notify (same UUID) |
| BLE pairing | None |
| Authentication | 4-digit passkey in every command (default: `1234`) |
| Heartbeat | CMD 1 every 2-3 seconds |
| MTU | 247 (Android only, iOS uses default) |

### Connection Flow

1. Scan for devices with name starting `BYD-`
2. Connect (10s timeout)
3. Set MTU to 247 (Android)
4. Discover service `FFE0`, characteristic `FFE1`
5. Enable notifications on `FFE1`
6. Send CMD 1 (ping) — authenticates passkey; device responds with telemetry
7. If no valid response within 5s, passkey is wrong
8. Continue sending CMD 1 every 2-3s as heartbeat

---

## Command Frame (Host -> Heater, 8 bytes)

```
Byte 0: 0xAA            (sync)
Byte 1: 0x55            (sync)
Byte 2: passkey / 100   (integer division, e.g. 1234 -> 0x0C)
Byte 3: passkey % 100   (e.g. 1234 -> 0x22)
Byte 4: cmd             (command number)
Byte 5: data & 0xFF     (data low byte)
Byte 6: data >> 8       (data high byte)
Byte 7: checksum        (byte2 + byte3 + byte4 + byte5 + byte6) & 0xFF
```

### Command Table

| CMD | Data | Description |
|-----|------|-------------|
| 1 | 0 | Ping / status request |
| 2 | 1-6 | Set run mode: 1=Manual, 2=Automatic, 3=Fan, 4=Plus, 5=One, 6=Temp (**requires onOff=2/RUNNING**) |
| 3 | 0/1 | Power OFF (0) / ON (1) |
| 4 | value | Set target temp (°C, range 8-36) or gear level (1-10) per mode |
| 10 | minutes | Sync clock (hours*60 + minutes since midnight) |
| 11 | minutes | Set timer start time (hours*60 + minutes) |
| 12 | minutes | Set timer duration |
| 13 | 0/1 | Disable (0) / Enable (1) scheduled timer |
| 14 | 0-4 | Voice language: 0=EN, 1=ZH, 2=RU, 3=Off, 4=DE |
| 15 | 0/1 | Temp unit: 0=Celsius, 1=Fahrenheit |
| 16 | value | Set fuel tank volume |
| 17 | value | Set fuel pump type (20=disable remote, 21=enable remote) |
| 18 | 0/1 | Auto start/stop: disable (0) / enable (1) |
| 19 | 0/1 | Altitude unit: 0=metric, 1=imperial |
| 20 | signed | Temperature compensation offset (signed 8-bit, degrees) |
| 21 | value | Display backlight brightness |

### Example Packets (passkey=1234)

| Purpose | Bytes |
|---------|-------|
| Ping | `AA 55 0C 22 01 00 00 2F` |
| Power ON | `AA 55 0C 22 03 01 00 32` |
| Power OFF | `AA 55 0C 22 03 00 00 31` |
| Set temp 20°C | `AA 55 0C 22 04 14 00 46` |

---

## Telemetry Frame (Heater -> Host)

### Protocol Detection

If `byte[0]==0xAA` and `byte[1]==0x55 or 0x66`: **V1 (plaintext)**
Otherwise: try XOR decrypt with key `"password"` and re-check header for **V2 (encrypted)**

### V2 XOR Decryption

XOR the entire buffer in 8-byte blocks with the static key:

```
Key: "password" = [0x70, 0x61, 0x73, 0x73, 0x77, 0x6F, 0x72, 0x64]
```

### V1 Telemetry (18 bytes, plaintext)

```
Byte  0:    0xAA           (sync)
Byte  1:    0x55 or 0x66   (0x55=normal, 0x66=fault)
Byte  2:    cmd echo       (last command received)
Byte  3:    onOff          (0=off, 1=starting, 2=running, 3=shutting down)
Byte  4:    errcode        (50=no error, when byte1=0x55)
Byte  5:    runStep        (0=idle, 1=self-check, 2=preheat, 3=heating, 4=cooling)
Bytes 6-7:  altitude       (byte6 + byte7*256) / 10 meters [little-endian]
Byte  8:    runM           (mode: 1=manual, 2=automatic, 3=fan)
Byte  9:    (mode-dependent: power-level in manual, target temp otherwise)
Byte 10:    (mode-dependent: N/A in manual, power-level+1 otherwise)
Bytes 11-12: voltage       (byte12*256 + byte11) / 10 volts [little-endian]
Bytes 13-14: exhaust temp  signed16(byte14*256 + byte13) °C [little-endian]
Bytes 15-16: ambient temp  signed16(byte16*256 + byte15) °C [little-endian]
Byte 17:    errcode        (when byte1=0x66)
```

### V2 Telemetry (40+ bytes, XOR encrypted)

After decryption:

```
Byte  0:    0xAA           (sync)
Byte  1:    0x55 or 0x66   (0x55=normal, 0x66=fault)
Byte  2:    cmd echo
Byte  3:    onOff          (0=off, 1=starting, 2=running, 3=shutting down)
Byte  4:    errcode        (50=no error, when byte1=0x55)
Byte  5:    runStep        (0=idle, 1=self-check, 2=preheat, 3=heating, 4=cooling)
Bytes 6-7:  altitude       (byte7 + byte6*256) / 10 meters [big-endian]
Byte  8:    (reserved)
Byte  9:    runT           (target temp, °C 8-36 or °F 40-99)
Byte 10:    runG           (gear level, 1-10)
Bytes 11-12: voltage       (byte11*256 + byte12) / 10 volts [big-endian]
Bytes 13-14: exhaust temp  signed16(byte13*256 + byte14) °C [big-endian]
Bytes 15-18: (reserved)
Bytes 19-20: device time   (byte19*256 + byte20) minutes since midnight
Bytes 21-22: auto-start    (byte21*256 + byte22) minutes since midnight
Bytes 23-24: timer dur.    (byte23*256 + byte24) minutes
Byte 25:    isAuto         (0=manual, 1=auto start/stop)
Byte 26:    language       (upper nibble: feature flag, lower: 0=EN 1=ZH 2=RU 3=Off 4=DE)
Byte 27:    temp unit      (upper nibble: feature flag, lower: 0=°C 1=°F)
Byte 28:    tank volume    (0xFF = not available)
Byte 29:    pump type      (0xFF = not available, >=20 = remote capable)
Byte 30:    altitude unit  (upper nibble: feature flag, lower: 0=metric 1=imperial)
Byte 31:    auto state
Bytes 32-33: ambient temp  signed16(byte32*256 + byte33) / 10 °C [big-endian]
Byte 34:    temp comp.     (signed 8-bit offset, degrees)
Byte 35:    errcode        (when byte1=0x66; 2=no fault)
Byte 36:    brightness     (display backlight, 0=not supported)
```

**Nibble byte decoding** (bytes 26, 27, 30):
- Feature nibble: `(byte >> 4) & 0x0F` — if `0xF`, feature not supported
- Value nibble: `byte & 0x0F`

---

## States

### onOff (byte 3)

| Value | State |
|-------|-------|
| 0 | Off |
| 1 | Starting |
| 2 | Running |
| 3 | Shutting down |

### runStep (byte 5)

| Value | Step |
|-------|------|
| 0 | Idle |
| 1 | Self-check |
| 2 | Preheating |
| 3 | Heating |
| 4 | Cooling |

### Error Codes

| Code | Meaning |
|------|---------|
| 50 | No error (V1, byte1=0x55) |
| 2 | No fault (V2, byte1=0x66) |
| E-1 | Startup failed |
| E-2 | Out of fuel |
| E-3 | Voltage anomaly |
| E-4 | Exhaust sensor anomaly |
| E-5 | Intake sensor anomaly |
| E-6 | Oil pump anomaly |
| E-7 | Fan anomaly |
| E-8 | Ignition plug anomaly |
| E-9 | Overheat protection |
| E-10 | Heater temp sensor anomaly |
| E-11 | CO over limit |
| E-50 | Low battery |

---

## OTA Service

| Parameter | Value |
|-----------|-------|
| Service UUID | `0000FEE0-0000-1000-8000-00805F9B34FB` |
| Characteristic UUID | `0000FEE1-0000-1000-8000-00805F9B34FB` |
| Start OTA | Send `[0x84, 0x00, ...]` (20 bytes) |
| End OTA | Send `[0x83, 0x00, ...]` (20 bytes) |
| Device name in OTA mode | `BYDOTA-` prefix |

---

## Command Constraints

### Mode Switching (CMD 2) Requires RUNNING State

The app only sends CMD 2 (set run mode) when `onOff == 2` (RUNNING). If `onOff` is 0
(off), 1 (starting), or 3 (shutting down), the app shows an "isOff" error and blocks the
command. Source: `heat/index.vue:1697` in decompiled APK.

**Correct fan mode activation sequence:**

1. Send CMD 3 data=1 (power on)
2. Poll telemetry until `onOff == 2` (byte 3 transitions: 0→1→2)
3. Send CMD 2 data=3 (switch to fan mode)

Sending the mode command during STARTING (onOff=1) will be ignored by the heater.

### Mode-Dependent Telemetry (V1 bytes 8-10)

Bytes 9-10 change meaning based on the current mode (byte 8), and mode is only valid when
the heater is on (`onOff != 0`):

| Mode (byte 8) | Byte 9 | Byte 10 |
|---------------|--------|---------|
| 1 (manual) | power level (1-10) | N/A |
| 2 (automatic) | target temp (°C/°F) | power level + 1 |
| 3 (fan) | target temp (°C/°F) | power level + 1 |
| heater off | target temp | power level |

---

## Notes

- V1 uses little-endian for multi-byte fields; V2 uses big-endian
- The XOR "encryption" in V2 is trivial — static key, no IV, no authentication
- Commands are always sent unencrypted regardless of protocol version
- The app reconnects up to 5 times automatically on disconnect
