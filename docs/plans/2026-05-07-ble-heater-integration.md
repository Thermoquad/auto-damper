---
type: plan
step: "1"
title: "BLE Heater Integration with Universal Protocol Interface"
status: pending
assessment_status: needed
provenance:
  source: roadmap
  issue_id: null
  roadmap_step: "1"
dates:
  created: "2026-05-07"
  approved: null
  completed: null
related_plans: []
---

# Step 1: BLE Heater Integration

## Overview

Add BLE central capability to the auto-damper firmware with a universal heater
interface supporting multiple protocols (BYD and CC) with runtime switching.
This step delivers scan, connect, telemetry decoding, and shell commands for
interactive BLE control. Damper state machine integration with heater data is
deferred to Step 2.

## Prerequisites

- Core firmware working (temperature, servo, state machine, shell) — **done**
- BLE protocol docs for BYD and CC — **done** (`docs/byd-ble-protocol.md`, `docs/cc-ble-protocol.md`)
- ESP32 BT binary blob fetched:

```bash
cd ../../  # Thermoquad root
source .venv/bin/activate
export ZEPHYR_BASE="zephyr"
west blobs fetch hal_espressif
```

---

## Task 1: Heater Interface Types and Protocol Ops

**Files:**
- Create: `include/auto_damper/heater.h`
- Modify: `include/auto_damper/zbus.h`

### Step 1: Create heater.h

```c
// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_HEATER_H
#define AUTO_DAMPER_HEATER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//////////////////////////////////////////////////////////////
// Universal Heater Types
//////////////////////////////////////////////////////////////

enum heater_power_state {
  HEATER_POWER_OFF,
  HEATER_POWER_STARTING,
  HEATER_POWER_RUNNING,
  HEATER_POWER_SHUTTING_DOWN,
};

enum heater_run_step {
  HEATER_STEP_IDLE,
  HEATER_STEP_SELF_CHECK,
  HEATER_STEP_PREHEAT,
  HEATER_STEP_HEATING,
  HEATER_STEP_COOLING,
};

struct heater_data {
  enum heater_power_state power;
  enum heater_run_step step;
  double exhaust_temp_c;
  double ambient_temp_c;
  double voltage;
  int error_code;
  int target_temp;
  int gear_level;
  bool connected;
  int64_t timestamp_us;
};

//////////////////////////////////////////////////////////////
// Protocol Driver Interface
//////////////////////////////////////////////////////////////

struct heater_protocol {
  const char *name;

  bool (*match)(const char *device_name);

  uint16_t service_uuid;
  uint16_t write_char_uuid;
  uint16_t notify_char_uuid; // 0 = auto-discover first notify characteristic

  uint32_t heartbeat_ms;

  int (*decode)(const uint8_t *buf, size_t len, struct heater_data *data);
  int (*encode_ping)(uint8_t *buf, size_t len);
  int (*encode_power)(uint8_t *buf, size_t len, bool on);
  int (*encode_set_temp)(uint8_t *buf, size_t len, int temp_c);
};

//////////////////////////////////////////////////////////////
// Protocol Registry
//////////////////////////////////////////////////////////////

extern const struct heater_protocol heater_protocol_byd;
extern const struct heater_protocol heater_protocol_cc;

const char *heater_power_state_str(enum heater_power_state s);
const char *heater_run_step_str(enum heater_run_step s);

#endif
```

### Step 2: Add heater channel to zbus.h

Add `#include <auto_damper/heater.h>` at the top of zbus.h (after the zbus
include), then append before the `#endif`:

```c
//////////////////////////////////////////////////////////////
// Heater Data Channel
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DECLARE(heater_data_chan);
```

---

## Task 2: BYD Protocol Driver

**Files:**
- Create: `src/heater_byd.c`

**Depends on:** Task 1

### Step 1: Create heater_byd.c

