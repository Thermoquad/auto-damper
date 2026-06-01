// SPDX-License-Identifier: Apache-2.0

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
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

#define MAX_SCAN_RESULTS 10

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
// Heater Slots
//
// One slot per concurrent heater. A slot becomes `active` when
// heater_ble_connect() picks it up; it stays active across
// disconnects when auto_reconnect is true. Discovery / subscribe
// param structs live in the slot because the GATT subsystem retains
// pointers to them for the duration of an operation — sharing
// across slots would corrupt in-flight discoveries on other heaters.
//////////////////////////////////////////////////////////////

struct heater_slot {
  bool active;
  struct bt_conn *conn;
  const struct heater_protocol *protocol;
  bt_addr_le_t addr;

  struct heater_data data;

  uint16_t write_handle;
  uint16_t notify_handle;
  uint16_t svc_start_handle;
  uint16_t svc_end_handle;

  struct bt_gatt_discover_params disc_params;
  struct bt_uuid_16 disc_uuid;
  struct bt_gatt_subscribe_params sub_params;
  struct bt_gatt_discover_params ccc_disc_params;

  struct k_work_delayable heartbeat_work;
  struct k_work_delayable reconnect_work;
  struct k_work_delayable offset_query_work;

  bool auto_reconnect;
};

static struct heater_slot slots[HEATERS_MAX];

static const struct heater_protocol *forced_protocol;

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static void start_discovery(struct heater_slot *s);
static void subscribe_notify(struct heater_slot *s);
static void heartbeat_handler(struct k_work *work);
static void reconnect_handler(struct k_work *work);
static void offset_query_handler(struct k_work *work);
static void radio_publish_handler(struct k_work *work);
static void schedule_rescan(void);
static void publish_heater_state(void);
static void publish_devices(void);
static int connect_slot(struct heater_slot *s);
int heater_ble_scan(int timeout_sec);
int heater_ble_connect(int index);

static K_WORK_DELAYABLE_DEFINE(radio_publish_work, radio_publish_handler);

#define RADIO_PUBLISH_INTERVAL_MS 2000
#define RECONNECT_DELAY_MS 3000

//////////////////////////////////////////////////////////////
// Zbus Channels
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DEFINE(heater_states_chan, struct heater_states, NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.count = 0));

ZBUS_CHAN_DEFINE(heater_devices_chan, struct heater_devices, NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.count = 0));

static void heater_cmd_callback(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(heater_cmd_listener, heater_cmd_callback);

ZBUS_CHAN_DEFINE(heater_command_chan, struct heater_command, NULL, NULL,
                ZBUS_OBSERVERS(heater_cmd_listener),
                ZBUS_MSG_INIT(.type = HEATER_CMD_SCAN));

//////////////////////////////////////////////////////////////
// Slot Helpers
//////////////////////////////////////////////////////////////

static struct heater_slot *slot_for_conn(struct bt_conn *conn)
{
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (slots[i].conn == conn) {
      return &slots[i];
    }
  }
  return NULL;
}

static struct heater_slot *slot_for_name(const char *name)
{
  if (!name || name[0] == '\0') {
    return NULL;
  }
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (slots[i].active && strcmp(slots[i].data.name, name) == 0) {
      return &slots[i];
    }
  }
  return NULL;
}

static struct heater_slot *find_free_slot(void)
{
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (!slots[i].active) {
      return &slots[i];
    }
  }
  return NULL;
}

static void release_slot(struct heater_slot *s)
{
  if (!s) {
    return;
  }
  k_work_cancel_delayable(&s->heartbeat_work);
  k_work_cancel_delayable(&s->reconnect_work);
  k_work_cancel_delayable(&s->offset_query_work);
  if (s->conn) {
    bt_conn_unref(s->conn);
    s->conn = NULL;
  }
  s->active = false;
  s->protocol = NULL;
  s->auto_reconnect = false;
  s->write_handle = 0;
  s->notify_handle = 0;
  s->svc_start_handle = 0;
  s->svc_end_handle = 0;
  memset(&s->data, 0, sizeof(s->data));
  memset(&s->addr, 0, sizeof(s->addr));
}

//////////////////////////////////////////////////////////////
// Publication
//////////////////////////////////////////////////////////////

