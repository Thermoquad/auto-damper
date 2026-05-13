// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/zbus/zbus.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <auto_damper/damper.h>
#include <auto_damper/heater.h>
#include <auto_damper/wifi.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(http_api, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define JSON_BUF_SIZE 512
#define BODY_BUF_SIZE 128
#define PUB_TIMEOUT K_MSEC(100)

//////////////////////////////////////////////////////////////
// HTTP Service
//////////////////////////////////////////////////////////////

#define HTTP_PORT 80

static uint16_t http_port = HTTP_PORT;
static bool server_running;

HTTP_SERVICE_DEFINE(damper_http_service, NULL, &http_port,
                    CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL, NULL);

//////////////////////////////////////////////////////////////
// JSON Helpers
//////////////////////////////////////////////////////////////

static int send_json(struct http_response_ctx *rsp, char *buf, int len)
{
  rsp->body = (const uint8_t *)buf;
  rsp->body_len = len;
  rsp->final_chunk = true;
  rsp->status = HTTP_200_OK;
  rsp->header_count = 0;
  return 0;
}

static int send_json_status(struct http_response_ctx *rsp, char *buf,
                            int len, int status_code)
{
  rsp->body = (const uint8_t *)buf;
  rsp->body_len = len;
  rsp->final_chunk = true;
  rsp->header_count = 0;

  switch (status_code) {
  case 400: rsp->status = HTTP_400_BAD_REQUEST; break;
  case 404: rsp->status = HTTP_404_NOT_FOUND; break;
  case 409: rsp->status = HTTP_409_CONFLICT; break;
  case 501: rsp->status = HTTP_501_NOT_IMPLEMENTED; break;
  default:  rsp->status = HTTP_500_INTERNAL_SERVER_ERROR; break;
  }
  return 0;
}

static int send_error(struct http_response_ctx *rsp, char *buf,
                      int status_code, const char *message)
{
  int len = snprintf(buf, JSON_BUF_SIZE, "{\"error\":\"%s\"}", message);
  return send_json_status(rsp, buf, len, status_code);
}

static int send_ok(struct http_response_ctx *rsp, char *buf)
{
  int len = snprintf(buf, JSON_BUF_SIZE, "{\"ok\":true}");
  return send_json(rsp, buf, len);
}

//////////////////////////////////////////////////////////////
// Minimal JSON Parser Helpers
//////////////////////////////////////////////////////////////