```c
// SPDX-License-Identifier: Apache-2.0

#include <string.h>
#include <zephyr/kernel.h>

#include <auto_damper/heater.h>

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

static uint16_t passkey = 1234;

void heater_byd_set_passkey(uint16_t key)
{
  passkey = key;
}

//////////////////////////////////////////////////////////////
// Match
//////////////////////////////////////////////////////////////

static bool byd_match(const char *name)
{
  return strncmp(name, "BYD-", 4) == 0;
}

//////////////////////////////////////////////////////////////
// XOR Decryption (V2)
//////////////////////////////////////////////////////////////

static void xor_decrypt(uint8_t *buf, size_t len)
{
  static const uint8_t key[] = "password";

  for (size_t i = 0; i < len; i++) {
    buf[i] ^= key[i % 8];
  }
}

//////////////////////////////////////////////////////////////
// Decode
//////////////////////////////////////////////////////////////

static void decode_power(uint8_t val, struct heater_data *data)
{
  switch (val) {
  case 1:
    data->power = HEATER_POWER_STARTING;
    break;
  case 2:
    data->power = HEATER_POWER_RUNNING;
    break;
  case 3:
    data->power = HEATER_POWER_SHUTTING_DOWN;
    break;
  default:
    data->power = HEATER_POWER_OFF;
    break;
  }
}

static void decode_step(uint8_t val, struct heater_data *data)
{
  switch (val) {
  case 1:
    data->step = HEATER_STEP_SELF_CHECK;
    break;
  case 2:
    data->step = HEATER_STEP_PREHEAT;
    break;
  case 3:
    data->step = HEATER_STEP_HEATING;
    break;
  case 4:
    data->step = HEATER_STEP_COOLING;
    break;
  default:
    data->step = HEATER_STEP_IDLE;
    break;
  }
}

static int byd_decode(const uint8_t *buf, size_t len, struct heater_data *data)
{
  if (len < 2) {
    return -EINVAL;
  }

  uint8_t tmp[48];
  bool v2 = false;

  if (buf[0] == 0xAA && (buf[1] == 0x55 || buf[1] == 0x66)) {
    if (len < 18) {
      return -EINVAL;
    }
    memcpy(tmp, buf, MIN(len, sizeof(tmp)));
  } else {
    if (len > sizeof(tmp)) {
      return -EINVAL;
    }
    memcpy(tmp, buf, len);
    xor_decrypt(tmp, len);
    if (tmp[0] != 0xAA || (tmp[1] != 0x55 && tmp[1] != 0x66)) {
      return -EPROTO;
    }
    v2 = true;
  }

  decode_power(tmp[3], data);
  decode_step(tmp[5], data);

  if (!v2) {
    data->voltage = (double)((tmp[12] << 8) | tmp[11]) / 10.0;
    data->exhaust_temp_c = (double)(int16_t)((tmp[14] << 8) | tmp[13]);
    data->ambient_temp_c = (double)(int16_t)((tmp[16] << 8) | tmp[15]);
    data->error_code = (tmp[1] == 0x66) ? tmp[17] : tmp[4];
    data->target_temp = tmp[9];
    data->gear_level = tmp[10];
  } else if (len >= 36) {
    data->voltage = (double)((tmp[11] << 8) | tmp[12]) / 10.0;
    data->exhaust_temp_c = (double)(int16_t)((tmp[13] << 8) | tmp[14]);
    data->ambient_temp_c = (double)(int16_t)((tmp[32] << 8) | tmp[33]) / 10.0;
    data->error_code = (tmp[1] == 0x66) ? tmp[35] : tmp[4];
    data->target_temp = tmp[9];
    data->gear_level = tmp[10];
  }

  data->connected = true;
  return 0;
}

//////////////////////////////////////////////////////////////
// Encode Helpers
//////////////////////////////////////////////////////////////

static void byd_frame(uint8_t *buf, uint8_t cmd, uint8_t data_lo, uint8_t data_hi)
{
  buf[0] = 0xAA;
  buf[1] = 0x55;
  buf[2] = passkey / 100;
  buf[3] = passkey % 100;
  buf[4] = cmd;
  buf[5] = data_lo;
  buf[6] = data_hi;
  buf[7] = (buf[2] + buf[3] + buf[4] + buf[5] + buf[6]) & 0xFF;
}

static int byd_encode_ping(uint8_t *buf, size_t len)
{
  if (len < 8) {
    return -ENOMEM;
  }
  byd_frame(buf, 0x01, 0x00, 0x00);
  return 8;
}

static int byd_encode_power(uint8_t *buf, size_t len, bool on)
{
  if (len < 8) {
    return -ENOMEM;
  }
  byd_frame(buf, 0x03, on ? 0x01 : 0x00, 0x00);
  return 8;
}

static int byd_encode_set_temp(uint8_t *buf, size_t len, int temp_c)
{
  if (len < 8) {
    return -ENOMEM;
  }
  byd_frame(buf, 0x04, temp_c & 0xFF, (temp_c >> 8) & 0xFF);
  return 8;
}

//////////////////////////////////////////////////////////////
// Protocol Definition
//////////////////////////////////////////////////////////////

const struct heater_protocol heater_protocol_byd = {
    .name = "byd",
    .match = byd_match,
    .service_uuid = 0xFFE0,
    .write_char_uuid = 0xFFE1,
    .notify_char_uuid = 0xFFE1,
    .heartbeat_ms = 2500,
    .decode = byd_decode,
    .encode_ping = byd_encode_ping,
    .encode_power = byd_encode_power,
    .encode_set_temp = byd_encode_set_temp,
};
```

---

## Task 3: CC Protocol Driver

**Files:**
- Create: `src/heater_cc.c`

**Depends on:** Task 1

### Step 1: Create heater_cc.c