static void publish_heater_state(void)
{
  /* struct heater_states is ~1.2KB — too large to stack-allocate in
   * a BT host callback. Claim the channel's storage and write in
   * place; finish + notify dispatches to observers. */
  if (zbus_chan_claim(&heater_states_chan, K_MSEC(100)) != 0) {
    return;
  }
  struct heater_states *msg = zbus_chan_msg(&heater_states_chan);

  msg->count = 0;
  for (int i = 0; i < HEATERS_MAX && msg->count < HEATERS_MAX; i++) {
    if (slots[i].active) {
      msg->heaters[msg->count++] = slots[i].data;
    }
  }
  zbus_chan_finish(&heater_states_chan);
  zbus_chan_notify(&heater_states_chan, K_MSEC(100));
}

static void publish_devices(void)
{
  struct heater_devices msg = {.count = scan_count};

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
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct heater_slot *s = CONTAINER_OF(dwork, struct heater_slot, heartbeat_work);

  if (!s->conn || !s->protocol || s->write_handle == 0) {
    return;
  }

  uint8_t buf[16];
  int pkt_len = s->protocol->encode_ping(buf, sizeof(buf));

  if (pkt_len > 0) {
    bt_gatt_write_without_response(s->conn, s->write_handle, buf, pkt_len, false);
  }
  k_work_schedule(&s->heartbeat_work, K_MSEC(s->protocol->heartbeat_ms));
}

static void offset_query_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct heater_slot *s = CONTAINER_OF(dwork, struct heater_slot, offset_query_work);

  if (!s->conn || !s->protocol || s->write_handle == 0) {
    return;
  }

  uint8_t buf[16];
  int pkt_len = s->protocol->encode_ping(buf, sizeof(buf));
  if (pkt_len > 0) {
    bt_gatt_write_without_response(s->conn, s->write_handle, buf, pkt_len, false);
  }

  if (s->protocol->encode_query_auto_offsets) {
    int qlen = s->protocol->encode_query_auto_offsets(buf, sizeof(buf));
    if (qlen > 0) {
      bt_gatt_write_without_response(s->conn, s->write_handle, buf, qlen, false);
    }
  }
}

//////////////////////////////////////////////////////////////
// Auto-Reconnect
//////////////////////////////////////////////////////////////

static void reconnect_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct heater_slot *s = CONTAINER_OF(dwork, struct heater_slot, reconnect_work);

  if (!s->active || s->conn || !s->auto_reconnect) {
    return;
  }

  LOG_INF("[%s] Auto-reconnecting...", s->data.name);
  int err = connect_slot(s);
  if (err && err != -EALREADY) {
    LOG_WRN("[%s] Reconnect failed: %d", s->data.name, err);
    k_work_schedule(&s->reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
  }
}

//////////////////////////////////////////////////////////////
// Notify Callback
//////////////////////////////////////////////////////////////