static bool json_get_string(const char *body, const char *key,
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

static bool json_get_double(const char *body, const char *key, double *out)
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

static bool json_get_int(const char *body, const char *key, int *out)
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

static bool json_get_bool(const char *body, const char *key, bool *out)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  char *p = strstr(body, search);
  if (!p) return false;

  char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;

  while (*colon == ':' || *colon == ' ') colon++;
  if (strncmp(colon, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (strncmp(colon, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

static int parse_body(const struct http_request_ctx *req, char *buf,
                      size_t buf_size)
{
  if (req->data_len == 0 || req->data_len >= buf_size) {
    return -EINVAL;
  }
  memcpy(buf, req->data, req->data_len);
  buf[req->data_len] = '\0';
  return 0;
}

//////////////////////////////////////////////////////////////
// Path Helpers
//////////////////////////////////////////////////////////////

static bool extract_path_name(const char *url, const char *prefix,
                              char *out, size_t out_len)
{
  size_t prefix_len = strlen(prefix);

  if (strncmp(url, prefix, prefix_len) != 0) {
    return false;
  }

  const char *name = url + prefix_len;
  size_t name_len = strlen(name);

  if (name_len == 0 || name_len >= out_len) {
    return false;
  }

  memcpy(out, name, name_len);
  out[name_len] = '\0';
  return true;
}

//////////////////////////////////////////////////////////////
// /api/damper* — Unified Damper Handler
//////////////////////////////////////////////////////////////

static char damper_buf[JSON_BUF_SIZE];
static char damper_body[BODY_BUF_SIZE];
static char servo_buf[JSON_BUF_SIZE];
static char servo_body[BODY_BUF_SIZE];

static int damper_state_get(struct http_response_ctx *rsp)
{
  struct damper_data data = {0};
  zbus_chan_read(&damper_data_chan, &data, PUB_TIMEOUT);

  int len = snprintf(damper_buf, sizeof(damper_buf),
      "{\"mode\":\"%s\",\"route\":\"%s\",\"angle\":%.1f,"
      "\"inside_angle\":%.1f,\"outside_angle\":%.1f,"
      "\"core_threshold\":%.1f,\"heater_name\":%s%s%s}",
      data.mode == DAMPER_MODE_AUTO ? "auto" : "manual",
      data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
      data.angle, data.inside_angle, data.outside_angle,
      data.core_threshold,
      data.heater_name[0] ? "\"" : "",
      data.heater_name[0] ? data.heater_name : "null",
      data.heater_name[0] ? "\"" : "");

  return send_json(rsp, damper_buf, len);
}

static int damper_state_patch(const struct http_request_ctx *req,
                              struct http_response_ctx *rsp)
{
  if (parse_body(req, damper_body, sizeof(damper_body)) < 0) {
    return send_error(rsp, damper_buf, 400, "invalid body");
  }

  double angle;
  bool auto_mode;

  if (json_get_double(damper_body, "angle", &angle)) {
    struct servo_config *cfg = servo_config_get();
    if (angle < 0 || angle > cfg->max_deg) {
      return send_error(rsp, damper_buf, 400, "angle out of range");
    }
    struct damper_command cmd = {
        .type = DAMPER_CMD_SET_ANGLE,
        .angle = angle,
    };
    zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);
  } else if (json_get_bool(damper_body, "auto", &auto_mode) && auto_mode) {
    struct damper_command cmd = {.type = DAMPER_CMD_SET_AUTO};
    zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);
  } else {
    return send_error(rsp, damper_buf, 400, "need angle or auto field");
  }

  return send_ok(rsp, damper_buf);
}

static int damper_config_handler(const struct http_request_ctx *req,
                                 struct http_response_ctx *rsp)
{
  if (parse_body(req, damper_body, sizeof(damper_body)) < 0) {
    return send_error(rsp, damper_buf, 400, "invalid body");
  }

  struct damper_config *cfg = damper_config_get();
  double inside, outside, threshold;

  inside = cfg->inside_angle;
  outside = cfg->outside_angle;
  threshold = cfg->core_threshold;

  json_get_double(damper_body, "inside_angle", &inside);
  json_get_double(damper_body, "outside_angle", &outside);
  json_get_double(damper_body, "core_threshold", &threshold);

  struct damper_command cmd = {
      .type = DAMPER_CMD_SET_CONFIG,
      .inside_angle = inside,
      .outside_angle = outside,
      .core_threshold = threshold,
  };
  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  return send_ok(rsp, damper_buf);
}

static int damper_heater_handler(const struct http_request_ctx *req,
                                 struct http_response_ctx *rsp)
{
  if (parse_body(req, damper_body, sizeof(damper_body)) < 0) {
    return send_error(rsp, damper_buf, 400, "invalid body");
  }

  char name[32] = "";
  json_get_string(damper_body, "name", name, sizeof(name));

  struct damper_command cmd = {.type = DAMPER_CMD_SET_HEATER};
  strncpy(cmd.heater_name, name, sizeof(cmd.heater_name) - 1);
  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  return send_ok(rsp, damper_buf);
}

static int servo_handler(enum http_method method,
                         const struct http_request_ctx *req,
                         struct http_response_ctx *rsp)
{
  struct servo_config *cfg = servo_config_get();

  if (method == HTTP_GET) {
    int len = snprintf(servo_buf, sizeof(servo_buf),
        "{\"min_us\":%u,\"max_us\":%u,\"max_deg\":%.1f}",
        cfg->min_us, cfg->max_us, cfg->max_deg);
    return send_json(rsp, servo_buf, len);
  }

  if (parse_body(req, servo_body, sizeof(servo_body)) < 0) {
    return send_error(rsp, servo_buf, 400, "invalid body");
  }

  int ival;
  double dval;

  if (json_get_int(servo_body, "min_us", &ival)) {
    cfg->min_us = (uint32_t)ival;
  }
  if (json_get_int(servo_body, "max_us", &ival)) {
    cfg->max_us = (uint32_t)ival;
  }
  if (json_get_double(servo_body, "max_deg", &dval)) {
    cfg->max_deg = dval;
  }

  servo_config_save();
  return send_ok(rsp, servo_buf);
}

static int handle_api_damper(struct http_client_ctx *client,
                             enum http_transaction_status status,
                             const struct http_request_ctx *req,
                             struct http_response_ctx *rsp,
                             void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  const char *url = (const char *)client->url_buffer;

  if (strcmp(url, "/api/damper") == 0) {
    if (client->method == HTTP_GET) {
      return damper_state_get(rsp);
    }
    return damper_state_patch(req, rsp);
  }

  if (strcmp(url, "/api/damper/config") == 0) {
    return damper_config_handler(req, rsp);
  }

  if (strcmp(url, "/api/damper/heater") == 0) {
    return damper_heater_handler(req, rsp);
  }

  if (strcmp(url, "/api/damper/servo") == 0) {
    return servo_handler(client->method, req, rsp);
  }

  return send_error(rsp, damper_buf, 404, "not found");
}

static struct http_resource_detail_dynamic api_damper_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods =
            BIT(HTTP_GET) | BIT(HTTP_PATCH) | BIT(HTTP_PUT) | BIT(HTTP_DELETE),
        .content_type = "application/json",
    },
    .cb = handle_api_damper,
};

HTTP_RESOURCE_DEFINE(api_damper_res, damper_http_service,
                     "/api/damper*", &api_damper_detail);

//////////////////////////////////////////////////////////////
// /api/heaters* — Unified Heaters Handler
//////////////////////////////////////////////////////////////

static char heaters_buf[JSON_BUF_SIZE];
static char heater_buf[JSON_BUF_SIZE];
static char heater_body[BODY_BUF_SIZE];

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

static int heaters_scan(const struct http_request_ctx *req,
                        struct http_response_ctx *rsp)
{
  int timeout = 5;

  if (req->data_len > 0 && req->data_len < BODY_BUF_SIZE) {
    char body[BODY_BUF_SIZE];
    memcpy(body, req->data, req->data_len);
    body[req->data_len] = '\0';
    json_get_int(body, "timeout", &timeout);
    if (timeout < 1 || timeout > 30) timeout = 5;
  }

  struct heater_command cmd = {.type = HEATER_CMD_SCAN, .scan_timeout = timeout};
  zbus_chan_pub(&heater_command_chan, &cmd, PUB_TIMEOUT);

  int len = snprintf(heaters_buf, sizeof(heaters_buf),
                     "{\"ok\":true,\"timeout\":%d}", timeout);
  return send_json(rsp, heaters_buf, len);
}

static int heaters_list(struct http_response_ctx *rsp)
{
  struct heater_devices devs;
  zbus_chan_read(&heater_devices_chan, &devs, K_NO_WAIT);

  int off = snprintf(heaters_buf, sizeof(heaters_buf),
                     "{\"heaters\":[");

  for (int i = 0; i < devs.count && off < (int)sizeof(heaters_buf) - 100; i++) {
    off += snprintf(heaters_buf + off, sizeof(heaters_buf) - off,
        "%s{\"name\":\"%s\",\"rssi\":%d,"
        "\"protocol\":\"%s\",\"connected\":%s}",
        i > 0 ? "," : "",
        devs.devices[i].name, devs.devices[i].rssi,
        devs.devices[i].protocol,
        i == devs.connected_index ? "true" : "false");
  }

  off += snprintf(heaters_buf + off, sizeof(heaters_buf) - off, "]}");
  return send_json(rsp, heaters_buf, off);
}

static int heater_item(const char *url, enum http_method method,
                       const struct http_request_ctx *req,
                       struct http_response_ctx *rsp)
{
  char name[32];
  if (!extract_path_name(url, "/api/heaters/", name, sizeof(name))) {
    return send_error(rsp, heater_buf, 400, "missing heater name");
  }

  struct heater_devices devs;
  zbus_chan_read(&heater_devices_chan, &devs, K_NO_WAIT);

  int idx = find_heater_by_name(name);
  bool is_connected = (idx >= 0 && idx == devs.connected_index);

  if (method == HTTP_GET) {
    if (idx < 0) {
      return send_error(rsp, heater_buf, 404, "heater not found");
    }

    if (is_connected) {
      struct heater_data hdata = {0};
      zbus_chan_read(&heater_data_chan, &hdata, K_NO_WAIT);

      int len = snprintf(heater_buf, sizeof(heater_buf),
          "{\"name\":\"%s\",\"protocol\":\"%s\",\"connected\":true,"
          "\"telemetry\":{"
          "\"power\":\"%s\",\"step\":\"%s\",\"mode\":\"%s\","
          "\"exhaust_temp\":%.1f,\"ambient_temp\":%.1f,"
          "\"voltage\":%.1f,\"target_temp\":%d,"
          "\"power_level\":%d,\"error\":%d,"
          "\"altitude_mode\":%s,"
          "\"startup_offset\":%d,\"shutdown_offset\":%d}}",
          devs.devices[idx].name, devs.devices[idx].protocol,
          heater_power_state_str(hdata.power),
          heater_run_step_str(hdata.step),
          heater_run_mode_str(hdata.mode),
          hdata.exhaust_temp_c, hdata.ambient_temp_c,
          hdata.voltage, hdata.target_temp,
          hdata.power_level, hdata.error_code,
          hdata.altitude_mode ? "true" : "false",
          hdata.startup_offset, hdata.shutdown_offset);
      return send_json(rsp, heater_buf, len);
    }

    int len = snprintf(heater_buf, sizeof(heater_buf),
        "{\"name\":\"%s\",\"protocol\":\"%s\",\"connected\":false}",
        devs.devices[idx].name, devs.devices[idx].protocol);
    return send_json(rsp, heater_buf, len);
  }

  if (method == HTTP_PUT) {
    if (idx < 0) {
      return send_error(rsp, heater_buf, 404, "heater not found");
    }
    if (devs.connected_index >= 0) {
      return send_error(rsp, heater_buf, 409, "already connected");
    }

    struct heater_command cmd = {.type = HEATER_CMD_CONNECT, .connect_index = idx};
    zbus_chan_pub(&heater_command_chan, &cmd, PUB_TIMEOUT);

    int len = snprintf(heater_buf, sizeof(heater_buf),
                       "{\"ok\":true,\"name\":\"%s\"}", name);
    return send_json(rsp, heater_buf, len);
  }

  if (method == HTTP_PATCH) {
    if (!is_connected) {
      return send_error(rsp, heater_buf, 400, "not connected to this heater");
    }
    if (parse_body(req, heater_body, sizeof(heater_body)) < 0) {
      return send_error(rsp, heater_buf, 400, "invalid body");
    }

    bool power_val;
    int int_val;
    char mode_str[16];
    struct heater_command cmd;

    if (json_get_bool(heater_body, "power", &power_val)) {
      cmd.type = HEATER_CMD_POWER;
      cmd.power_on = power_val;
    } else if (json_get_string(heater_body, "mode", mode_str,
                               sizeof(mode_str))) {
      cmd.type = HEATER_CMD_SET_MODE;
      if (strcmp(mode_str, "manual") == 0) cmd.mode = HEATER_MODE_MANUAL;
      else if (strcmp(mode_str, "automatic") == 0) cmd.mode = HEATER_MODE_AUTOMATIC;
      else if (strcmp(mode_str, "fan") == 0) cmd.mode = HEATER_MODE_FAN;
      else return send_error(rsp, heater_buf, 400, "invalid mode");
    } else if (json_get_int(heater_body, "temp", &int_val)) {
      if (int_val < 8 || int_val > 36) {
        return send_error(rsp, heater_buf, 400, "temp must be 8-36");
      }
      cmd.type = HEATER_CMD_SET_TEMP;
      cmd.temp = int_val;
    } else if (json_get_int(heater_body, "power_level", &int_val)) {
      cmd.type = HEATER_CMD_ADJUST_POWER;
      cmd.power_delta = int_val;
    } else if (json_get_bool(heater_body, "altitude", &power_val)) {
      cmd.type = HEATER_CMD_ALTITUDE;
    } else if (json_get_int(heater_body, "startup_offset", &int_val)) {
      cmd.type = HEATER_CMD_SET_AUTO_OFFSETS;
      cmd.auto_offsets.startup = int_val;
      if (!json_get_int(heater_body, "shutdown_offset", &int_val)) {
        return send_error(rsp, heater_buf, 400, "need shutdown_offset");
      }
      cmd.auto_offsets.shutdown = int_val;
    } else {
      return send_error(rsp, heater_buf, 400,
                        "need power, mode, temp, power_level, altitude, or offsets");
    }

    zbus_chan_pub(&heater_command_chan, &cmd, PUB_TIMEOUT);
    return send_ok(rsp, heater_buf);
  }

  return send_error(rsp, heater_buf, 400, "unsupported method");
}

static int handle_api_heaters(struct http_client_ctx *client,
                              enum http_transaction_status status,
                              const struct http_request_ctx *req,
                              struct http_response_ctx *rsp,
                              void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  const char *url = (const char *)client->url_buffer;

  if (strcmp(url, "/api/heaters") == 0) {
    return heaters_list(rsp);
  }

  if (strcmp(url, "/api/heaters/scan") == 0) {
    return heaters_scan(req, rsp);
  }

  return heater_item(url, client->method, req, rsp);
}

static struct http_resource_detail_dynamic api_heaters_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods =
            BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_PUT) |
            BIT(HTTP_DELETE) | BIT(HTTP_PATCH),
        .content_type = "application/json",
    },
    .cb = handle_api_heaters,
};

HTTP_RESOURCE_DEFINE(api_heaters_res, damper_http_service,
                     "/api/heaters*", &api_heaters_detail);

//////////////////////////////////////////////////////////////
// Server Lifecycle
//////////////////////////////////////////////////////////////

void http_api_start(void)
{
  if (server_running) {
    return;
  }

  int ret = http_server_start();
  if (ret < 0) {
    LOG_ERR("Failed to start HTTP server: %d", ret);
    return;
  }

  server_running = true;
  LOG_INF("HTTP server started on port %d", http_port);
}

void http_api_stop(void)
{
  if (!server_running) {
    return;
  }

  http_server_stop();
  server_running = false;
  LOG_INF("HTTP server stopped");
}