```c
// SPDX-License-Identifier: Apache-2.0

#include <string.h>
#include <zephyr/kernel.h>

#include <auto_damper/heater.h>

//////////////////////////////////////////////////////////////
// Match
//////////////////////////////////////////////////////////////

static bool cc_match(const char *name)
{
  return strstr(name, "Heater") != NULL;
}

//////////////////////////////////////////////////////////////
// Checksum
//////////////////////////////////////////////////////////////

static uint8_t cc_checksum(const uint8_t *buf, size_t len)
{
  uint8_t sum = 0;

  for (size_t i = 0; i < len; i++) {
    sum += buf[i];
  }
  return sum;
}

//////////////////////////////////////////////////////////////
// Decode
//////////////////////////////////////////////////////////////

static int cc_decode(const uint8_t *buf, size_t len, struct heater_data *data)
{
  if (len < 19) {
    return -EINVAL;
  }
  if (buf[0] != 0xAB || buf[1] != 0xBA) {
    return -EPROTO;
  }
  if (buf[3] != 0xCC) {
    return -EPROTO;
  }

  switch (buf[4]) {
  case 0x01:
    data->power = HEATER_POWER_RUNNING;
    data->step = HEATER_STEP_HEATING;
    break;
  case 0x02:
    data->power = HEATER_POWER_SHUTTING_DOWN;
    data->step = HEATER_STEP_COOLING;
    break;
  case 0x04:
    data->power = HEATER_POWER_RUNNING;
    data->step = HEATER_STEP_IDLE;
    break;
  case 0x06:
    data->power = HEATER_POWER_RUNNING;
    data->step = HEATER_STEP_IDLE;
    break;
  default:
    data->power = HEATER_POWER_OFF;
    data->step = HEATER_STEP_IDLE;
    break;
  }

  if (buf[5] == 0xFF) {
    data->error_code = buf[6];
    data->gear_level = 0;
  } else {
    data->error_code = 0;
    data->gear_level = buf[6];
  }

  data->target_temp = (buf[5] != 0xFF) ? buf[6] : 0;
  data->voltage = (double)buf[9];
  data->ambient_temp_c = (double)buf[11] - 30.0;
  data->exhaust_temp_c = (double)((buf[12] << 8) | buf[13]);
  data->connected = true;

  return 0;
}

//////////////////////////////////////////////////////////////
// Encode
//////////////////////////////////////////////////////////////

static int cc_encode_ping(uint8_t *buf, size_t len)
{
  if (len < 8) {
    return -ENOMEM;
  }
  uint8_t cmd[] = {0xBA, 0xAB, 0x04, 0xCC, 0x00, 0x00, 0x00};

  memcpy(buf, cmd, 7);
  buf[7] = cc_checksum(buf, 7);
  return 8;
}

static int cc_encode_power(uint8_t *buf, size_t len, bool on)
{
  if (len < 8) {
    return -ENOMEM;
  }
  if (!on) {
    return -ENOTSUP;
  }

  uint8_t cmd[] = {0xBA, 0xAB, 0x04, 0xBB, 0xA1, 0x00, 0x00};

  memcpy(buf, cmd, 7);
  buf[7] = cc_checksum(buf, 7);
  return 8;
}

static int cc_encode_set_temp(uint8_t *buf, size_t len, int temp_c)
{
  (void)buf;
  (void)len;
  (void)temp_c;
  return -ENOTSUP;
}

//////////////////////////////////////////////////////////////
// Protocol Definition
//////////////////////////////////////////////////////////////

const struct heater_protocol heater_protocol_cc = {
    .name = "cc",
    .match = cc_match,
    .service_uuid = 0xFFF0,
    .write_char_uuid = 0xFFF2,
    .notify_char_uuid = 0,
    .heartbeat_ms = 2000,
    .decode = cc_decode,
    .encode_ping = cc_encode_ping,
    .encode_power = cc_encode_power,
    .encode_set_temp = cc_encode_set_temp,
};
```

### Step 2: Commit

`feat(auto-damper): add universal heater protocol interface with BYD and CC drivers`

---

## Task 4: Enable BLE in Build

**Files:**
- Modify: `prj.conf`

**Depends on:** Blob fetch prerequisite (see Prerequisites)

### Step 1: Add BLE config to prj.conf

Append after the existing zbus section:

```ini
# Bluetooth
CONFIG_BT=y
CONFIG_BT_CENTRAL=y
CONFIG_BT_GATT_CLIENT=y
CONFIG_BT_GATT_AUTO_DISCOVER_CCC=y
CONFIG_BT_DEVICE_NAME="auto-damper"
CONFIG_BT_MAX_CONN=1
CONFIG_BT_LOG_LEVEL_INF=y

# Increase heap for BLE stack (~25.6KB BT + margin)
CONFIG_HEAP_MEM_POOL_SIZE=65536
```

**Note:** Replace the existing `CONFIG_HEAP_MEM_POOL_SIZE=4096` line.

### Step 2: Verify build compiles with BLE enabled

Run: `task build-firmware` (or `west build -b esp32_devkitc/esp32/procpu -p always`)

Expected: Build succeeds. If blob not fetched, error will reference `libbtdm_app.a`.

---

## Task 5: BLE Connection Manager

**Files:**
- Create: `src/heater_ble.c`

**Depends on:** Tasks 1-4

### Step 1: Create heater_ble.c

