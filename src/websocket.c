// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/websocket.h>
#include <zephyr/zbus/zbus.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <auto_damper/damper.h>
#include <auto_damper/heater.h>
#include <auto_damper/ota.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(websocket, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// HTTP Service (defined in http_api.c)
//////////////////////////////////////////////////////////////

extern const struct http_service_desc damper_http_service;

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define WS_MAX_CLIENTS 4
/* Sized for the worst case: heater_states message with all 10 slots
 * populated. Each per-heater JSON object runs ~350B, plus envelope. */
#define WS_BUF_SIZE 4096
#define WS_RECV_BUF_SIZE 256
#define WS_RECV_TIMEOUT_MS 100
#define WS_SEND_TIMEOUT_MS 500

//////////////////////////////////////////////////////////////
// Client Context
//////////////////////////////////////////////////////////////

struct ws_client {
  int sock;
  bool sent_initial;
  struct k_work_delayable recv_work;
  uint8_t recv_buf[WS_RECV_BUF_SIZE];
};

static struct ws_client ws_clients[WS_MAX_CLIENTS];

//////////////////////////////////////////////////////////////
// Work Queue
//////////////////////////////////////////////////////////////

#define WS_STACK_SIZE 4096

K_THREAD_STACK_DEFINE(ws_work_stack, WS_STACK_SIZE);
static struct k_work_q ws_work_q;

//////////////////////////////////////////////////////////////
// Broadcast Helpers
//////////////////////////////////////////////////////////////

static char ws_tx_buf[WS_BUF_SIZE];
static struct k_mutex ws_tx_mutex;

static void ws_broadcast(const char *json, int len)
{
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (ws_clients[i].sock < 0) {
      continue;
    }
    int ret = websocket_send_msg(ws_clients[i].sock,
        (const uint8_t *)json, len,
        WEBSOCKET_OPCODE_DATA_TEXT, false, true,
        WS_SEND_TIMEOUT_MS);
    if (ret < 0 && ret != -EAGAIN) {
      LOG_DBG("WS send failed slot %d: %d", i, ret);
    }
  }
}

static void ws_send_to(int slot, const char *json, int len)
{
  if (slot < 0 || slot >= WS_MAX_CLIENTS || ws_clients[slot].sock < 0) {
    return;
  }
  websocket_send_msg(ws_clients[slot].sock,
      (const uint8_t *)json, len,
      WEBSOCKET_OPCODE_DATA_TEXT, false, true,
      WS_SEND_TIMEOUT_MS);
}

//////////////////////////////////////////////////////////////
// Zbus Subscriber - Push Updates
//////////////////////////////////////////////////////////////

ZBUS_SUBSCRIBER_DEFINE(ws_sub, 8);
ZBUS_CHAN_ADD_OBS(damper_data_chan, ws_sub, 5);
ZBUS_CHAN_ADD_OBS(heater_states_chan, ws_sub, 5);
ZBUS_CHAN_ADD_OBS(heater_devices_chan, ws_sub, 5);

/* Serialize one heater_data into the JSON array entry. Returns bytes
 * written. Caller ensures buf has at least 400 bytes remaining. */
static int ws_format_heater(char *buf, size_t cap, const struct heater_data *h)
{
  return snprintf(buf, cap,
      "{\"name\":\"%s\","
      "\"power\":\"%s\",\"step\":\"%s\",\"mode\":\"%s\","
      "\"core_temp\":%.1f,\"ambient_temp\":%.1f,"
      "\"voltage\":%.1f,\"target_temp\":%d,"
      "\"power_level\":%d,\"error\":%d,"
      "\"altitude_mode\":%s,"
      "\"startup_offset\":%d,\"shutdown_offset\":%d,"
      "\"ble_rssi_dbm\":%d,"
      "\"connected\":%s}",
      h->name,
      heater_power_state_str(h->power),
      heater_run_step_str(h->step),
      heater_run_mode_str(h->mode),
      h->core_temp_c, h->ambient_temp_c,
      h->voltage, h->target_temp,
      h->power_level, h->error_code,
      h->altitude_mode ? "true" : "false",
      h->startup_offset, h->shutdown_offset,
      (int)h->ble_rssi_dbm,
      h->connected ? "true" : "false");
}

