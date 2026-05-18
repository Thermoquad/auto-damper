# Auto-Damper REST API

JSON REST API served on port 80 over WiFi STA.

All request/response bodies are `application/json`. Errors return `{"error":"message"}` with appropriate HTTP status.

---

## Damper

### GET /api/damper

Current damper state.

**Response:**

```json
{
  "mode": "auto",
  "angle": 90.0,
  "position": 0,
  "temperature": 85.3
}
```

- `mode` — `"auto"` or `"manual"`
- `angle` — current servo angle in degrees
- `position` — current position ID, or `null` if angle doesn't match any position
- `temperature` — duct thermocouple reading in Celsius

### PATCH /api/damper

Control the damper. Only one field per request.

**Move to position (enters manual mode):**

```json
{"position": 0}
```

**Move to raw angle (enters manual mode):**

```json
{"angle": 30.5}
```

**Set mode:**

```json
{"auto": true}
```

**Response:**

```json
{"ok": true}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 400 | Unknown position ID |
| 400 | Angle out of servo range |

---

## Positions

10 slots (ID 0-9). Each position has a label and servo angle. Stored in NVS.

### GET /api/damper/positions

List all configured positions.

**Response:**

```json
{
  "positions": [
    {"id": 0, "label": "inside", "angle": 0.0},
    {"id": 1, "label": "outside", "angle": 30.0}
  ]
}
```

Only returns active positions (omits empty slots).

### PUT /api/damper/positions/{id}

Create or update a position. `{id}` is 0-9.

**Request:**

```json
{"label": "inside", "angle": 90.0}
```

- `label` — max 15 characters
- `angle` — servo angle in degrees (0 to `servo_max_deg`)

**Response:**

```json
{"ok": true, "id": 0, "label": "inside", "angle": 90.0}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 400 | ID out of range (0-9) |
| 400 | Label too long |
| 400 | Angle out of servo range |

### DELETE /api/damper/positions/{id}

Remove a position. Fails if a target references this position.

**Response:**

```json
{"ok": true}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 400 | ID out of range |
| 409 | Position referenced by a target |

---

## Targets

10 slots (ID 0-9). Each target maps a temperature range to a position. When in auto mode, the damper moves to the position whose range contains the current duct temperature. If temperature falls in a gap between ranges, the damper holds its current position.

Overlapping ranges are rejected on write.

### GET /api/damper/targets

List all configured targets.

**Response:**

```json
{
  "targets": [
    {"id": 0, "range": [0.0, 70.0], "position": 1},
    {"id": 1, "range": [75.0, 200.0], "position": 0}
  ]
}
```

Only returns active targets.

### PUT /api/damper/targets/{id}

Create or update a target. `{id}` is 0-9.

**Request:**

```json
{"range": [70.0, 200.0], "position": 0}
```

- `range` — `[low, high]` in Celsius, `low < high`
- `position` — ID of an existing position (0-9)

**Response:**

```json
{"ok": true, "id": 0, "range": [70.0, 200.0], "position": 0}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 400 | ID out of range (0-9) |
| 400 | `low >= high` |
| 400 | Position ID doesn't exist |
| 409 | Range overlaps an existing target |

### DELETE /api/damper/targets/{id}

Remove a target.

**Response:**

```json
{"ok": true}
```

---

## Heaters

BLE heater discovery and control. Heaters are identified by their BLE advertised name (e.g. `Heater5587`, `BYD-E466E572ABBF`). The protocol is derived from the name automatically.

### POST /api/heaters/scan

Start a BLE scan for heaters.

**Request (optional):**

```json
{"timeout": 5}
```

- `timeout` — scan duration in seconds (1-30, default 5)

**Response:**

```json
{"ok": true, "timeout": 5}
```

### GET /api/heaters

List discovered heaters from the last scan.

**Response:**

```json
{
  "heaters": [
    {
      "name": "Heater5587",
      "addr": "EC:B1:B6:05:FE:AB",
      "rssi": -46,
      "protocol": "cc",
      "connected": false
    },
    {
      "name": "BYD-E466E572ABBF",
      "addr": "E4:66:E5:72:AB:BF",
      "rssi": -61,
      "protocol": "byd",
      "connected": false
    }
  ]
}
```

### GET /api/heaters/{name}

Get connection status and telemetry for a heater.

**Response (connected):**

```json
{
  "name": "Heater5587",
  "protocol": "cc",
  "connected": true,
  "telemetry": {
    "power": "RUNNING",
    "step": "IDLE",
    "mode": "fan",
    "core_temp": 22.0,
    "ambient_temp": 24.0,
    "voltage": 12.0,
    "target_temp": 0,
    "power_level": 0,
    "error": 0
  }
}
```

**Response (not connected):**

```json
{
  "name": "Heater5587",
  "protocol": "cc",
  "connected": false
}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 404 | Name not found in scan results |

### PUT /api/heaters/{name}

Connect to a heater by name.

**Response:**

```json
{"ok": true, "name": "Heater5587"}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 404 | Name not found in scan results |
| 409 | Already connected (to this or another heater) |

### DELETE /api/heaters/{name}

Disconnect from a heater.

**Response:**

```json
{"ok": true}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 400 | Not connected |

### PATCH /api/heaters/{name}

Send a control command to a connected heater. One field per request.

**Power:**

```json
{"power": true}
```

**Mode (CC: manual, automatic, fan):**

```json
{"mode": "fan"}
```

**Target temperature (8-36 C):**

```json
{"temp": 20}
```

**Power level (1-10, manual mode):**

```json
{"power_level": 5}
```

**Response:**

```json
{"ok": true}
```

**Errors:**

| Status | Condition |
|--------|-----------|
| 400 | Not connected to this heater |
| 400 | Invalid value (out of range, unknown mode) |
| 501 | Command not supported by this protocol |

---

## Servo Configuration

Low-level servo parameters. Stored in NVS.

### GET /api/damper/servo

**Response:**

```json
{
  "min_us": 500,
  "max_us": 2500,
  "max_deg": 270.0
}
```

### PATCH /api/damper/servo

**Request (any subset):**

```json
{"min_us": 500, "max_us": 2500, "max_deg": 270.0}
```

**Response:**

```json
{"ok": true}
```