```c
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/heater.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(heater_ble, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Protocol Registry
//////////////////////////////////////////////////////////////

static const struct heater_protocol *protocols[] = {
    &heater_protocol_byd,
    &heater_protocol_cc,
};

#define NUM_PROTOCOLS ARRAY_SIZE(protocols)

//////////////////////////////////////////////////////////////
// Scan Results
//////////////////////////////////////////////////////////////

#define MAX_SCAN_RESULTS 8

struct ble_scan_result {
  bt_addr_le_t addr;
  char name[32];
  int8_t rssi;
  const struct heater_protocol *protocol;
};

static struct ble_scan_result scan_results[MAX_SCAN_RESULTS];
static int scan_count;
static bool scanning;

//////////////////////////////////////////////////////////////
// Connection State
//////////////////////////////////////////////////////////////

static struct bt_conn *heater_conn;
static const struct heater_protocol *active_protocol;
static const struct heater_protocol *forced_protocol;

static uint16_t write_handle;
static uint16_t notify_handle;
static uint16_t svc_start_handle;
static uint16_t svc_end_handle;

static struct bt_gatt_discover_params disc_params;
static struct bt_uuid_16 disc_uuid;
static struct bt_gatt_subscribe_params sub_params;
static struct bt_gatt_discover_params ccc_disc_params;

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static void start_discovery(void);
static void subscribe_notify(void);
static void heartbeat_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_handler);

//////////////////////////////////////////////////////////////
// Zbus Channel
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DEFINE(heater_data_chan, struct heater_data, NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.power = HEATER_POWER_OFF,
                              .step = HEATER_STEP_IDLE, .connected = false,
                              .timestamp_us = 0));

//////////////////////////////////////////////////////////////
// Heartbeat
//////////////////////////////////////////////////////////////

static void heartbeat_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  if (!heater_conn || !active_protocol || write_handle == 0) {
    return;
  }

  uint8_t buf[16];
  int pkt_len = active_protocol->encode_ping(buf, sizeof(buf));

  if (pkt_len > 0) {
    bt_gatt_write_without_response(heater_conn, write_handle, buf, pkt_len,
                                   false);
  }

  k_work_schedule(&heartbeat_work, K_MSEC(active_protocol->heartbeat_ms));
}

static void start_heartbeat(void)
{
  if (active_protocol) {
    k_work_schedule(&heartbeat_work, K_MSEC(100));
  }
}

static void stop_heartbeat(void)
{
  k_work_cancel_delayable(&heartbeat_work);
}

//////////////////////////////////////////////////////////////
// Notify Callback
//////////////////////////////////////////////////////////////

static uint8_t notify_cb(struct bt_conn *conn,
                         struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length)
{
  if (!data) {
    LOG_WRN("Unsubscribed");
    return BT_GATT_ITER_STOP;
  }

  if (!active_protocol) {
    return BT_GATT_ITER_CONTINUE;
  }

  struct heater_data hdata = {0};
  int ret = active_protocol->decode(data, length, &hdata);

  if (ret == 0) {
    hdata.timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks());
    zbus_chan_pub(&heater_data_chan, &hdata, K_MSEC(100));
  } else {
    LOG_DBG("Decode failed: %d (len=%u)", ret, length);
  }

  return BT_GATT_ITER_CONTINUE;
}

//////////////////////////////////////////////////////////////
// GATT Discovery
//////////////////////////////////////////////////////////////

static uint8_t discover_cb(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
  if (!attr) {
    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
      if (write_handle != 0 && notify_handle != 0) {
        LOG_INF("Discovery complete: write=%u notify=%u", write_handle,
                notify_handle);
        subscribe_notify();
      } else {
        LOG_ERR("Missing handles: write=%u notify=%u", write_handle,
                notify_handle);
      }
    } else {
      LOG_ERR("Service not found");
    }
    return BT_GATT_ITER_STOP;
  }

  if (params->type == BT_GATT_DISCOVER_PRIMARY) {
    struct bt_gatt_service_val *svc = attr->user_data;

    svc_start_handle = attr->handle + 1;
    svc_end_handle = svc->end_handle;
    LOG_INF("Service found: handles %u-%u", svc_start_handle,
            svc_end_handle);

    params->uuid = NULL;
    params->start_handle = svc_start_handle;
    params->end_handle = svc_end_handle;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, params);

    if (err) {
      LOG_ERR("Char discovery failed: %d", err);
    }
    return BT_GATT_ITER_STOP;
  }

  if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
    struct bt_gatt_chrc *chrc = attr->user_data;

    if (chrc->uuid->type != BT_UUID_TYPE_16) {
      return BT_GATT_ITER_CONTINUE;
    }

    uint16_t uuid_val = BT_UUID_16(chrc->uuid)->val;
    bool has_notify = (chrc->properties & BT_GATT_CHRC_NOTIFY) != 0;

    if (uuid_val == active_protocol->write_char_uuid) {
      write_handle = chrc->value_handle;
      LOG_INF("Write char 0x%04x: handle=%u", uuid_val, write_handle);
    }

    if (active_protocol->notify_char_uuid != 0) {
      if (uuid_val == active_protocol->notify_char_uuid && has_notify) {
        notify_handle = chrc->value_handle;
        LOG_INF("Notify char 0x%04x: handle=%u", uuid_val, notify_handle);
      }
    } else if (has_notify && notify_handle == 0) {
      notify_handle = chrc->value_handle;
      LOG_INF("Notify char (auto) 0x%04x: handle=%u", uuid_val,
              notify_handle);
    }

    return BT_GATT_ITER_CONTINUE;
  }

  return BT_GATT_ITER_STOP;
}

static void start_discovery(void)
{
  write_handle = 0;
  notify_handle = 0;

  disc_uuid.uuid.type = BT_UUID_TYPE_16;
  disc_uuid.val = active_protocol->service_uuid;

  disc_params.uuid = &disc_uuid.uuid;
  disc_params.func = discover_cb;
  disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  disc_params.type = BT_GATT_DISCOVER_PRIMARY;

  int err = bt_gatt_discover(heater_conn, &disc_params);

  if (err) {
    LOG_ERR("Discovery start failed: %d", err);
  }
}

static void subscribe_notify(void)
{
  sub_params.notify = notify_cb;
  sub_params.value = BT_GATT_CCC_NOTIFY;
  sub_params.value_handle = notify_handle;
  sub_params.ccc_handle = 0;
#if defined(CONFIG_BT_GATT_AUTO_DISCOVER_CCC)
  sub_params.end_handle = svc_end_handle;
  sub_params.disc_params = &ccc_disc_params;
#endif

  int err = bt_gatt_subscribe(heater_conn, &sub_params);

  if (err && err != -EALREADY) {
    LOG_ERR("Subscribe failed: %d", err);
    return;
  }

  LOG_INF("Subscribed to notifications");
  start_heartbeat();
}

//////////////////////////////////////////////////////////////
// Connection Callbacks
//////////////////////////////////////////////////////////////

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
  if (conn != heater_conn) {
    return;
  }

  if (err) {
    LOG_ERR("Connect failed: %u", err);
    bt_conn_unref(heater_conn);
    heater_conn = NULL;
    return;
  }

  LOG_INF("Connected to heater (%s protocol)", active_protocol->name);
  start_discovery();
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
  if (conn != heater_conn) {
    return;
  }

  LOG_INF("Disconnected: reason %u", reason);
  stop_heartbeat();
  bt_conn_unref(heater_conn);
  heater_conn = NULL;
  write_handle = 0;
  notify_handle = 0;

  struct heater_data hdata = {.connected = false};

  zbus_chan_pub(&heater_data_chan, &hdata, K_MSEC(100));
}

BT_CONN_CB_DEFINE(heater_conn_cbs) = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
};

//////////////////////////////////////////////////////////////
// Scan
//////////////////////////////////////////////////////////////

static bool parse_name(struct bt_data *data, void *user_data)
{
  char *name = user_data;

  if (data->type == BT_DATA_NAME_COMPLETE ||
      data->type == BT_DATA_NAME_SHORTENED) {
    size_t copy_len = MIN(data->data_len, 31);
    memcpy(name, data->data, copy_len);
    name[copy_len] = '\0';
    return false;
  }
  return true;
}

static const struct heater_protocol *detect_protocol(const char *name)
{
  if (forced_protocol) {
    return forced_protocol;
  }

  for (int i = 0; i < NUM_PROTOCOLS; i++) {
    if (protocols[i]->match(name)) {
      return protocols[i];
    }
  }
  return NULL;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                    struct net_buf_simple *ad)
{
  char name[32] = {0};

  bt_data_parse(ad, parse_name, name);
  if (name[0] == '\0') {
    return;
  }

  const struct heater_protocol *proto = detect_protocol(name);

  if (!proto && !forced_protocol) {
    return;
  }

  for (int i = 0; i < scan_count; i++) {
    if (bt_addr_le_cmp(&scan_results[i].addr, addr) == 0) {
      return;
    }
  }

  if (scan_count >= MAX_SCAN_RESULTS) {
    return;
  }

  struct ble_scan_result *r = &scan_results[scan_count++];

  bt_addr_le_copy(&r->addr, addr);
  strncpy(r->name, name, sizeof(r->name) - 1);
  r->rssi = rssi;
  r->protocol = proto;

  char addr_str[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
  LOG_INF("[%d] %s \"%s\" RSSI %d (%s)", scan_count - 1, addr_str, name,
          rssi, proto ? proto->name : "unknown");
}

//////////////////////////////////////////////////////////////
// Scan Timeout
//////////////////////////////////////////////////////////////

static void scan_timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scan_timeout_work, scan_timeout_handler);

static void scan_timeout_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  if (scanning) {
    bt_le_scan_stop();
    scanning = false;
    LOG_INF("Scan complete: %d device(s) found", scan_count);
  }
}

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

static bool bt_ready;

int heater_ble_init(void)
{
  if (bt_ready) {
    return 0;
  }

  int err = bt_enable(NULL);

  if (err) {
    LOG_ERR("BT enable failed: %d", err);
    return err;
  }

  bt_ready = true;
  LOG_INF("Bluetooth initialized");
  return 0;
}

int heater_ble_scan(int timeout_sec)
{
  if (!bt_ready) {
    int err = heater_ble_init();
    if (err) {
      return err;
    }
  }

  if (scanning) {
    return -EALREADY;
  }

  scan_count = 0;

  int err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, scan_cb);

  if (err) {
    LOG_ERR("Scan start failed: %d", err);
    return err;
  }

  scanning = true;
  k_work_schedule(&scan_timeout_work, K_SECONDS(timeout_sec));
  LOG_INF("Scanning for %d seconds...", timeout_sec);
  return 0;
}

int heater_ble_scan_stop(void)
{
  if (!scanning) {
    return 0;
  }

  k_work_cancel_delayable(&scan_timeout_work);
  bt_le_scan_stop();
  scanning = false;
  LOG_INF("Scan stopped: %d device(s)", scan_count);
  return 0;
}

int heater_ble_connect(int index)
{
  if (heater_conn) {
    return -EALREADY;
  }

  if (index < 0 || index >= scan_count) {
    return -EINVAL;
  }

  if (scanning) {
    heater_ble_scan_stop();
  }

  struct ble_scan_result *r = &scan_results[index];

  active_protocol = r->protocol;
  if (!active_protocol && forced_protocol) {
    active_protocol = forced_protocol;
  }
  if (!active_protocol) {
    return -ENOTSUP;
  }

  int err = bt_conn_le_create(&r->addr, BT_CONN_LE_CREATE_CONN,
                              BT_LE_CONN_PARAM_DEFAULT, &heater_conn);
  if (err) {
    LOG_ERR("Connect failed: %d", err);
    heater_conn = NULL;
    return err;
  }

  LOG_INF("Connecting to %s (%s)...", r->name, active_protocol->name);
  return 0;
}

int heater_ble_disconnect(void)
{
  if (!heater_conn) {
    return -ENOTCONN;
  }

  stop_heartbeat();
  return bt_conn_disconnect(heater_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

void heater_ble_set_protocol(const struct heater_protocol *proto)
{
  forced_protocol = proto;
}

const struct heater_protocol *heater_ble_get_protocol(void)
{
  return active_protocol;
}

bool heater_ble_is_connected(void)
{
  return heater_conn != NULL;
}

int heater_ble_get_scan_count(void)
{
  return scan_count;
}

const struct ble_scan_result *heater_ble_get_scan_result(int index)
{
  if (index < 0 || index >= scan_count) {
    return NULL;
  }
  return &scan_results[index];
}

bool heater_ble_is_scanning(void)
{
  return scanning;
}

int heater_ble_send_power(bool on)
{
  if (!heater_conn || !active_protocol || write_handle == 0) {
    return -ENOTCONN;
  }

  uint8_t buf[16];
  int pkt_len = active_protocol->encode_power(buf, sizeof(buf), on);

  if (pkt_len < 0) {
    return pkt_len;
  }
  return bt_gatt_write_without_response(heater_conn, write_handle, buf,
                                        pkt_len, false);
}

int heater_ble_send_set_temp(int temp_c)
{
  if (!heater_conn || !active_protocol || write_handle == 0) {
    return -ENOTCONN;
  }

  uint8_t buf[16];
  int pkt_len = active_protocol->encode_set_temp(buf, sizeof(buf), temp_c);

  if (pkt_len < 0) {
    return pkt_len;
  }
  return bt_gatt_write_without_response(heater_conn, write_handle, buf,
                                        pkt_len, false);
}
```