static uint8_t notify_cb(struct bt_conn *conn,
                         struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length)
{
  ARG_UNUSED(conn);

  struct heater_slot *s =
      CONTAINER_OF(params, struct heater_slot, sub_params);

  if (!data) {
    LOG_WRN("[%s] Unsubscribed", s->data.name);
    return BT_GATT_ITER_STOP;
  }

  if (!s->protocol) {
    return BT_GATT_ITER_CONTINUE;
  }

  struct heater_data hdata = s->data;
  int ret = s->protocol->decode(data, length, &hdata);

  if (ret == 0) {
    hdata.connected = true;
    hdata.timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks());
    /* protocol decoder may not set .name; ensure ours is preserved */
    strncpy(hdata.name, s->data.name, sizeof(hdata.name) - 1);
    s->data = hdata;
    publish_heater_state();
  } else {
    LOG_DBG("[%s] Decode failed: %d (len=%u)", s->data.name, ret, length);
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
  struct heater_slot *s =
      CONTAINER_OF(params, struct heater_slot, disc_params);

  if (!attr) {
    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
      if (s->write_handle != 0 && s->notify_handle != 0) {
        LOG_INF("[%s] Discovery complete: write=%u notify=%u",
                s->data.name, s->write_handle, s->notify_handle);
        subscribe_notify(s);
      } else {
        LOG_ERR("[%s] Missing handles: write=%u notify=%u",
                s->data.name, s->write_handle, s->notify_handle);
      }
    } else {
      LOG_ERR("[%s] Service not found", s->data.name);
    }
    return BT_GATT_ITER_STOP;
  }

  if (params->type == BT_GATT_DISCOVER_PRIMARY) {
    struct bt_gatt_service_val *svc = attr->user_data;

    s->svc_start_handle = attr->handle + 1;
    s->svc_end_handle = svc->end_handle;
    LOG_INF("[%s] Service found: handles %u-%u",
            s->data.name, s->svc_start_handle, s->svc_end_handle);

    params->uuid = NULL;
    params->start_handle = s->svc_start_handle;
    params->end_handle = s->svc_end_handle;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, params);
    if (err) {
      LOG_ERR("[%s] Char discovery failed: %d", s->data.name, err);
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

    if (uuid_val == s->protocol->write_char_uuid) {
      s->write_handle = chrc->value_handle;
      LOG_INF("[%s] Write char 0x%04x: handle=%u",
              s->data.name, uuid_val, s->write_handle);
    }

    if (s->protocol->notify_char_uuid != 0) {
      if (uuid_val == s->protocol->notify_char_uuid && has_notify) {
        s->notify_handle = chrc->value_handle;
        LOG_INF("[%s] Notify char 0x%04x: handle=%u",
                s->data.name, uuid_val, s->notify_handle);
      }
    } else if (has_notify && s->notify_handle == 0) {
      s->notify_handle = chrc->value_handle;
      LOG_INF("[%s] Notify char (auto) 0x%04x: handle=%u",
              s->data.name, uuid_val, s->notify_handle);
    }

    return BT_GATT_ITER_CONTINUE;
  }

  return BT_GATT_ITER_STOP;
}

static void start_discovery(struct heater_slot *s)
{
  s->write_handle = 0;
  s->notify_handle = 0;

  s->disc_uuid.uuid.type = BT_UUID_TYPE_16;
  s->disc_uuid.val = s->protocol->service_uuid;

  s->disc_params.uuid = &s->disc_uuid.uuid;
  s->disc_params.func = discover_cb;
  s->disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  s->disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  s->disc_params.type = BT_GATT_DISCOVER_PRIMARY;

  int err = bt_gatt_discover(s->conn, &s->disc_params);
  if (err) {
    LOG_ERR("[%s] Discovery start failed: %d", s->data.name, err);
  }
}

static void subscribe_notify(struct heater_slot *s)
{
  s->sub_params.notify = notify_cb;
  s->sub_params.value = BT_GATT_CCC_NOTIFY;
  s->sub_params.value_handle = s->notify_handle;
  s->sub_params.ccc_handle = 0;
#if defined(CONFIG_BT_GATT_AUTO_DISCOVER_CCC)
  s->sub_params.end_handle = s->svc_end_handle;
  s->sub_params.disc_params = &s->ccc_disc_params;
#endif

  int err = bt_gatt_subscribe(s->conn, &s->sub_params);
  if (err && err != -EALREADY) {
    LOG_ERR("[%s] Subscribe failed: %d", s->data.name, err);
    return;
  }
  LOG_INF("[%s] Subscribed to notifications", s->data.name);

  k_work_schedule(&s->heartbeat_work, K_MSEC(100));

  if (s->protocol->encode_query_auto_offsets && s->write_handle != 0) {
    uint8_t qbuf[16];
    int qlen = s->protocol->encode_query_auto_offsets(qbuf, sizeof(qbuf));
    if (qlen > 0) {
      bt_gatt_write_without_response(s->conn, s->write_handle, qbuf, qlen, false);
    }
  }
}

