// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>
#include <stdio.h>

#include <auto_damper/wifi.h>
#include <auto_damper/wifi_config.h>

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define WIFI_THREAD_STACK_SIZE 2048
#define WIFI_THREAD_PRIORITY 7
#define WIFI_LOOP_SLEEP_MS 1000
#define WIFI_RETRY_DELAY_MS 1000

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

struct wifi_state {
  bool connected;
  bool connecting;
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char password[WIFI_PASSWORD_MAX_LEN + 1];
  bool has_credentials;
  bool auto_connect;
  uint32_t reconnect_interval;
  uint64_t last_connect_attempt_time;
};

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback net_addr_mgmt_cb;
static struct wifi_state wifi_state;
static K_THREAD_STACK_DEFINE(wifi_thread_stack, WIFI_THREAD_STACK_SIZE);
static struct k_thread wifi_thread_data;

//////////////////////////////////////////////////////////////
// HTTP Server Lifecycle
//////////////////////////////////////////////////////////////

void http_api_start(void) __attribute__((weak));
void http_api_stop(void) __attribute__((weak));

void http_api_start(void) {}
void http_api_stop(void) {}

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static void wifi_thread(void *p1, void *p2, void *p3);
static int wifi_connect_internal(void);
static void set_hostname_from_device_id(void);

//////////////////////////////////////////////////////////////
// Event Handlers
//////////////////////////////////////////////////////////////

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *iface)
{
  ARG_UNUSED(iface);

  switch (mgmt_event) {
  case NET_EVENT_WIFI_CONNECT_RESULT: {
    const struct wifi_status *status =
        (const struct wifi_status *)cb->info;

    wifi_state.connecting = false;

    if (status && status->conn_status == WIFI_STATUS_CONN_SUCCESS) {
      LOG_INF("WiFi connected to '%s'", wifi_state.ssid);
      wifi_state.connected = true;

      char addr[16];
      if (wifi_get_ip_address(addr, sizeof(addr)) == 0) {
        LOG_INF("IP address: %s", addr);
        http_api_start();
      }
    } else {
      LOG_ERR("WiFi connection failed: %s",
              status ? wifi_conn_status_txt(status->conn_status)
                     : "unknown");
      wifi_state.connected = false;

      if (status &&
          status->conn_status == WIFI_STATUS_CONN_WRONG_PASSWORD) {
        LOG_ERR("Wrong password — disabling auto-connect");
        wifi_state.auto_connect = false;
      }
    }
    break;
  }

  case NET_EVENT_WIFI_DISCONNECT_RESULT:
    LOG_INF("WiFi disconnected");
    wifi_state.connected = false;
    wifi_state.connecting = false;
    http_api_stop();
    break;

  default:
    break;
  }
}

static void net_addr_event_handler(struct net_mgmt_event_callback *cb,
                                   uint64_t mgmt_event,
                                   struct net_if *iface)
{
  ARG_UNUSED(cb);
  ARG_UNUSED(iface);

  if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND ||
      mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
    char addr[16];
    if (wifi_get_ip_address(addr, sizeof(addr)) == 0) {
      LOG_INF("IP address: %s", addr);
    }
    http_api_start();
  }
}

//////////////////////////////////////////////////////////////
// WiFi Thread
//////////////////////////////////////////////////////////////

static void wifi_thread(void *p1, void *p2, void *p3)
{
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  LOG_INF("WiFi thread started");

  struct wifi_credentials creds;
  struct wifi_settings settings;

  if (wifi_config_load(&creds) == 0) {
    strncpy(wifi_state.ssid, creds.ssid, WIFI_SSID_MAX_LEN);
    strncpy(wifi_state.password, creds.password, WIFI_PASSWORD_MAX_LEN);
    wifi_state.has_credentials = true;
    LOG_INF("WiFi credentials loaded: SSID='%s'", wifi_state.ssid);
  }

  if (wifi_settings_load(&settings) == 0) {
    wifi_state.auto_connect = settings.auto_connect;
    wifi_state.reconnect_interval = settings.reconnect_interval;
  }

  set_hostname_from_device_id();

  if (wifi_state.auto_connect && wifi_state.has_credentials) {
    LOG_INF("Auto-connecting to WiFi...");
    wifi_connect_internal();
  }

  while (true) {
    if (wifi_state.auto_connect && wifi_state.has_credentials &&
        !wifi_state.connected && !wifi_state.connecting) {
      uint64_t elapsed =
          k_uptime_get() - wifi_state.last_connect_attempt_time;
      if (elapsed >= wifi_state.reconnect_interval * 1000ULL) {
        LOG_INF("Retrying WiFi connection...");
        wifi_connect_internal();
      }
    }
    k_sleep(K_MSEC(WIFI_LOOP_SLEEP_MS));
  }
}