### Step 2: Commit

`feat(auto-damper): add BLE connection manager with scan, connect, heartbeat`

---

## Task 6: BLE Shell Commands

**Files:**
- Modify: `src/shell.c`

**Depends on:** Task 5

### Step 1: Add BLE shell commands

Add the following includes at the top of `shell.c`:

```c
#include <zephyr/bluetooth/addr.h>
#include <auto_damper/heater.h>
```

Add extern declarations for `heater_ble.c` public API (after existing includes):

```c
extern int heater_ble_init(void);
extern int heater_ble_scan(int timeout_sec);
extern int heater_ble_scan_stop(void);
extern int heater_ble_connect(int index);
extern int heater_ble_disconnect(void);
extern void heater_ble_set_protocol(const struct heater_protocol *proto);
extern const struct heater_protocol *heater_ble_get_protocol(void);
extern bool heater_ble_is_connected(void);
extern bool heater_ble_is_scanning(void);
extern int heater_ble_get_scan_count(void);
extern int heater_ble_send_power(bool on);
extern int heater_ble_send_set_temp(int temp_c);

struct ble_scan_result {
  bt_addr_le_t addr;
  char name[32];
  int8_t rssi;
  const struct heater_protocol *protocol;
};

extern const struct ble_scan_result *heater_ble_get_scan_result(int index);
```