//////////////////////////////////////////////////////////////
// Connection Callbacks
//////////////////////////////////////////////////////////////

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
  struct heater_slot *s = slot_for_conn(conn);
  if (!s) {
    return;
  }

  if (err) {
    LOG_ERR("[%s] Connect failed: %u", s->data.name, err);
    bt_conn_unref(s->conn);
    s->conn = NULL;
    s->data.connected = false;
    publish_heater_state();
    if (s->auto_reconnect) {
      k_work_schedule(&s->reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
    } else {
      release_slot(s);
      publish_heater_state();
    }
    return;
  }

  LOG_INF("[%s] Connected (%s)", s->data.name, s->protocol->name);
  s->data.connected = true;
  publish_heater_state();
  start_discovery(s);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
  struct heater_slot *s = slot_for_conn(conn);
  if (!s) {
    return;
  }

  LOG_INF("[%s] Disconnected: reason %u", s->data.name, reason);
  k_work_cancel_delayable(&s->heartbeat_work);

  bt_conn_unref(s->conn);
  s->conn = NULL;
  s->write_handle = 0;
  s->notify_handle = 0;
  s->data.connected = false;

  /* Clear transient telemetry but keep .name so reconnect can identify us */
  char saved_name[32];
  strncpy(saved_name, s->data.name, sizeof(saved_name));
  saved_name[sizeof(saved_name) - 1] = '\0';
  memset(&s->data, 0, sizeof(s->data));
  strncpy(s->data.name, saved_name, sizeof(s->data.name) - 1);

  if (s->auto_reconnect) {
    publish_heater_state();
    LOG_INF("[%s] Will reconnect in %d ms", s->data.name, RECONNECT_DELAY_MS);
    k_work_schedule(&s->reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
  } else {
    release_slot(s);
    publish_heater_state();
    publish_devices();
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
  /* Rescan periodically regardless of currently-connected slots —
   * new heaters that come into range should be auto-connected too. */
  k_work_schedule(&rescan_work, K_SECONDS(RESCAN_INTERVAL_SEC));
}

static void rescan_handler(struct k_work *work)
{
  ARG_UNUSED(work);
  if (scanning) {
    return;
  }
  heater_ble_scan(5);
}

static void scan_timeout_handler(struct k_work *work)
{
  ARG_UNUSED(work);
  if (!scanning) {
    return;
  }
  bt_le_scan_stop();
  scanning = false;
  LOG_INF("Scan complete: %d device(s) found", scan_count);
  publish_devices();

  /* Auto-connect to every heater in range that isn't already in a
   * slot. heater_ble_connect() short-circuits with -EALREADY when
   * the address is already managed and -ENOSPC when slots are full;
   * both are non-fatal here. */
  for (int i = 0; i < scan_count; i++) {
    int err = heater_ble_connect(i);
    if (err && err != -EALREADY && err != -ENOSPC) {
      LOG_WRN("Auto-connect to [%d] %s failed: %d",
              i, scan_results[i].name, err);
    }
  }
  schedule_rescan();
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

  for (int i = 0; i < HEATERS_MAX; i++) {
    k_work_init_delayable(&slots[i].heartbeat_work, heartbeat_handler);
    k_work_init_delayable(&slots[i].reconnect_work, reconnect_handler);
    k_work_init_delayable(&slots[i].offset_query_work, offset_query_handler);
  }

  int err = bt_enable(NULL);
  if (err) {
    LOG_ERR("BT enable failed: %d", err);
    return err;
  }
  bt_ready = true;
  LOG_INF("Bluetooth initialized");

  k_work_reschedule(&radio_publish_work, K_MSEC(RADIO_PUBLISH_INTERVAL_MS));
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

static int connect_slot(struct heater_slot *s)
{
  if (s->conn) {
    return -EALREADY;
  }
  int err = bt_conn_le_create(&s->addr, BT_CONN_LE_CREATE_CONN,
                              BT_LE_CONN_PARAM_DEFAULT, &s->conn);
  if (err) {
    LOG_ERR("[%s] Connect failed: %d", s->data.name, err);
    s->conn = NULL;
    return err;
  }
  LOG_INF("[%s] Connecting (%s)...", s->data.name, s->protocol->name);
  return 0;
}

int heater_ble_connect(int index)
{
  if (index < 0 || index >= scan_count) {
    return -EINVAL;
  }

  if (scanning) {
    heater_ble_scan_stop();
  }
  k_work_cancel_delayable(&rescan_work);

  struct ble_scan_result *r = &scan_results[index];

  for (int i = 0; i < HEATERS_MAX; i++) {
    if (slots[i].active && bt_addr_le_cmp(&slots[i].addr, &r->addr) == 0) {
      return -EALREADY;
    }
  }

  const struct heater_protocol *proto = r->protocol;
  if (!proto && forced_protocol) {
    proto = forced_protocol;
  }
  if (!proto) {
    return -ENOTSUP;
  }

  struct heater_slot *s = find_free_slot();
  if (!s) {
    LOG_WRN("No free slot for new heater");
    return -ENOSPC;
  }

  s->active = true;
  s->protocol = proto;
  bt_addr_le_copy(&s->addr, &r->addr);
  memset(&s->data, 0, sizeof(s->data));
  strncpy(s->data.name, r->name, sizeof(s->data.name) - 1);
  s->auto_reconnect = true;

  int err = connect_slot(s);
  if (err) {
    release_slot(s);
    schedule_rescan();
    return err;
  }
  publish_devices();
  publish_heater_state();
  return 0;
}

int heater_ble_disconnect(const char *name)
{
  struct heater_slot *s = slot_for_name(name);
  if (!s) {
    return -ENOENT;
  }
  s->auto_reconnect = false;
  if (s->conn) {
    return bt_conn_disconnect(s->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
  }
  release_slot(s);
  publish_heater_state();
  return 0;
}

void heater_ble_set_protocol(const struct heater_protocol *proto)
{
  forced_protocol = proto;
}

const struct heater_protocol *heater_ble_get_protocol(void)
{
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (slots[i].active && slots[i].protocol) {
      return slots[i].protocol;
    }
  }
  return NULL;
}

bool heater_ble_is_connected(void)
{
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (slots[i].conn != NULL) {
      return true;
    }
  }
  return false;
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
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (!slots[i].conn) {
      continue;
    }
    for (int j = 0; j < scan_count; j++) {
      if (bt_addr_le_cmp(&scan_results[j].addr, &slots[i].addr) == 0) {
        return j;
      }
    }
  }
  return -1;
}

const char *heater_ble_get_connected_name(void)
{
  for (int i = 0; i < HEATERS_MAX; i++) {
    if (slots[i].conn && slots[i].data.name[0] != '\0') {
      return slots[i].data.name;
    }
  }
  return NULL;
}

//////////////////////////////////////////////////////////////
// Radio (RSSI) Sampling
//
// Iterate active slots; for each connected one, sample link RSSI via
// HCI_Read_RSSI and update cached telemetry. One publish per cycle
// emits the full collection.
//////////////////////////////////////////////////////////////

static void radio_publish_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  bool any_change = false;

  for (int i = 0; i < HEATERS_MAX; i++) {
    struct heater_slot *s = &slots[i];
    if (!s->conn) {
      continue;
    }
    uint16_t handle;
    if (bt_hci_get_conn_handle(s->conn, &handle) != 0) {
      continue;
    }
    struct net_buf *buf = bt_hci_cmd_alloc(K_FOREVER);
    if (!buf) {
      continue;
    }
    struct bt_hci_cp_read_rssi *cp = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(handle);
    struct net_buf *rsp = NULL;
    if (bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp) == 0 && rsp) {
      struct bt_hci_rp_read_rssi *rp = (void *)rsp->data;
      if (rp->status == 0) {
        s->data.ble_rssi_dbm = rp->rssi;
        s->data.timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks());
        any_change = true;
      }
      net_buf_unref(rsp);
    }
  }

  if (any_change) {
    publish_heater_state();
  }
  k_work_reschedule(&radio_publish_work, K_MSEC(RADIO_PUBLISH_INTERVAL_MS));
}

