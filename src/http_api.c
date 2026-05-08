// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/bluetooth/addr.h>
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
// BLE Externs (from heater_ble.c and shell.c)
//////////////////////////////////////////////////////////////

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
extern int heater_ble_send_set_mode(enum heater_run_mode mode);

struct ble_scan_result {
  bt_addr_le_t addr;
  char name[32];
  int8_t rssi;
  const struct heater_protocol *protocol;
};

extern const struct ble_scan_result *heater_ble_get_scan_result(int index);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define JSON_BUF_SIZE 384
#define BODY_BUF_SIZE 96
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
// JSON Response Helpers
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

static int send_json_error(struct http_response_ctx *rsp, char *buf,
                           int status_code, const char *message)
{
  int len = snprintf(buf, JSON_BUF_SIZE,
                     "{\"error\":\"%s\"}", message);
  rsp->body = (const uint8_t *)buf;
  rsp->body_len = len;
  rsp->final_chunk = true;
  rsp->status = status_code == 400 ? HTTP_400_BAD_REQUEST : HTTP_500_INTERNAL_SERVER_ERROR;
  rsp->header_count = 0;
  return 0;
}

//////////////////////////////////////////////////////////////
// GET /api/status
//////////////////////////////////////////////////////////////

static char status_buf[JSON_BUF_SIZE];

static int handle_api_status(struct http_client_ctx *client,
                             enum http_transaction_status status,
                             const struct http_request_ctx *req,
                             struct http_response_ctx *rsp,
                             void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  struct damper_config *cfg = damper_config_get();
  struct temperature_data temp = {0};
  struct damper_data data = {0};

  zbus_chan_read(&temperature_data_chan, &temp, PUB_TIMEOUT);
  zbus_chan_read(&damper_data_chan, &data, PUB_TIMEOUT);

  int len = snprintf(status_buf, sizeof(status_buf),
      "{"
      "\"temperature\":%.1f,"
      "\"route\":\"%s\","
      "\"mode\":\"%s\","
      "\"servo_degrees\":%.1f,"
      "\"config\":{"
        "\"temp_high\":%.1f,"
        "\"temp_low\":%.1f,"
        "\"servo_inside\":%.1f,"
        "\"servo_outside\":%.1f"
      "}"
      "}",
      temp.celsius,
      data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
      data.mode == DAMPER_MODE_AUTO ? "auto" : "override",
      data.servo_degrees,
      cfg->temp_high, cfg->temp_low,
      cfg->servo_inside_deg, cfg->servo_outside_deg);

  return send_json(rsp, status_buf, len);
}

static struct http_resource_detail_dynamic api_status_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_type = "application/json",
    },
    .cb = handle_api_status,
};

HTTP_RESOURCE_DEFINE(api_status_res, damper_http_service,
                     "/api/status", &api_status_detail);

//////////////////////////////////////////////////////////////
// POST /api/config
//////////////////////////////////////////////////////////////

static char config_body[BODY_BUF_SIZE];
static char config_resp[JSON_BUF_SIZE];