static bool ws_is_heater_connected(const char *name)
{
  struct heater_states st;
  zbus_chan_read(&heater_states_chan, &st, K_NO_WAIT);
  for (int i = 0; i < st.count && i < HEATERS_MAX; i++) {
    if (st.heaters[i].connected &&
        strcmp(st.heaters[i].name, name) == 0) {
      return true;
    }
  }
  return false;
}

#define WS_SUB_STACK_SIZE 4096
#define WS_SUB_PRIORITY 7

static void ws_subscriber_thread(void *p1, void *p2, void *p3)
{
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  const struct zbus_channel *chan;

  while (1) {
    if (zbus_sub_wait(&ws_sub, &chan, K_FOREVER) != 0) {
      continue;
    }

    k_mutex_lock(&ws_tx_mutex, K_FOREVER);

    int len = 0;

    if (chan == &damper_data_chan) {
      struct damper_data data;
      zbus_chan_read(chan, &data, K_NO_WAIT);
      len = snprintf(ws_tx_buf, sizeof(ws_tx_buf),
          "{\"type\":\"damper\",\"mode\":\"%s\",\"route\":\"%s\","
          "\"angle\":%.1f,\"inside_angle\":%.1f,\"outside_angle\":%.1f,"
          "\"core_threshold\":%.1f,\"cool_setpoint\":%.1f,"
          "\"cool_hysteresis\":%.1f,\"heater_name\":%s%s%s}",
          data.mode == DAMPER_MODE_AUTO ? "auto" :
          data.mode == DAMPER_MODE_HEATING ? "heating" :
          data.mode == DAMPER_MODE_COOLING ? "cooling" : "manual",
          data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
          data.angle, data.inside_angle, data.outside_angle,
          data.core_threshold, data.cool_setpoint,
          data.cool_hysteresis,
          data.heater_name[0] ? "\"" : "",
          data.heater_name[0] ? data.heater_name : "null",
          data.heater_name[0] ? "\"" : "");
    } else if (chan == &heater_states_chan) {
      /* heater_states is a large channel (~1.2KB); read in place via
       * claim to avoid stack-allocating the full struct here. */
      if (zbus_chan_claim(chan, K_MSEC(100)) == 0) {
        const struct heater_states *states = zbus_chan_const_msg(chan);
        len = snprintf(ws_tx_buf, sizeof(ws_tx_buf),
                       "{\"type\":\"heater_states\",\"heaters\":[");
        for (int i = 0; i < states->count && i < HEATERS_MAX; i++) {
          if (len > (int)sizeof(ws_tx_buf) - 400) break;
          if (i > 0) ws_tx_buf[len++] = ',';
          len += ws_format_heater(ws_tx_buf + len,
                                  sizeof(ws_tx_buf) - len,
                                  &states->heaters[i]);
        }
        len += snprintf(ws_tx_buf + len, sizeof(ws_tx_buf) - len, "]}");
        zbus_chan_finish(chan);
      }
    } else if (chan == &heater_devices_chan) {
      struct heater_devices devs;
      zbus_chan_read(chan, &devs, K_NO_WAIT);
      len = snprintf(ws_tx_buf, sizeof(ws_tx_buf),
          "{\"type\":\"heaters\",\"devices\":[");
      for (int i = 0; i < devs.count && len < (int)sizeof(ws_tx_buf) - 100; i++) {
        if (i > 0) ws_tx_buf[len++] = ',';
        len += snprintf(ws_tx_buf + len, sizeof(ws_tx_buf) - len,
            "{\"name\":\"%s\",\"rssi\":%d,\"protocol\":\"%s\","
            "\"connected\":%s}",
            devs.devices[i].name, devs.devices[i].rssi,
            devs.devices[i].protocol,
            ws_is_heater_connected(devs.devices[i].name) ? "true" : "false");
      }
      len += snprintf(ws_tx_buf + len, sizeof(ws_tx_buf) - len, "]}");
    }

    if (len > 0) {
      ws_broadcast(ws_tx_buf, len);
    }

    k_mutex_unlock(&ws_tx_mutex);
  }
}