Add the BLE shell command handlers before the Shell Registration section:

```c
//////////////////////////////////////////////////////////////
// damper ble scan [timeout_sec]
//////////////////////////////////////////////////////////////

static int cmd_ble_scan(const struct shell *sh, size_t argc, char **argv)
{
  int timeout = 5;

  if (argc >= 2) {
    timeout = atoi(argv[1]);
    if (timeout <= 0 || timeout > 30) {
      timeout = 5;
    }
  }

  int err = heater_ble_scan(timeout);

  if (err == -EALREADY) {
    shell_warn(sh, "Already scanning");
  } else if (err) {
    shell_error(sh, "Scan failed: %d", err);
  } else {
    shell_print(sh, "Scanning for %d seconds...", timeout);
  }
  return err;
}

//////////////////////////////////////////////////////////////
// damper ble stop
//////////////////////////////////////////////////////////////

static int cmd_ble_stop(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  heater_ble_scan_stop();
  shell_print(sh, "Scan stopped, %d device(s) found",
              heater_ble_get_scan_count());

  for (int i = 0; i < heater_ble_get_scan_count(); i++) {
    const struct ble_scan_result *r = heater_ble_get_scan_result(i);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&r->addr, addr_str, sizeof(addr_str));
    shell_print(sh, "  [%d] %s \"%s\" RSSI %d (%s)", i, addr_str, r->name,
                r->rssi, r->protocol ? r->protocol->name : "unknown");
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// damper ble connect <index>
//////////////////////////////////////////////////////////////

static int cmd_ble_connect(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 2) {
    shell_error(sh, "Usage: damper ble connect <index>");
    return -EINVAL;
  }

  int index = atoi(argv[1]);
  int err = heater_ble_connect(index);

  if (err == -EALREADY) {
    shell_warn(sh, "Already connected");
  } else if (err == -EINVAL) {
    shell_error(sh, "Invalid index (0-%d)", heater_ble_get_scan_count() - 1);
  } else if (err) {
    shell_error(sh, "Connect failed: %d", err);
  } else {
    shell_print(sh, "Connecting...");
  }
  return err;
}

//////////////////////////////////////////////////////////////
// damper ble disconnect
//////////////////////////////////////////////////////////////

static int cmd_ble_disconnect(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  int err = heater_ble_disconnect();

  if (err == -ENOTCONN) {
    shell_warn(sh, "Not connected");
  } else if (err) {
    shell_error(sh, "Disconnect failed: %d", err);
  } else {
    shell_print(sh, "Disconnecting...");
  }
  return err;
}

//////////////////////////////////////////////////////////////
// damper ble protocol <byd|cc|auto>
//////////////////////////////////////////////////////////////

static int cmd_ble_protocol(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 2) {
    const struct heater_protocol *p = heater_ble_get_protocol();
    shell_print(sh, "Protocol: %s", p ? p->name : "none");
    shell_print(sh, "Usage: damper ble protocol <byd|cc|auto>");
    return 0;
  }

  if (strcmp(argv[1], "byd") == 0) {
    heater_ble_set_protocol(&heater_protocol_byd);
    shell_print(sh, "Protocol forced: byd");
  } else if (strcmp(argv[1], "cc") == 0) {
    heater_ble_set_protocol(&heater_protocol_cc);
    shell_print(sh, "Protocol forced: cc");
  } else if (strcmp(argv[1], "auto") == 0) {
    heater_ble_set_protocol(NULL);
    shell_print(sh, "Protocol: auto-detect");
  } else {
    shell_error(sh, "Unknown protocol: %s (use byd, cc, or auto)", argv[1]);
    return -EINVAL;
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// damper ble status
//////////////////////////////////////////////////////////////

static int cmd_ble_status(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  const struct heater_protocol *p = heater_ble_get_protocol();

  shell_print(sh, "BLE Status:");
  shell_print(sh, "  Connected:    %s",
              heater_ble_is_connected() ? "yes" : "no");
  shell_print(sh, "  Protocol:     %s", p ? p->name : "none");
  shell_print(sh, "  Scanning:     %s",
              heater_ble_is_scanning() ? "yes" : "no");

  if (!heater_ble_is_connected()) {
    return 0;
  }

  struct heater_data hdata;
  int ret = zbus_chan_read(&heater_data_chan, &hdata, K_MSEC(100));

  if (ret) {
    shell_warn(sh, "  No telemetry data");
    return 0;
  }

  shell_print(sh, "Heater Telemetry:");
  shell_print(sh, "  Power:        %s",
              heater_power_state_str(hdata.power));
  shell_print(sh, "  Step:         %s",
              heater_run_step_str(hdata.step));
  shell_print(sh, "  Exhaust:      %.1f C", hdata.exhaust_temp_c);
  shell_print(sh, "  Ambient:      %.1f C", hdata.ambient_temp_c);
  shell_print(sh, "  Voltage:      %.1f V", hdata.voltage);
  shell_print(sh, "  Target temp:  %d C", hdata.target_temp);
  shell_print(sh, "  Gear:         %d", hdata.gear_level);
  shell_print(sh, "  Error:        %d", hdata.error_code);
  return 0;
}
```