static int handle_api_config(struct http_client_ctx *client,
                             enum http_transaction_status status,
                             const struct http_request_ctx *req,
                             struct http_response_ctx *rsp,
                             void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, config_resp, 400, "invalid body");
  }

  memcpy(config_body, req->data, req->data_len);
  config_body[req->data_len] = '\0';

  char param[32] = {0};
  double value = 0;

  char *p = strstr(config_body, "\"param\"");
  char *v = strstr(config_body, "\"value\"");

  if (!p || !v) {
    return send_json_error(rsp, config_resp, 400,
                           "need param and value");
  }

  char *q1 = strchr(p + 7, '"');
  if (q1) {
    char *q2 = strchr(q1 + 1, '"');
    if (q2 && (q2 - q1 - 1) < (int)sizeof(param)) {
      memcpy(param, q1 + 1, q2 - q1 - 1);
    }
  }

  char *colon = strchr(v + 7, ':');
  if (colon) {
    value = strtod(colon + 1, NULL);
  }

  if (param[0] == '\0') {
    return send_json_error(rsp, config_resp, 400, "invalid param");
  }

  struct damper_command cmd;

  if (strcmp(param, "temp_high") == 0) {
    cmd.type = DAMPER_CMD_SET_TEMP_HIGH;
  } else if (strcmp(param, "temp_low") == 0) {
    cmd.type = DAMPER_CMD_SET_TEMP_LOW;
  } else if (strcmp(param, "servo_inside") == 0) {
    cmd.type = DAMPER_CMD_SET_SERVO_INSIDE;
  } else if (strcmp(param, "servo_outside") == 0) {
    cmd.type = DAMPER_CMD_SET_SERVO_OUTSIDE;
  } else {
    return send_json_error(rsp, config_resp, 400, "unknown param");
  }

  cmd.value = value;
  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  int len = snprintf(config_resp, sizeof(config_resp),
                     "{\"ok\":true,\"param\":\"%s\",\"value\":%.1f}",
                     param, value);
  return send_json(rsp, config_resp, len);
}

static struct http_resource_detail_dynamic api_config_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_config,
};

HTTP_RESOURCE_DEFINE(api_config_res, damper_http_service,
                     "/api/config", &api_config_detail);

//////////////////////////////////////////////////////////////
// POST /api/override
//////////////////////////////////////////////////////////////

static char override_body[BODY_BUF_SIZE];
static char override_resp[JSON_BUF_SIZE];

static int handle_api_override(struct http_client_ctx *client,
                               enum http_transaction_status status,
                               const struct http_request_ctx *req,
                               struct http_response_ctx *rsp,
                               void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, override_resp, 400, "invalid body");
  }

  memcpy(override_body, req->data, req->data_len);
  override_body[req->data_len] = '\0';

  struct damper_command cmd = {.value = 0.0};

  if (strstr(override_body, "\"inside\"")) {
    cmd.type = DAMPER_CMD_OVERRIDE_INSIDE;
  } else if (strstr(override_body, "\"outside\"")) {
    cmd.type = DAMPER_CMD_OVERRIDE_OUTSIDE;
  } else if (strstr(override_body, "\"auto\"")) {
    cmd.type = DAMPER_CMD_SET_AUTO;
  } else {
    return send_json_error(rsp, override_resp, 400,
                           "mode must be inside, outside, or auto");
  }

  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  const char *mode_str =
      cmd.type == DAMPER_CMD_OVERRIDE_INSIDE    ? "inside"
      : cmd.type == DAMPER_CMD_OVERRIDE_OUTSIDE ? "outside"
                                                : "auto";
  int len = snprintf(override_resp, sizeof(override_resp),
                     "{\"ok\":true,\"mode\":\"%s\"}", mode_str);
  return send_json(rsp, override_resp, len);
}

static struct http_resource_detail_dynamic api_override_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_override,
};

HTTP_RESOURCE_DEFINE(api_override_res, damper_http_service,
                     "/api/override", &api_override_detail);

//////////////////////////////////////////////////////////////
// GET /api/ble/status
//////////////////////////////////////////////////////////////

static char ble_status_buf[JSON_BUF_SIZE];