K_THREAD_DEFINE(ws_sub_thread, WS_SUB_STACK_SIZE,
                ws_subscriber_thread, NULL, NULL, NULL,
                WS_SUB_PRIORITY, 0, 0);

//////////////////////////////////////////////////////////////
// Minimal JSON Helpers (for command parsing)
//////////////////////////////////////////////////////////////

static bool ws_json_get_string(const char *body, const char *key,
                               char *out, size_t out_len)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  char *p = strstr(body, search);
  if (!p) return false;
  char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;
  char *q1 = strchr(colon, '"');
  if (!q1) return false;
  char *q2 = strchr(q1 + 1, '"');
  if (!q2 || (size_t)(q2 - q1 - 1) >= out_len) return false;
  memcpy(out, q1 + 1, q2 - q1 - 1);
  out[q2 - q1 - 1] = '\0';
  return true;
}

static bool ws_json_get_double(const char *body, const char *key, double *out)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  char *p = strstr(body, search);
  if (!p) return false;
  char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;
  *out = strtod(colon + 1, NULL);
  return true;
}

static bool ws_json_get_int(const char *body, const char *key, int *out)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  char *p = strstr(body, search);
  if (!p) return false;
  char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;
  *out = atoi(colon + 1);
  return true;
}

static bool ws_json_get_bool(const char *body, const char *key, bool *out)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  char *p = strstr(body, search);
  if (!p) return false;
  char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;
  while (*colon == ':' || *colon == ' ') colon++;
  if (strncmp(colon, "true", 4) == 0) { *out = true; return true; }
  if (strncmp(colon, "false", 5) == 0) { *out = false; return true; }
  return false;
}

//////////////////////////////////////////////////////////////
// Command Processing
//////////////////////////////////////////////////////////////

static char ws_cmd_buf[WS_BUF_SIZE];

//////////////////////////////////////////////////////////////
// OTA worker thread
//
// OTA download is slow (~30s for 800KB over TLS) and must not run on
// the WS recv work item. Dedicated thread waits on a semaphore; the
// `ota.check` command gives the sem, the thread runs the update,
// progress callbacks broadcast as `{type:"ota", ...}` to every
// connected WS client.
//////////////////////////////////////////////////////////////

static K_SEM_DEFINE(ws_ota_trigger, 0, 1);
static char ws_ota_progress_buf[512];

/* What ota.check / ota.install set so the worker knows which to run. */
static enum { OTA_REQ_NONE, OTA_REQ_CHECK, OTA_REQ_INSTALL } ws_ota_request;

static const char *ws_ota_state_str(enum ota_state s)
{
  switch (s) {
  case OTA_STATE_IDLE:             return "idle";
  case OTA_STATE_CHECKING:         return "checking";
  case OTA_STATE_UP_TO_DATE:       return "up_to_date";
  case OTA_STATE_UPDATE_AVAILABLE: return "update_available";
  case OTA_STATE_DOWNLOADING:      return "downloading";
  case OTA_STATE_VERIFYING:        return "verifying";
  case OTA_STATE_SWAP_PENDING:     return "swap_pending";
  case OTA_STATE_FAILED:           return "failed";
  default:                         return "unknown";
  }
}