### Step 2: Add string helpers

Add to `heater_ble.c` (or a new `heater_common.c`):

```c
const char *heater_power_state_str(enum heater_power_state s)
{
  switch (s) {
  case HEATER_POWER_OFF: return "OFF";
  case HEATER_POWER_STARTING: return "STARTING";
  case HEATER_POWER_RUNNING: return "RUNNING";
  case HEATER_POWER_SHUTTING_DOWN: return "SHUTTING_DOWN";
  default: return "UNKNOWN";
  }
}

const char *heater_run_step_str(enum heater_run_step s)
{
  switch (s) {
  case HEATER_STEP_IDLE: return "IDLE";
  case HEATER_STEP_SELF_CHECK: return "SELF_CHECK";
  case HEATER_STEP_PREHEAT: return "PREHEAT";
  case HEATER_STEP_HEATING: return "HEATING";
  case HEATER_STEP_COOLING: return "COOLING";
  default: return "UNKNOWN";
  }
}
```

### Step 3: Update shell registration

Replace the existing `SHELL_STATIC_SUBCMD_SET_CREATE` and
`SHELL_CMD_REGISTER` at the bottom of `shell.c`:

```c
//////////////////////////////////////////////////////////////
// Shell Registration
//////////////////////////////////////////////////////////////

SHELL_STATIC_SUBCMD_SET_CREATE(
    ble_cmds,
    SHELL_CMD_ARG(scan, NULL, "Scan for heaters [timeout_sec]", cmd_ble_scan,
                  1, 1),
    SHELL_CMD(stop, NULL, "Stop scanning", cmd_ble_stop),
    SHELL_CMD_ARG(connect, NULL, "Connect: damper ble connect <index>",
                  cmd_ble_connect, 2, 0),
    SHELL_CMD(disconnect, NULL, "Disconnect from heater", cmd_ble_disconnect),
    SHELL_CMD_ARG(protocol, NULL, "Set protocol: byd, cc, or auto",
                  cmd_ble_protocol, 1, 1),
    SHELL_CMD(status, NULL, "Show heater BLE status and telemetry",
              cmd_ble_status),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    damper_cmds,
    SHELL_CMD(status, NULL, "Show damper status and config", cmd_status),
    SHELL_CMD(set, NULL, "Set config: damper set <param> <value>", cmd_set),
    SHELL_CMD(override, NULL, "Override: damper override <inside|outside|auto>",
              cmd_override),
    SHELL_CMD(ble, &ble_cmds, "BLE heater commands", NULL),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(damper, &damper_cmds, "Damper control commands", NULL);
```