static int handle_api_ble_status(struct http_client_ctx *client,
                                 enum http_transaction_status status,
                                 const struct http_request_ctx *req,
                                 struct http_response_ctx *rsp,
                                 void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  const struct heater_protocol *p = heater_ble_get_protocol();
  bool connected = heater_ble_is_connected();

  int len;

  if (connected) {
    struct heater_data hdata = {0};
    zbus_chan_read(&heater_data_chan, &hdata, K_MSEC(100));

    len = snprintf(ble_status_buf, sizeof(ble_status_buf),
        "{"
        "\"connected\":true,"
        "\"protocol\":\"%s\","
        "\"scanning\":%s,"
        "\"telemetry\":{"
          "\"power\":\"%s\","
          "\"step\":\"%s\","
          "\"mode\":\"%s\","
          "\"exhaust_temp\":%.1f,"
          "\"ambient_temp\":%.1f,"
          "\"voltage\":%.1f,"
          "\"target_temp\":%d,"
          "\"power_level\":%d,"
          "\"error\":%d"
        "}"
        "}",
        p ? p->name : "none",
        heater_ble_is_scanning() ? "true" : "false",
        heater_power_state_str(hdata.power),
        heater_run_step_str(hdata.step),
        heater_run_mode_str(hdata.mode),
        hdata.exhaust_temp_c, hdata.ambient_temp_c,
        hdata.voltage, hdata.target_temp,
        hdata.power_level, hdata.error_code);
  } else {
    len = snprintf(ble_status_buf, sizeof(ble_status_buf),
        "{"
        "\"connected\":false,"
        "\"protocol\":\"%s\","
        "\"scanning\":%s"
        "}",
        p ? p->name : "none",
        heater_ble_is_scanning() ? "true" : "false");
  }

  return send_json(rsp, ble_status_buf, len);
}

static struct http_resource_detail_dynamic api_ble_status_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_status,
};

HTTP_RESOURCE_DEFINE(api_ble_status_res, damper_http_service,
                     "/api/ble/status", &api_ble_status_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/scan
//////////////////////////////////////////////////////////////

static char scan_resp[JSON_BUF_SIZE];

static int handle_api_ble_scan(struct http_client_ctx *client,
                               enum http_transaction_status status,
                               const struct http_request_ctx *req,
                               struct http_response_ctx *rsp,
                               void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  int timeout = 5;

  if (req->data_len > 0 && req->data_len < BODY_BUF_SIZE) {
    char body[BODY_BUF_SIZE];
    memcpy(body, req->data, req->data_len);
    body[req->data_len] = '\0';

    char *t = strstr(body, "\"timeout\"");
    if (t) {
      char *colon = strchr(t + 9, ':');
      if (colon) {
        int val = atoi(colon + 1);
        if (val > 0 && val <= 30) {
          timeout = val;
        }
      }
    }
  }

  int err = heater_ble_scan(timeout);
  if (err == -EALREADY) {
    return send_json_error(rsp, scan_resp, 400, "already scanning");
  } else if (err) {
    return send_json_error(rsp, scan_resp, 500, "scan failed");
  }

  int len = snprintf(scan_resp, sizeof(scan_resp),
                     "{\"ok\":true,\"timeout\":%d}", timeout);
  return send_json(rsp, scan_resp, len);
}

static struct http_resource_detail_dynamic api_ble_scan_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_scan,
};

HTTP_RESOURCE_DEFINE(api_ble_scan_res, damper_http_service,
                     "/api/ble/scan", &api_ble_scan_detail);

//////////////////////////////////////////////////////////////
// GET /api/ble/devices
//////////////////////////////////////////////////////////////

static char devices_buf[JSON_BUF_SIZE];

static int handle_api_ble_devices(struct http_client_ctx *client,
                                  enum http_transaction_status status,
                                  const struct http_request_ctx *req,
                                  struct http_response_ctx *rsp,
                                  void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  heater_ble_scan_stop();

  int count = heater_ble_get_scan_count();
  int off = snprintf(devices_buf, sizeof(devices_buf),
                     "{\"count\":%d,\"devices\":[", count);

  for (int i = 0; i < count && off < (int)sizeof(devices_buf) - 100; i++) {
    const struct ble_scan_result *r = heater_ble_get_scan_result(i);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&r->addr, addr_str, sizeof(addr_str));

    off += snprintf(devices_buf + off, sizeof(devices_buf) - off,
        "%s{\"index\":%d,\"addr\":\"%s\",\"name\":\"%s\","
        "\"rssi\":%d,\"protocol\":\"%s\"}",
        i > 0 ? "," : "", i, addr_str, r->name, r->rssi,
        r->protocol ? r->protocol->name : "unknown");
  }

  off += snprintf(devices_buf + off, sizeof(devices_buf) - off, "]}");
  return send_json(rsp, devices_buf, off);
}