static void ws_ota_progress_cb(const struct ota_progress *p)
{
  k_mutex_lock(&ws_tx_mutex, K_FOREVER);
  int len = snprintf(ws_ota_progress_buf, sizeof(ws_ota_progress_buf),
      "{\"type\":\"ota\",\"state\":\"%s\","
      "\"running_version\":\"%s\","
      "\"available_version\":\"%s\","
      "\"bytes_received\":%u,\"bytes_total\":%u,"
      "\"error\":\"%s\"}",
      ws_ota_state_str(p->state),
      p->running_version,
      p->available_version,
      p->bytes_received,
      p->bytes_total,
      p->error);
  if (len > 0) {
    ws_broadcast(ws_ota_progress_buf, len);
  }
  k_mutex_unlock(&ws_tx_mutex);
}

static void ws_ota_thread_fn(void *a, void *b, void *c)
{
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  while (1) {
    k_sem_take(&ws_ota_trigger, K_FOREVER);
    int req = ws_ota_request;
    ws_ota_request = OTA_REQ_NONE;
    if (req == OTA_REQ_CHECK) {
      ota_check(ws_ota_progress_cb);
    } else if (req == OTA_REQ_INSTALL) {
      ota_install_pending(ws_ota_progress_cb);
    }
  }
}

/* 8KB stack matches the shell where the OTA already runs successfully -
 * mbedTLS handshake is the largest consumer. */
K_THREAD_DEFINE(ws_ota_tid, 8192, ws_ota_thread_fn, NULL, NULL, NULL,
                7, 0, 0);

static void ws_send_result(int slot, bool ok, const char *error)
{
  int len;
  if (ok) {
    len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf), "{\"type\":\"result\",\"ok\":true}");
  } else {
    len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf),
        "{\"type\":\"result\",\"ok\":false,\"error\":\"%s\"}", error);
  }
  ws_send_to(slot, ws_cmd_buf, len);
}