//////////////////////////////////////////////////////////////
// Send Helpers (by target name)
//////////////////////////////////////////////////////////////

static int slot_write(struct heater_slot *s, const uint8_t *buf, int len)
{
  if (!s || !s->conn || s->write_handle == 0) {
    return -ENOTCONN;
  }
  if (len <= 0) {
    return -EINVAL;
  }
  return bt_gatt_write_without_response(s->conn, s->write_handle, buf, len, false);
}

int heater_ble_send_power(const char *target, bool on)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0) {
    return -ENOTCONN;
  }

  /* CC 0xA1 is a TOGGLE — it ignores the requested direction. Guard
   * against redundant sends using the slot's cached telemetry. Treat
   * RUNNING / STARTING as "on", OFF / SHUTTING_DOWN as "off". */
  bool is_on = s->data.power == HEATER_POWER_RUNNING ||
               s->data.power == HEATER_POWER_STARTING;
  if (on == is_on) {
    return 0;
  }

  uint8_t buf[16];
  int pkt_len;

  if (!on && s->data.mode == HEATER_MODE_FAN && s->protocol->encode_set_mode) {
    /* CC fan mode needs 0xA4 — the generic toggle (0xA1) would switch
       to heating instead of off. */
    pkt_len = s->protocol->encode_set_mode(buf, sizeof(buf), HEATER_MODE_FAN);
  } else {
    pkt_len = s->protocol->encode_power(buf, sizeof(buf), on);
  }
  return slot_write(s, buf, pkt_len);
}