static struct http_resource_detail_dynamic api_ble_devices_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_devices,
};

HTTP_RESOURCE_DEFINE(api_ble_devices_res, damper_http_service,
                     "/api/ble/devices", &api_ble_devices_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/connect
//////////////////////////////////////////////////////////////

static char connect_body[BODY_BUF_SIZE];
static char connect_resp[JSON_BUF_SIZE];

static int handle_api_ble_connect(struct http_client_ctx *client,
                                  enum http_transaction_status status,
                                  const struct http_request_ctx *req,
                                  struct http_response_ctx *rsp,
                                  void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, connect_resp, 400, "invalid body");
  }

  memcpy(connect_body, req->data, req->data_len);
  connect_body[req->data_len] = '\0';

  int index = -1;
  char *idx = strstr(connect_body, "\"index\"");
  if (idx) {
    char *colon = strchr(idx + 7, ':');
    if (colon) {
      index = atoi(colon + 1);
    }
  }

  if (index < 0) {
    return send_json_error(rsp, connect_resp, 400, "need index");
  }

  int err = heater_ble_connect(index);
  if (err == -EALREADY) {
    return send_json_error(rsp, connect_resp, 400, "already connected");
  } else if (err == -EINVAL) {
    return send_json_error(rsp, connect_resp, 400, "invalid index");
  } else if (err) {
    return send_json_error(rsp, connect_resp, 500, "connect failed");
  }

  int len = snprintf(connect_resp, sizeof(connect_resp),
                     "{\"ok\":true,\"index\":%d}", index);
  return send_json(rsp, connect_resp, len);
}

static struct http_resource_detail_dynamic api_ble_connect_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_connect,
};

HTTP_RESOURCE_DEFINE(api_ble_connect_res, damper_http_service,
                     "/api/ble/connect", &api_ble_connect_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/disconnect
//////////////////////////////////////////////////////////////

static char disconnect_resp[128];

static int handle_api_ble_disconnect(struct http_client_ctx *client,
                                     enum http_transaction_status status,
                                     const struct http_request_ctx *req,
                                     struct http_response_ctx *rsp,
                                     void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  int err = heater_ble_disconnect();
  if (err == -ENOTCONN) {
    return send_json_error(rsp, disconnect_resp, 400, "not connected");
  } else if (err) {
    return send_json_error(rsp, disconnect_resp, 500, "disconnect failed");
  }

  int len = snprintf(disconnect_resp, sizeof(disconnect_resp),
                     "{\"ok\":true}");
  return send_json(rsp, disconnect_resp, len);
}

static struct http_resource_detail_dynamic api_ble_disconnect_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_disconnect,
};

HTTP_RESOURCE_DEFINE(api_ble_disconnect_res, damper_http_service,
                     "/api/ble/disconnect", &api_ble_disconnect_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/protocol
//////////////////////////////////////////////////////////////

static char proto_body[BODY_BUF_SIZE];
static char proto_resp[128];

static int handle_api_ble_protocol(struct http_client_ctx *client,
                                   enum http_transaction_status status,
                                   const struct http_request_ctx *req,
                                   struct http_response_ctx *rsp,
                                   void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, proto_resp, 400, "invalid body");
  }

  memcpy(proto_body, req->data, req->data_len);
  proto_body[req->data_len] = '\0';

  if (strstr(proto_body, "\"byd\"")) {
    heater_ble_set_protocol(&heater_protocol_byd);
  } else if (strstr(proto_body, "\"cc\"")) {
    heater_ble_set_protocol(&heater_protocol_cc);
  } else if (strstr(proto_body, "\"auto\"")) {
    heater_ble_set_protocol(NULL);
  } else {
    return send_json_error(rsp, proto_resp, 400,
                           "protocol must be byd, cc, or auto");
  }

  const struct heater_protocol *p = heater_ble_get_protocol();
  int len = snprintf(proto_resp, sizeof(proto_resp),
                     "{\"ok\":true,\"protocol\":\"%s\"}",
                     p ? p->name : "auto");
  return send_json(rsp, proto_resp, len);
}