static int find_heater_by_name(const char *name)
{
  struct heater_devices devs;
  zbus_chan_read(&heater_devices_chan, &devs, K_NO_WAIT);
  for (int i = 0; i < devs.count; i++) {
    if (strcmp(devs.devices[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static void ws_handle_command(int slot, const char *msg, int msg_len)
{
  char type[32];
  if (!ws_json_get_string(msg, "type", type, sizeof(type))) {
    ws_send_result(slot, false, "missing type");
    return;
  }

  if (strcmp(type, "damper.set") == 0) {
    double angle;
    bool auto_mode;

    char mode_str[16];
    if (ws_json_get_double(msg, "angle", &angle)) {
      struct servo_config *cfg = servo_config_get();
      if (angle < 0 || angle > cfg->max_deg) {
        ws_send_result(slot, false, "angle out of range");
        return;
      }
      struct damper_command cmd = {
          .type = DAMPER_CMD_SET_ANGLE,
          .angle = angle,
      };
      zbus_chan_pub(&damper_command_chan, &cmd, K_MSEC(100));
    } else if (ws_json_get_bool(msg, "auto", &auto_mode) && auto_mode) {
      struct damper_command cmd = {.type = DAMPER_CMD_SET_MODE,
                                   .mode = DAMPER_MODE_AUTO};
      zbus_chan_pub(&damper_command_chan, &cmd, K_MSEC(100));
    } else if (ws_json_get_string(msg, "mode", mode_str, sizeof(mode_str))) {
      struct damper_command cmd = {.type = DAMPER_CMD_SET_MODE};
      if (strcmp(mode_str, "auto") == 0) cmd.mode = DAMPER_MODE_AUTO;
      else if (strcmp(mode_str, "manual") == 0) cmd.mode = DAMPER_MODE_MANUAL;
      else if (strcmp(mode_str, "heating") == 0) cmd.mode = DAMPER_MODE_HEATING;
      else if (strcmp(mode_str, "cooling") == 0) cmd.mode = DAMPER_MODE_COOLING;
      else { ws_send_result(slot, false, "invalid mode"); return; }
      zbus_chan_pub(&damper_command_chan, &cmd, K_MSEC(100));
    } else {
      ws_send_result(slot, false, "need angle, auto, or mode");
      return;
    }
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "damper.config") == 0) {
    struct damper_config *cfg = damper_config_get();
    double inside, outside, threshold, cool_sp, cool_hy;

    if (ws_json_get_double(msg, "inside_angle", &inside)) {
      cfg->inside_angle = inside;
    } else {
      inside = cfg->inside_angle;
    }
    if (ws_json_get_double(msg, "outside_angle", &outside)) {
      cfg->outside_angle = outside;
    } else {
      outside = cfg->outside_angle;
    }
    if (ws_json_get_double(msg, "core_threshold", &threshold)) {
      cfg->core_threshold = threshold;
    } else {
      threshold = cfg->core_threshold;
    }
    if (ws_json_get_double(msg, "cool_setpoint", &cool_sp)) {
      cfg->cool_setpoint = cool_sp;
    } else {
      cool_sp = cfg->cool_setpoint;
    }
    if (ws_json_get_double(msg, "cool_hysteresis", &cool_hy)) {
      cfg->cool_hysteresis = cool_hy;
    } else {
      cool_hy = cfg->cool_hysteresis;
    }

    struct damper_command cmd = {
        .type = DAMPER_CMD_SET_CONFIG,
        .inside_angle = inside,
        .outside_angle = outside,
        .core_threshold = threshold,
        .cool_setpoint = cool_sp,
        .cool_hysteresis = cool_hy,
    };
    zbus_chan_pub(&damper_command_chan, &cmd, K_MSEC(100));
    int rc = damper_last_config_result();
    ws_send_result(slot, rc == 0, rc < 0 ? "save failed" : NULL);

  } else if (strcmp(type, "damper.heater") == 0) {
    char name[32] = "";
    ws_json_get_string(msg, "name", name, sizeof(name));
    struct damper_command cmd = {.type = DAMPER_CMD_SET_HEATER};
    strncpy(cmd.heater_name, name, sizeof(cmd.heater_name) - 1);
    zbus_chan_pub(&damper_command_chan, &cmd, K_MSEC(100));
    int rc = damper_last_config_result();
    ws_send_result(slot, rc == 0, rc < 0 ? "save failed" : NULL);

  } else if (strcmp(type, "damper.status") == 0) {
    struct damper_data data;
    zbus_chan_read(&damper_data_chan, &data, K_NO_WAIT);
    int len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf),
        "{\"type\":\"damper\",\"mode\":\"%s\",\"route\":\"%s\","
        "\"angle\":%.1f,\"inside_angle\":%.1f,\"outside_angle\":%.1f,"
        "\"core_threshold\":%.1f,\"cool_setpoint\":%.1f,"
        "\"cool_hysteresis\":%.1f,\"heater_name\":%s%s%s}",
        data.mode == DAMPER_MODE_AUTO ? "auto" :
        data.mode == DAMPER_MODE_HEATING ? "heating" :
        data.mode == DAMPER_MODE_COOLING ? "cooling" : "manual",
        data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
        data.angle, data.inside_angle, data.outside_angle,
        data.core_threshold, data.cool_setpoint,
        data.cool_hysteresis,
        data.heater_name[0] ? "\"" : "",
        data.heater_name[0] ? data.heater_name : "null",
        data.heater_name[0] ? "\"" : "");
    ws_send_to(slot, ws_cmd_buf, len);
    return;

  } else if (strcmp(type, "servo.set") == 0) {
    struct servo_config *cfg = servo_config_get();
    int ival;
    double dval;
    if (ws_json_get_int(msg, "min_us", &ival)) cfg->min_us = (uint32_t)ival;
    if (ws_json_get_int(msg, "max_us", &ival)) cfg->max_us = (uint32_t)ival;
    if (ws_json_get_double(msg, "max_deg", &dval)) cfg->max_deg = dval;
    servo_config_save();
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "heaters.scan") == 0) {
    int timeout = 5;
    ws_json_get_int(msg, "timeout", &timeout);
    if (timeout < 1 || timeout > 30) timeout = 5;
    struct heater_command cmd = {.type = HEATER_CMD_SCAN, .scan_timeout = timeout};
    zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(100));
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "heaters.list") == 0) {
    struct heater_devices devs;
    zbus_chan_read(&heater_devices_chan, &devs, K_NO_WAIT);
    int len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf),
        "{\"type\":\"heaters\",\"devices\":[");
    for (int i = 0; i < devs.count && len < (int)sizeof(ws_cmd_buf) - 100; i++) {
      if (i > 0) ws_cmd_buf[len++] = ',';
      len += snprintf(ws_cmd_buf + len, sizeof(ws_cmd_buf) - len,
          "{\"name\":\"%s\",\"rssi\":%d,\"protocol\":\"%s\","
          "\"connected\":%s}",
          devs.devices[i].name, devs.devices[i].rssi,
          devs.devices[i].protocol,
          ws_is_heater_connected(devs.devices[i].name) ? "true" : "false");
    }
    len += snprintf(ws_cmd_buf + len, sizeof(ws_cmd_buf) - len, "]}");
    ws_send_to(slot, ws_cmd_buf, len);
    return;

  } else if (strcmp(type, "heaters.connect") == 0) {
    char name[32];
    if (!ws_json_get_string(msg, "name", name, sizeof(name))) {
      ws_send_result(slot, false, "need name");
      return;
    }
    int idx = find_heater_by_name(name);
    if (idx < 0) {
      ws_send_result(slot, false, "heater not found");
      return;
    }
    struct heater_command cmd = {.type = HEATER_CMD_CONNECT, .connect_index = idx};
    zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(100));
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "heaters.disconnect") == 0) {
    char target[32];
    if (!ws_json_get_string(msg, "target", target, sizeof(target))) {
      ws_send_result(slot, false, "need target");
      return;
    }
    struct heater_command cmd = {.type = HEATER_CMD_DISCONNECT};
    strncpy(cmd.target_name, target, sizeof(cmd.target_name) - 1);
    zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(100));
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "heater.command") == 0) {
    char target[32];
    if (!ws_json_get_string(msg, "target", target, sizeof(target))) {
      ws_send_result(slot, false, "need target");
      return;
    }
    if (!ws_is_heater_connected(target)) {
      ws_send_result(slot, false, "not connected");
      return;
    }

    bool power_val;
    int int_val;
    char mode_str[16];
    struct heater_command cmd = {0};
    strncpy(cmd.target_name, target, sizeof(cmd.target_name) - 1);

    if (ws_json_get_bool(msg, "power", &power_val)) {
      cmd.type = HEATER_CMD_POWER;
      cmd.power_on = power_val;
    } else if (ws_json_get_string(msg, "mode", mode_str, sizeof(mode_str))) {
      cmd.type = HEATER_CMD_SET_MODE;
      if (strcmp(mode_str, "manual") == 0) cmd.mode = HEATER_MODE_MANUAL;
      else if (strcmp(mode_str, "automatic") == 0) cmd.mode = HEATER_MODE_AUTOMATIC;
      else if (strcmp(mode_str, "fan") == 0) cmd.mode = HEATER_MODE_FAN;
      else { ws_send_result(slot, false, "invalid mode"); return; }
    } else if (ws_json_get_int(msg, "temp", &int_val)) {
      if (int_val < 8 || int_val > 36) {
        ws_send_result(slot, false, "temp must be 8-36");
        return;
      }
      cmd.type = HEATER_CMD_SET_TEMP;
      cmd.temp = int_val;
    } else if (ws_json_get_int(msg, "power_level", &int_val)) {
      cmd.type = HEATER_CMD_ADJUST_POWER;
      cmd.power_delta = int_val;
    } else if (ws_json_get_bool(msg, "altitude", &power_val)) {
      cmd.type = HEATER_CMD_ALTITUDE;
    } else if (ws_json_get_int(msg, "startup_offset", &int_val)) {
      cmd.type = HEATER_CMD_SET_AUTO_OFFSETS;
      cmd.auto_offsets.startup = int_val;
      if (!ws_json_get_int(msg, "shutdown_offset", &int_val)) {
        ws_send_result(slot, false, "need shutdown_offset");
        return;
      }
      cmd.auto_offsets.shutdown = int_val;
    } else {
      ws_send_result(slot, false, "need power, mode, temp, power_level, altitude, or offsets");
      return;
    }
    zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(100));

    /* Re-broadcast a pending notification to all clients so every
     * connected UI puts a spinner on the relevant button until the
     * heater's next telemetry update reflects the new state. */
    char pending_buf[160];
    int plen = 0;
    switch (cmd.type) {
    case HEATER_CMD_POWER:
      plen = snprintf(pending_buf, sizeof(pending_buf),
          "{\"type\":\"heater.pending\",\"target\":\"%s\",\"field\":\"power\",\"value\":%s}",
          target, cmd.power_on ? "true" : "false");
      break;
    case HEATER_CMD_SET_MODE: {
      const char *m = cmd.mode == HEATER_MODE_MANUAL ? "manual" :
                      cmd.mode == HEATER_MODE_AUTOMATIC ? "automatic" :
                      cmd.mode == HEATER_MODE_FAN ? "fan" : "";
      plen = snprintf(pending_buf, sizeof(pending_buf),
          "{\"type\":\"heater.pending\",\"target\":\"%s\",\"field\":\"mode\",\"value\":\"%s\"}",
          target, m);
      break;
    }
    case HEATER_CMD_ALTITUDE:
      plen = snprintf(pending_buf, sizeof(pending_buf),
          "{\"type\":\"heater.pending\",\"target\":\"%s\",\"field\":\"altitude\"}",
          target);
      break;
    case HEATER_CMD_ADJUST_POWER:
      plen = snprintf(pending_buf, sizeof(pending_buf),
          "{\"type\":\"heater.pending\",\"target\":\"%s\",\"field\":\"power_level\"}",
          target);
      break;
    case HEATER_CMD_SET_AUTO_OFFSETS:
      plen = snprintf(pending_buf, sizeof(pending_buf),
          "{\"type\":\"heater.pending\",\"target\":\"%s\",\"field\":\"auto_offsets\"}",
          target);
      break;
    default:
      break;
    }
    if (plen > 0) {
      ws_broadcast(pending_buf, plen);
    }

    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "ota.status") == 0) {
    /* Reply with just the running version + IDLE state so the UI
     * has something to show before an update is triggered. */
    char running[16];
    ota_get_running_version(running, sizeof(running));
    int len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf),
        "{\"type\":\"ota\",\"state\":\"idle\","
        "\"running_version\":\"%s\","
        "\"available_version\":\"\",\"bytes_received\":0,"
        "\"bytes_total\":0,\"error\":\"\"}",
        running);
    ws_send_to(slot, ws_cmd_buf, len);
    return;

  } else if (strcmp(type, "ota.check") == 0) {
    /* Lightweight check - fetch manifest, compare versions, broadcast
     * the result. Does NOT install. User then clicks "Install" which
     * triggers ota.install. */
    if (k_sem_count_get(&ws_ota_trigger) > 0) {
      ws_send_result(slot, false, "ota already in progress");
      return;
    }
    ws_ota_request = OTA_REQ_CHECK;
    k_sem_give(&ws_ota_trigger);
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "ota.install") == 0) {
    /* User confirmed the update - download, verify, swap, reboot. */
    if (k_sem_count_get(&ws_ota_trigger) > 0) {
      ws_send_result(slot, false, "ota already in progress");
      return;
    }
    ws_ota_request = OTA_REQ_INSTALL;
    k_sem_give(&ws_ota_trigger);
    ws_send_result(slot, true, NULL);

  } else {
    ws_send_result(slot, false, "unknown command type");
  }
}