int heater_ble_send_set_temp(const char *target, int temp_c)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0) {
    return -ENOTCONN;
  }
  uint8_t buf[16];
  int pkt_len = s->protocol->encode_set_temp(buf, sizeof(buf), temp_c);
  return slot_write(s, buf, pkt_len);
}

int heater_ble_send_adjust_power(const char *target, int delta)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0) {
    return -ENOTCONN;
  }
  if (!s->protocol->encode_adjust_power) {
    return -ENOTSUP;
  }
  uint8_t buf[16];
  int pkt_len = s->protocol->encode_adjust_power(buf, sizeof(buf), delta);
  return slot_write(s, buf, pkt_len);
}

int heater_ble_send_set_mode(const char *target, enum heater_run_mode mode)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0) {
    return -ENOTCONN;
  }
  if (!s->protocol->encode_set_mode) {
    return -ENOTSUP;
  }
  uint8_t buf[16];
  int pkt_len = s->protocol->encode_set_mode(buf, sizeof(buf), mode);
  return slot_write(s, buf, pkt_len);
}

int heater_ble_send_altitude(const char *target)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0 ||
      !s->protocol->encode_altitude) {
    return -ENOTCONN;
  }
  uint8_t buf[16];
  int pkt_len = s->protocol->encode_altitude(buf, sizeof(buf));
  return slot_write(s, buf, pkt_len);
}

int heater_ble_send_set_auto_offsets(const char *target, int startup, int shutdown)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0 ||
      !s->protocol->encode_set_auto_offsets) {
    return -ENOTCONN;
  }
  uint8_t buf[16];
  int pkt_len = s->protocol->encode_set_auto_offsets(buf, sizeof(buf),
                                                     startup, shutdown);
  int ret = slot_write(s, buf, pkt_len);
  if (ret == 0) {
    k_work_schedule(&s->offset_query_work, K_MSEC(500));
  }
  return ret;
}

int heater_ble_send_query_auto_offsets(const char *target)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || !s->protocol || s->write_handle == 0 ||
      !s->protocol->encode_query_auto_offsets) {
    return -ENOTCONN;
  }
  uint8_t buf[16];
  int pkt_len = s->protocol->encode_query_auto_offsets(buf, sizeof(buf));
  return slot_write(s, buf, pkt_len);
}

int heater_ble_send_raw(const char *target, const uint8_t *data, uint8_t len)
{
  struct heater_slot *s = slot_for_name(target);
  if (!s || !s->conn || s->write_handle == 0) {
    return -ENOTCONN;
  }
  return slot_write(s, data, len);
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
  case HEATER_CMD_DISCONNECT:
    heater_ble_disconnect(cmd->target_name);
    break;
  case HEATER_CMD_POWER:
    heater_ble_send_power(cmd->target_name, cmd->power_on);
    break;
  case HEATER_CMD_SET_MODE:
    heater_ble_send_set_mode(cmd->target_name, cmd->mode);
    break;
  case HEATER_CMD_SET_TEMP:
    heater_ble_send_set_temp(cmd->target_name, cmd->temp);
    break;
  case HEATER_CMD_ADJUST_POWER:
    heater_ble_send_adjust_power(cmd->target_name, cmd->power_delta);
    break;
  case HEATER_CMD_ALTITUDE:
    heater_ble_send_altitude(cmd->target_name);
    break;
  case HEATER_CMD_SET_AUTO_OFFSETS:
    heater_ble_send_set_auto_offsets(cmd->target_name,
                                     cmd->auto_offsets.startup,
                                     cmd->auto_offsets.shutdown);
    break;
  case HEATER_CMD_QUERY_AUTO_OFFSETS:
    heater_ble_send_query_auto_offsets(cmd->target_name);
    break;
  case HEATER_CMD_RAW:
    heater_ble_send_raw(cmd->target_name, cmd->raw.data, cmd->raw.len);
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