static struct http_resource_detail_dynamic api_ble_protocol_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_protocol,
};

HTTP_RESOURCE_DEFINE(api_ble_protocol_res, damper_http_service,
                     "/api/ble/protocol", &api_ble_protocol_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/power
//////////////////////////////////////////////////////////////

static char power_body[BODY_BUF_SIZE];
static char power_resp[128];

static int handle_api_ble_power(struct http_client_ctx *client,
                                enum http_transaction_status status,
                                const struct http_request_ctx *req,
                                struct http_response_ctx *rsp,
                                void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, power_resp, 400, "invalid body");
  }

  memcpy(power_body, req->data, req->data_len);
  power_body[req->data_len] = '\0';

  bool on;

  if (strstr(power_body, "\"on\"") || strstr(power_body, "true")) {
    on = true;
  } else if (strstr(power_body, "\"off\"") || strstr(power_body, "false")) {
    on = false;
  } else {
    return send_json_error(rsp, power_resp, 400,
                           "need on/off or true/false");
  }

  int err = heater_ble_send_power(on);
  if (err == -ENOTCONN) {
    return send_json_error(rsp, power_resp, 400, "not connected");
  } else if (err == -ENOTSUP) {
    return send_json_error(rsp, power_resp, 400, "not supported");
  } else if (err) {
    return send_json_error(rsp, power_resp, 500, "send failed");
  }

  int len = snprintf(power_resp, sizeof(power_resp),
                     "{\"ok\":true,\"power\":\"%s\"}", on ? "on" : "off");
  return send_json(rsp, power_resp, len);
}

static struct http_resource_detail_dynamic api_ble_power_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_power,
};

HTTP_RESOURCE_DEFINE(api_ble_power_res, damper_http_service,
                     "/api/ble/power", &api_ble_power_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/temp
//////////////////////////////////////////////////////////////

static char temp_body[BODY_BUF_SIZE];
static char temp_resp[128];

static int handle_api_ble_temp(struct http_client_ctx *client,
                               enum http_transaction_status status,
                               const struct http_request_ctx *req,
                               struct http_response_ctx *rsp,
                               void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, temp_resp, 400, "invalid body");
  }

  memcpy(temp_body, req->data, req->data_len);
  temp_body[req->data_len] = '\0';

  char *v = strstr(temp_body, "\"temp\"");
  if (!v) {
    return send_json_error(rsp, temp_resp, 400, "need temp field");
  }

  char *colon = strchr(v + 6, ':');
  if (!colon) {
    return send_json_error(rsp, temp_resp, 400, "invalid format");
  }

  int temp_c = atoi(colon + 1);
  if (temp_c < 8 || temp_c > 36) {
    return send_json_error(rsp, temp_resp, 400,
                           "temp must be 8-36");
  }

  int err = heater_ble_send_set_temp(temp_c);
  if (err == -ENOTCONN) {
    return send_json_error(rsp, temp_resp, 400, "not connected");
  } else if (err == -ENOTSUP) {
    return send_json_error(rsp, temp_resp, 400, "not supported");
  } else if (err) {
    return send_json_error(rsp, temp_resp, 500, "send failed");
  }

  int len = snprintf(temp_resp, sizeof(temp_resp),
                     "{\"ok\":true,\"temp\":%d}", temp_c);
  return send_json(rsp, temp_resp, len);
}