//////////////////////////////////////////////////////////////
// Receive Work Handler
//////////////////////////////////////////////////////////////

static void ws_recv_handler(struct k_work *work)
{
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct ws_client *client = CONTAINER_OF(dwork, struct ws_client, recv_work);

  int slot = (int)(client - ws_clients);

  /* On the first tick after handshake, push the OTA snapshot so the
   * UI has the running version on initial load - without this the
   * frontend would have to wait for an idle ota.status response which
   * could race with the WS open event. */
  if (slot >= 0 && slot < WS_MAX_CLIENTS && client->sock >= 0 &&
      !client->sent_initial) {
    client->sent_initial = true;
    char running[16];
    ota_get_running_version(running, sizeof(running));
    int len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf),
        "{\"type\":\"ota\",\"state\":\"idle\","
        "\"running_version\":\"%s\","
        "\"available_version\":\"\",\"bytes_received\":0,"
        "\"bytes_total\":0,\"error\":\"\"}",
        running);
    if (len > 0) ws_send_to(slot, ws_cmd_buf, len);
  }
  if (slot < 0 || slot >= WS_MAX_CLIENTS || client->sock < 0) {
    return;
  }

  uint64_t remaining;
  uint32_t msg_type;

  int ret = websocket_recv_msg(client->sock, client->recv_buf,
      sizeof(client->recv_buf) - 1, &msg_type, &remaining,
      WS_RECV_TIMEOUT_MS);

  if (ret < 0) {
    if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
      goto reschedule;
    }
    LOG_INF("WS client disconnected (slot %d, err %d)", slot, ret);
    websocket_unregister(client->sock);
    client->sock = -1;
    return;
  }

  if (ret == 0) {
    LOG_INF("WS client closed (slot %d)", slot);
    websocket_unregister(client->sock);
    client->sock = -1;
    return;
  }

  client->recv_buf[ret] = '\0';
  ws_handle_command(slot, (const char *)client->recv_buf, ret);