### Step 4: Commit

`feat(auto-damper): add BLE shell commands for heater scan/connect/monitor`

---

## Task 7: Build and Hardware Verification

**Depends on:** All previous tasks

### Step 1: Fetch BLE blob (if not already done)

Run from Thermoquad root:

```bash
source .venv/bin/activate
export ZEPHYR_BASE="zephyr"
west blobs fetch hal_espressif
```

### Step 2: Build

Run: `task rebuild-firmware`

Expected: Build succeeds. Flash and DRAM usage will increase significantly
due to BLE stack (~+100KB flash, ~+30KB RAM).

### Step 3: Flash and verify BT init

Run: `task flash-firmware`

Expected serial output:

```
[00:00:xx.xxx] <inf> heater_ble: Bluetooth initialized
```

Or BT init may be deferred until first `damper ble scan`.

### Step 4: Verify scan

```
uart:~$ damper ble scan
Scanning for 5 seconds...
```

After 5 seconds:

```
uart:~$ damper ble stop
Scan stopped, N device(s) found
  [0] XX:XX:XX:XX:XX:XX "BYD-1234" RSSI -65 (byd)
```

If no heater is powered on, verify scan starts and stops without crashing.

### Step 5: Verify connect (requires powered-on heater)

```
uart:~$ damper ble connect 0
Connecting...
Connected to heater (byd protocol)
Service found: handles X-Y
Write char 0xffe1: handle=X
Notify char 0xffe1: handle=X
Subscribed to notifications
```

### Step 6: Verify telemetry

```
uart:~$ damper ble status
BLE Status:
  Connected:    yes
  Protocol:     byd
  Scanning:     no
Heater Telemetry:
  Power:        OFF
  Step:         IDLE
  Exhaust:      25.0 C
  Ambient:      22.0 C
  Voltage:      12.4 V
  ...
```

### Step 7: Verify protocol switch

```
uart:~$ damper ble protocol cc
Protocol forced: cc
uart:~$ damper ble scan
```

Scan should now match any device with "Heater" in name.

---

## Verification Checklist

- [ ] Build succeeds with BLE enabled
- [ ] Flash usage is within ESP32 limits (4MB)
- [ ] `damper ble scan` starts and stops cleanly
- [ ] `damper ble protocol` switches between byd, cc, and auto
- [ ] `damper ble connect` establishes connection (requires heater)
- [ ] `damper ble status` shows decoded telemetry (requires heater)
- [ ] `damper ble disconnect` cleanly disconnects
- [ ] Heartbeat runs at protocol-specified interval
- [ ] Existing damper commands still work (status, set, override)
- [ ] No crashes on scan with no heater present
- [ ] No crashes on connection timeout