static struct http_resource_detail_dynamic api_ble_temp_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_temp,
};

HTTP_RESOURCE_DEFINE(api_ble_temp_res, damper_http_service,
                     "/api/ble/temp", &api_ble_temp_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/gear
//////////////////////////////////////////////////////////////

static char plevel_body[BODY_BUF_SIZE];
static char plevel_resp[128];

static int handle_api_ble_power_level(struct http_client_ctx *client,
                                      enum http_transaction_status status,
                                      const struct http_request_ctx *req,
                                      struct http_response_ctx *rsp,
                                      void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, plevel_resp, 400, "invalid body");
  }

  memcpy(plevel_body, req->data, req->data_len);
  plevel_body[req->data_len] = '\0';

  char *v = strstr(plevel_body, "\"level\"");
  if (!v) {
    return send_json_error(rsp, plevel_resp, 400, "need level field");
  }

  char *colon = strchr(v + 7, ':');
  if (!colon) {
    return send_json_error(rsp, plevel_resp, 400, "invalid format");
  }

  int level = atoi(colon + 1);
  if (level < 1 || level > 10) {
    return send_json_error(rsp, plevel_resp, 400,
                           "level must be 1-10");
  }

  int err = heater_ble_send_set_temp(level);
  if (err == -ENOTCONN) {
    return send_json_error(rsp, plevel_resp, 400, "not connected");
  } else if (err) {
    return send_json_error(rsp, plevel_resp, 500, "send failed");
  }

  int len = snprintf(plevel_resp, sizeof(plevel_resp),
                     "{\"ok\":true,\"level\":%d}", level);
  return send_json(rsp, plevel_resp, len);
}

static struct http_resource_detail_dynamic api_ble_power_level_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_power_level,
};

HTTP_RESOURCE_DEFINE(api_ble_power_level_res, damper_http_service,
                     "/api/ble/power-level",
                     &api_ble_power_level_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/mode
//////////////////////////////////////////////////////////////

static char mode_body[BODY_BUF_SIZE];
static char mode_resp[128];

static int handle_api_ble_mode(struct http_client_ctx *client,
                               enum http_transaction_status status,
                               const struct http_request_ctx *req,
                               struct http_response_ctx *rsp,
                               void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, mode_resp, 400, "invalid body");
  }

  memcpy(mode_body, req->data, req->data_len);
  mode_body[req->data_len] = '\0';

  enum heater_run_mode mode;
  const char *mode_str;

  if (strstr(mode_body, "\"manual\"")) {
    mode = HEATER_MODE_MANUAL;
    mode_str = "manual";
  } else if (strstr(mode_body, "\"automatic\"")) {
    mode = HEATER_MODE_AUTOMATIC;
    mode_str = "automatic";
  } else if (strstr(mode_body, "\"fan\"")) {
    mode = HEATER_MODE_FAN;
    mode_str = "fan";
  } else {
    return send_json_error(rsp, mode_resp, 400,
                           "mode must be manual, automatic, or fan");
  }

  int err = heater_ble_send_set_mode(mode);
  if (err == -ENOTCONN) {
    return send_json_error(rsp, mode_resp, 400, "not connected");
  } else if (err == -ENOTSUP) {
    return send_json_error(rsp, mode_resp, 400, "not supported");
  } else if (err) {
    return send_json_error(rsp, mode_resp, 500, "send failed");
  }

  int len = snprintf(mode_resp, sizeof(mode_resp),
                     "{\"ok\":true,\"mode\":\"%s\"}", mode_str);
  return send_json(rsp, mode_resp, len);
}

static struct http_resource_detail_dynamic api_ble_mode_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_mode,
};

HTTP_RESOURCE_DEFINE(api_ble_mode_res, damper_http_service,
                     "/api/ble/mode", &api_ble_mode_detail);

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