reschedule:
  k_work_reschedule_for_queue(&ws_work_q, &client->recv_work, K_MSEC(10));
}

//////////////////////////////////////////////////////////////
// WebSocket Setup Callback
//////////////////////////////////////////////////////////////

static int ws_setup(int ws_socket, struct http_request_ctx *req,
                    void *user_data)
{
  int slot = -1;
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (ws_clients[i].sock < 0) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    LOG_WRN("No free WS slots");
    return -ENOSPC;
  }

  ws_clients[slot].sock = ws_socket;
  ws_clients[slot].sent_initial = false;
  LOG_INF("WS client connected (slot %d, fd %d)", slot, ws_socket);

  k_work_reschedule_for_queue(&ws_work_q, &ws_clients[slot].recv_work,
      K_MSEC(100));

  return 0;
}

//////////////////////////////////////////////////////////////
// HTTP Resource
//////////////////////////////////////////////////////////////

static uint8_t ws_data_buf[WS_RECV_BUF_SIZE];

static struct http_resource_detail_websocket ws_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_WEBSOCKET,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = ws_setup,
    .data_buffer = ws_data_buf,
    .data_buffer_len = sizeof(ws_data_buf),
};

HTTP_RESOURCE_DEFINE(ws_res, damper_http_service,
                     "/api/ws", &ws_detail);

//////////////////////////////////////////////////////////////
// Init
//////////////////////////////////////////////////////////////

static int ws_module_init(void)
{
  struct k_work_queue_config cfg = {.name = "ws_work"};

  k_mutex_init(&ws_tx_mutex);

  k_work_queue_init(&ws_work_q);
  k_work_queue_start(&ws_work_q, ws_work_stack, WS_STACK_SIZE, 6, &cfg);

  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    ws_clients[i].sock = -1;
    k_work_init_delayable(&ws_clients[i].recv_work, ws_recv_handler);
  }

  LOG_INF("WebSocket endpoint ready at /api/ws");
  return 0;
}

SYS_INIT(ws_module_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
