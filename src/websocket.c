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
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(websocket, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// HTTP Service (defined in http_api.c)
//////////////////////////////////////////////////////////////

extern const struct http_service_desc damper_http_service;

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define WS_MAX_CLIENTS 2
#define WS_BUF_SIZE 512
#define WS_RECV_BUF_SIZE 256
#define WS_RECV_TIMEOUT_MS 100
#define WS_SEND_TIMEOUT_MS 500

//////////////////////////////////////////////////////////////
// Client Context
//////////////////////////////////////////////////////////////

struct ws_client {
  int sock;
  struct k_work_delayable recv_work;
  uint8_t recv_buf[WS_RECV_BUF_SIZE];
};

static struct ws_client ws_clients[WS_MAX_CLIENTS];

//////////////////////////////////////////////////////////////
// Work Queue
//////////////////////////////////////////////////////////////

#define WS_STACK_SIZE 2048

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
// Zbus Subscriber — Push Updates
//////////////////////////////////////////////////////////////

ZBUS_SUBSCRIBER_DEFINE(ws_sub, 8);
ZBUS_CHAN_ADD_OBS(damper_data_chan, ws_sub, 5);
ZBUS_CHAN_ADD_OBS(heater_data_chan, ws_sub, 5);
ZBUS_CHAN_ADD_OBS(heater_devices_chan, ws_sub, 5);

#define WS_SUB_STACK_SIZE 2048
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
          "\"core_threshold\":%.1f,\"heater_name\":%s%s%s}",
          data.mode == DAMPER_MODE_AUTO ? "auto" : "manual",
          data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
          data.angle, data.inside_angle, data.outside_angle,
          data.core_threshold,
          data.heater_name[0] ? "\"" : "",
          data.heater_name[0] ? data.heater_name : "null",
          data.heater_name[0] ? "\"" : "");
    } else if (chan == &heater_data_chan) {
      struct heater_data data;
      zbus_chan_read(chan, &data, K_NO_WAIT);
      len = snprintf(ws_tx_buf, sizeof(ws_tx_buf),
          "{\"type\":\"heater\","
          "\"name\":%s%s%s,"
          "\"power\":\"%s\",\"step\":\"%s\",\"mode\":\"%s\","
          "\"exhaust_temp\":%.1f,\"ambient_temp\":%.1f,"
          "\"voltage\":%.1f,\"target_temp\":%d,"
          "\"power_level\":%d,\"error\":%d,"
          "\"altitude_mode\":%s,"
          "\"startup_offset\":%d,\"shutdown_offset\":%d,"
          "\"connected\":%s}",
          data.name[0] ? "\"" : "",
          data.name[0] ? data.name : "null",
          data.name[0] ? "\"" : "",
          heater_power_state_str(data.power),
          heater_run_step_str(data.step),
          heater_run_mode_str(data.mode),
          data.exhaust_temp_c, data.ambient_temp_c,
          data.voltage, data.target_temp,
          data.power_level, data.error_code,
          data.altitude_mode ? "true" : "false",
          data.startup_offset, data.shutdown_offset,
          data.connected ? "true" : "false");
    } else if (chan == &heater_devices_chan) {
      struct heater_devices devs;
      zbus_chan_read(chan, &devs, K_NO_WAIT);
      len = snprintf(ws_tx_buf, sizeof(ws_tx_buf),
          "{\"type\":\"heaters\",\"connected\":%d,\"devices\":[",
          devs.connected_index);
      for (int i = 0; i < devs.count && len < (int)sizeof(ws_tx_buf) - 80; i++) {
        if (i > 0) ws_tx_buf[len++] = ',';
        len += snprintf(ws_tx_buf + len, sizeof(ws_tx_buf) - len,
            "{\"name\":\"%s\",\"rssi\":%d,\"protocol\":\"%s\"}",
            devs.devices[i].name, devs.devices[i].rssi,
            devs.devices[i].protocol);
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
      struct damper_command cmd = {.type = DAMPER_CMD_SET_AUTO};
      zbus_chan_pub(&damper_command_chan, &cmd, K_MSEC(100));
    } else {
      ws_send_result(slot, false, "need angle or auto");
      return;
    }
    ws_send_result(slot, true, NULL);

  } else if (strcmp(type, "damper.config") == 0) {
    struct damper_config *cfg = damper_config_get();
    double inside, outside, threshold;

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

    struct damper_command cmd = {
        .type = DAMPER_CMD_SET_CONFIG,
        .inside_angle = inside,
        .outside_angle = outside,
        .core_threshold = threshold,
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
        "\"core_threshold\":%.1f,\"heater_name\":%s%s%s}",
        data.mode == DAMPER_MODE_AUTO ? "auto" : "manual",
        data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
        data.angle, data.inside_angle, data.outside_angle,
        data.core_threshold,
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
        "{\"type\":\"heaters\",\"connected\":%d,\"devices\":[",
        devs.connected_index);
    for (int i = 0; i < devs.count && len < (int)sizeof(ws_cmd_buf) - 80; i++) {
      if (i > 0) ws_cmd_buf[len++] = ',';
      len += snprintf(ws_cmd_buf + len, sizeof(ws_cmd_buf) - len,
          "{\"name\":\"%s\",\"rssi\":%d,\"protocol\":\"%s\"}",
          devs.devices[i].name, devs.devices[i].rssi,
          devs.devices[i].protocol);
    }
    len += snprintf(ws_cmd_buf + len, sizeof(ws_cmd_buf) - len, "]}");
    ws_send_to(slot, ws_cmd_buf, len);
    return;

  } else if (strcmp(type, "heater.status") == 0) {
    struct heater_data data;
    zbus_chan_read(&heater_data_chan, &data, K_NO_WAIT);
    int len = snprintf(ws_cmd_buf, sizeof(ws_cmd_buf),
        "{\"type\":\"heater\","
        "\"name\":%s%s%s,"
        "\"power\":\"%s\",\"step\":\"%s\",\"mode\":\"%s\","
        "\"exhaust_temp\":%.1f,\"ambient_temp\":%.1f,"
        "\"voltage\":%.1f,\"target_temp\":%d,"
        "\"power_level\":%d,\"error\":%d,\"connected\":%s}",
        data.name[0] ? "\"" : "",
        data.name[0] ? data.name : "null",
        data.name[0] ? "\"" : "",
        heater_power_state_str(data.power),
        heater_run_step_str(data.step),
        heater_run_mode_str(data.mode),
        data.exhaust_temp_c, data.ambient_temp_c,
        data.voltage, data.target_temp,
        data.power_level, data.error_code,
        data.connected ? "true" : "false");
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

  } else if (strcmp(type, "heater.command") == 0) {
    struct heater_data hstate;
    zbus_chan_read(&heater_data_chan, &hstate, K_NO_WAIT);
    if (!hstate.connected) {
      ws_send_result(slot, false, "not connected");
      return;
    }

    bool power_val;
    int int_val;
    char mode_str[16];
    struct heater_command cmd;

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