//////////////////////////////////////////////////////////////
// Internal Functions
//////////////////////////////////////////////////////////////

static int wifi_connect_internal(void)
{
  if (!wifi_state.has_credentials || wifi_state.connecting) {
    return -EINVAL;
  }

  struct net_if *iface = net_if_get_wifi_sta();
  if (!iface) {
    LOG_ERR("WiFi STA interface not available");
    return -ENODEV;
  }

  wifi_state.connecting = true;
  wifi_state.last_connect_attempt_time = k_uptime_get();

  struct wifi_connect_req_params params = {
      .ssid = (const uint8_t *)wifi_state.ssid,
      .ssid_length = strlen(wifi_state.ssid),
      .psk = strlen(wifi_state.password) > 0
                 ? (const uint8_t *)wifi_state.password
                 : NULL,
      .psk_length = strlen(wifi_state.password),
      .security = strlen(wifi_state.password) > 0
                      ? WIFI_SECURITY_TYPE_PSK
                      : WIFI_SECURITY_TYPE_NONE,
      .band = WIFI_FREQ_BAND_2_4_GHZ,
      .channel = WIFI_CHANNEL_ANY,
      .timeout = SYS_FOREVER_MS,
  };

  LOG_INF("Connecting to '%s'...", wifi_state.ssid);

  int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
                     sizeof(params));
  if (ret) {
    LOG_ERR("WiFi connect request failed: %d", ret);
    wifi_state.connecting = false;

    uint64_t now = k_uptime_get();
    uint64_t interval_ms = wifi_state.reconnect_interval * 1000ULL;
    wifi_state.last_connect_attempt_time =
        now - interval_ms + WIFI_RETRY_DELAY_MS;
    LOG_INF("Will retry in %u ms", WIFI_RETRY_DELAY_MS);

    return ret;
  }

  return 0;
}

static void set_hostname_from_device_id(void)
{
  uint8_t device_id[16];
  ssize_t id_len = hwinfo_get_device_id(device_id, sizeof(device_id));
  if (id_len < 2) {
    return;
  }

  char hostname[64];
  snprintf(hostname, sizeof(hostname), CONFIG_NET_HOSTNAME "-%02x%02x",
           device_id[id_len - 2], device_id[id_len - 1]);

  net_hostname_set(hostname, strlen(hostname));
  LOG_INF("Hostname: %s", hostname);
}

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int wifi_connect(const char *ssid, const char *password)
{
  if (!ssid) {
    return -EINVAL;
  }

  strncpy(wifi_state.ssid, ssid, WIFI_SSID_MAX_LEN);
  wifi_state.ssid[WIFI_SSID_MAX_LEN] = '\0';

  if (password) {
    strncpy(wifi_state.password, password, WIFI_PASSWORD_MAX_LEN);
    wifi_state.password[WIFI_PASSWORD_MAX_LEN] = '\0';
  } else {
    wifi_state.password[0] = '\0';
  }

  wifi_state.has_credentials = true;
  return wifi_connect_internal();
}

int wifi_disconnect(void)
{
  struct net_if *iface = net_if_get_wifi_sta();
  if (!iface) {
    return -ENODEV;
  }
  return net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
}

bool wifi_is_connected(void)
{
  return wifi_state.connected;
}

int wifi_get_ip_address(char *addr_str, size_t buf_len)
{
  if (!addr_str || buf_len < 16) {
    return -EINVAL;
  }

  struct net_if *iface = net_if_get_wifi_sta();
  if (!iface) {
    return -ENODEV;
  }

  struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
  if (!ipv4) {
    return -ENOENT;
  }

  struct net_if_addr *unicast = &ipv4->unicast[0].ipv4;
  if (!unicast || !unicast->is_used) {
    return -ENOENT;
  }

  net_addr_ntop(AF_INET, &unicast->address.in_addr, addr_str, buf_len);
  return 0;
}

//////////////////////////////////////////////////////////////
// Init
//////////////////////////////////////////////////////////////

static int wifi_init(void)
{
  net_mgmt_init_event_callback(
      &wifi_mgmt_cb, wifi_event_handler,
      NET_EVENT_WIFI_CONNECT_RESULT |
          NET_EVENT_WIFI_DISCONNECT_RESULT);
  net_mgmt_add_event_callback(&wifi_mgmt_cb);

  net_mgmt_init_event_callback(
      &net_addr_mgmt_cb, net_addr_event_handler,
      NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_DHCP_BOUND);
  net_mgmt_add_event_callback(&net_addr_mgmt_cb);

  k_thread_create(&wifi_thread_data, wifi_thread_stack,
                  K_THREAD_STACK_SIZEOF(wifi_thread_stack), wifi_thread,
                  NULL, NULL, NULL, WIFI_THREAD_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&wifi_thread_data, "wifi");

  return 0;
}

SYS_INIT(wifi_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
