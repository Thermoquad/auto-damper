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

static int connected_index = -1;
static bool auto_reconnect;
static struct heater_data last_heater_data;

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static void start_discovery(void);
static void subscribe_notify(void);
static void heartbeat_handler(struct k_work *work);
static void reconnect_handler(struct k_work *work);
static void schedule_rescan(void);
int heater_ble_scan(int timeout_sec);
int heater_ble_connect(int index);

static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_handler);
static K_WORK_DELAYABLE_DEFINE(reconnect_work, reconnect_handler);

#define RECONNECT_DELAY_MS 3000

//////////////////////////////////////////////////////////////
// Zbus Channel
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DEFINE(heater_data_chan, struct heater_data, NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.power = HEATER_POWER_OFF,
                              .step = HEATER_STEP_IDLE, .connected = false,
                              .timestamp_us = 0));

ZBUS_CHAN_DEFINE(heater_devices_chan, struct heater_devices, NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.count = 0, .connected_index = -1));

static void heater_cmd_callback(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(heater_cmd_listener, heater_cmd_callback);

ZBUS_CHAN_DEFINE(heater_command_chan, struct heater_command, NULL, NULL,
                ZBUS_OBSERVERS(heater_cmd_listener),
                ZBUS_MSG_INIT(.type = HEATER_CMD_SCAN));

static void publish_devices(void)
{
  struct heater_devices msg = {.count = scan_count, .connected_index = connected_index};

  for (int i = 0; i < scan_count && i < HEATER_DEVICES_MAX; i++) {
    strncpy(msg.devices[i].name, scan_results[i].name,
            sizeof(msg.devices[i].name) - 1);
    msg.devices[i].rssi = scan_results[i].rssi;
    if (scan_results[i].protocol) {
      strncpy(msg.devices[i].protocol, scan_results[i].protocol->name,
              sizeof(msg.devices[i].protocol) - 1);
    }
  }
  zbus_chan_pub(&heater_devices_chan, &msg, K_NO_WAIT);
}

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
// Auto-Reconnect
//////////////////////////////////////////////////////////////

static void reconnect_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  if (heater_conn || !auto_reconnect || connected_index < 0) {
    return;
  }

  LOG_INF("Auto-reconnecting to index %d...", connected_index);
  int err = heater_ble_connect(connected_index);
  if (err && err != -EALREADY) {
    LOG_WRN("Reconnect failed: %d, retrying...", err);
    k_work_schedule(&reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
  }
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

  struct heater_data hdata = last_heater_data;
  int ret = active_protocol->decode(data, length, &hdata);

  if (ret == 0) {
    hdata.connected = true;
    hdata.timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks());
    if (connected_index >= 0 && connected_index < scan_count) {
      strncpy(hdata.name, scan_results[connected_index].name,
              sizeof(hdata.name) - 1);
    }
    last_heater_data = hdata;
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

  if (active_protocol && active_protocol->encode_query_auto_offsets &&
      write_handle != 0) {
    uint8_t qbuf[16];
    int qlen = active_protocol->encode_query_auto_offsets(qbuf, sizeof(qbuf));
    if (qlen > 0) {
      bt_gatt_write_without_response(heater_conn, write_handle,
                                     qbuf, qlen, false);
    }
  }
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
    if (auto_reconnect && connected_index >= 0) {
      k_work_schedule(&reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
    }
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
  publish_devices();

  if (auto_reconnect && connected_index >= 0) {
    LOG_INF("Will reconnect in %d ms", RECONNECT_DELAY_MS);
    k_work_schedule(&reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
  } else {
    schedule_rescan();
  }
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
// Scan Timeout & Periodic Rescan
//////////////////////////////////////////////////////////////

#define RESCAN_INTERVAL_SEC 15

static void scan_timeout_handler(struct k_work *work);
static void rescan_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scan_timeout_work, scan_timeout_handler);
static K_WORK_DELAYABLE_DEFINE(rescan_work, rescan_handler);

static void schedule_rescan(void)
{
  if (!heater_conn) {
    k_work_schedule(&rescan_work, K_SECONDS(RESCAN_INTERVAL_SEC));
  }
}

static void rescan_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  if (heater_conn || scanning) {
    return;
  }
  heater_ble_scan(5);
}

static void scan_timeout_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  if (scanning) {
    bt_le_scan_stop();
    scanning = false;
    LOG_INF("Scan complete: %d device(s) found", scan_count);
    publish_devices();
    if (!heater_conn && scan_count > 0) {
      heater_ble_connect(0);
    } else {
      schedule_rescan();
    }
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
  publish_devices();
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
  k_work_cancel_delayable(&rescan_work);

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
    schedule_rescan();
    return err;
  }

  connected_index = index;
  auto_reconnect = true;
  publish_devices();

  LOG_INF("Connecting to %s (%s)...", r->name, active_protocol->name);
  return 0;
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

int heater_ble_get_connected_index(void)
{
  if (!heater_conn) {
    return -1;
  }

  struct bt_conn_info info;
  if (bt_conn_get_info(heater_conn, &info) < 0) {
    return -1;
  }

  for (int i = 0; i < scan_count; i++) {
    if (bt_addr_le_cmp(&scan_results[i].addr, info.le.dst) == 0) {
      return i;
    }
  }
  return -1;
}

const char *heater_ble_get_connected_name(void)
{
  int idx = heater_ble_get_connected_index();
  if (idx < 0) {
    return NULL;
  }
  return scan_results[idx].name;
}

int heater_ble_send_power(bool on)
{
  if (!heater_conn || !active_protocol || write_handle == 0) {
    return -ENOTCONN;
  }

  uint8_t buf[16];
  int pkt_len;

  if (!on && last_heater_data.mode == HEATER_MODE_FAN &&
      active_protocol->encode_set_mode) {
    /* CC fan mode needs its own toggle (0xA4) — the generic power
       toggle (0xA1) would switch to heating instead of off. */
    pkt_len = active_protocol->encode_set_mode(buf, sizeof(buf),
                                               HEATER_MODE_FAN);
  } else {
    pkt_len = active_protocol->encode_power(buf, sizeof(buf), on);
  }

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

int heater_ble_send_adjust_power(int delta)
{
  if (!heater_conn || !active_protocol || write_handle == 0) {
    return -ENOTCONN;
  }

  if (!active_protocol->encode_adjust_power) {
    return -ENOTSUP;
  }

  uint8_t buf[16];
  int pkt_len = active_protocol->encode_adjust_power(buf, sizeof(buf), delta);

  if (pkt_len < 0) {
    return pkt_len;
  }
  return bt_gatt_write_without_response(heater_conn, write_handle, buf,
                                        pkt_len, false);
}

int heater_ble_send_set_mode(enum heater_run_mode mode)
{
  if (!heater_conn || !active_protocol || write_handle == 0) {
    return -ENOTCONN;
  }

  if (!active_protocol->encode_set_mode) {
    return -ENOTSUP;
  }

  uint8_t buf[16];
  int pkt_len = active_protocol->encode_set_mode(buf, sizeof(buf), mode);

  if (pkt_len < 0) {
    return pkt_len;
  }
  return bt_gatt_write_without_response(heater_conn, write_handle, buf,
                                        pkt_len, false);
}

//////////////////////////////////////////////////////////////
// Zbus Command Listener
//////////////////////////////////////////////////////////////

static void heater_cmd_callback(const struct zbus_channel *chan)
{
  const struct heater_command *cmd = zbus_chan_const_msg(chan);

  switch (cmd->type) {
  case HEATER_CMD_SCAN:
    heater_ble_scan(cmd->scan_timeout);
    break;
  case HEATER_CMD_SCAN_STOP:
    heater_ble_scan_stop();
    break;
  case HEATER_CMD_CONNECT:
    heater_ble_connect(cmd->connect_index);
    break;
  case HEATER_CMD_POWER:
    heater_ble_send_power(cmd->power_on);
    break;
  case HEATER_CMD_SET_MODE:
    heater_ble_send_set_mode(cmd->mode);
    break;
  case HEATER_CMD_SET_TEMP:
    heater_ble_send_set_temp(cmd->temp);
    break;
  case HEATER_CMD_ADJUST_POWER:
    heater_ble_send_adjust_power(cmd->power_delta);
    break;
  case HEATER_CMD_ALTITUDE:
    if (heater_conn && active_protocol && active_protocol->encode_altitude &&
        write_handle != 0) {
      uint8_t abuf[16];
      int alen = active_protocol->encode_altitude(abuf, sizeof(abuf));
      if (alen > 0) {
        bt_gatt_write_without_response(heater_conn, write_handle,
                                       abuf, alen, false);
      }
    }
    break;
  case HEATER_CMD_SET_AUTO_OFFSETS:
    if (heater_conn && active_protocol &&
        active_protocol->encode_set_auto_offsets && write_handle != 0) {
      uint8_t obuf[16];
      int olen = active_protocol->encode_set_auto_offsets(
          obuf, sizeof(obuf), cmd->auto_offsets.startup,
          cmd->auto_offsets.shutdown);
      if (olen > 0) {
        bt_gatt_write_without_response(heater_conn, write_handle,
                                       obuf, olen, false);
        if (active_protocol->encode_query_auto_offsets) {
          int qlen = active_protocol->encode_query_auto_offsets(
              obuf, sizeof(obuf));
          if (qlen > 0) {
            bt_gatt_write_without_response(heater_conn, write_handle,
                                           obuf, qlen, false);
          }
        }
      }
    }
    break;
  case HEATER_CMD_QUERY_AUTO_OFFSETS:
    if (heater_conn && active_protocol &&
        active_protocol->encode_query_auto_offsets && write_handle != 0) {
      uint8_t obuf[16];
      int olen = active_protocol->encode_query_auto_offsets(
          obuf, sizeof(obuf));
      if (olen > 0) {
        bt_gatt_write_without_response(heater_conn, write_handle,
                                       obuf, olen, false);
      }
    }
    break;
  case HEATER_CMD_RAW:
    if (heater_conn && write_handle != 0 && cmd->raw.len > 0) {
      bt_gatt_write_without_response(heater_conn, write_handle,
                                     cmd->raw.data, cmd->raw.len, false);
    }
    break;
  }
}

//////////////////////////////////////////////////////////////
// String Helpers
//////////////////////////////////////////////////////////////

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
  case HEATER_STEP_BLOWING: return "BLOWING";
  default: return "UNKNOWN";
  }
}

const char *heater_run_mode_str(enum heater_run_mode m)
{
  switch (m) {
  case HEATER_MODE_MANUAL: return "manual";
  case HEATER_MODE_AUTOMATIC: return "automatic";
  case HEATER_MODE_FAN: return "fan";
  default: return "unknown";
  }
}
