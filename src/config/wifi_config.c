// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <auto_damper/config.h>
#include <auto_damper/wifi_config.h>

LOG_MODULE_REGISTER(wifi_config, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define WIFI_CONFIG_VERSION   1
#define WIFI_SETTINGS_VERSION 1
#define WIFI_DEFAULT_AUTO_CONNECT true
#define WIFI_DEFAULT_RECONNECT_INTERVAL 5

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int wifi_config_save(const char *ssid, const char *password)
{
  if (!ssid || !password) {
    return -EINVAL;
  }

  if (strlen(ssid) == 0 || strlen(ssid) > WIFI_SSID_MAX_LEN) {
    return -EINVAL;
  }

  struct wifi_credentials creds = {0};
  strncpy(creds.ssid, ssid, WIFI_SSID_MAX_LEN);
  strncpy(creds.password, password, WIFI_PASSWORD_MAX_LEN);

  int rc = config_save(NVS_ID_WIFI_CREDENTIALS,
                       CONFIG_TYPE_WIFI_CREDENTIALS,
                       WIFI_CONFIG_VERSION, &creds, sizeof(creds));
  if (rc == 0) {
    LOG_INF("WiFi credentials saved: SSID='%s'", ssid);
  }
  return rc;
}

int wifi_config_load(struct wifi_credentials *creds)
{
  if (!creds) {
    return -EINVAL;
  }

  uint16_t version;
  int rc = config_load(NVS_ID_WIFI_CREDENTIALS,
                       CONFIG_TYPE_WIFI_CREDENTIALS,
                       &version, creds, sizeof(*creds));
  if (rc == 0) {
    creds->ssid[WIFI_SSID_MAX_LEN] = '\0';
    creds->password[WIFI_PASSWORD_MAX_LEN] = '\0';
  }
  return rc;
}

bool wifi_config_exists(void)
{
  return config_exists(NVS_ID_WIFI_CREDENTIALS) == 1;
}

int wifi_config_delete(void)
{
  return config_delete(NVS_ID_WIFI_CREDENTIALS);
}

int wifi_settings_save(bool auto_connect, uint32_t reconnect_interval)
{
  struct wifi_settings settings = {
      .auto_connect = auto_connect,
      .reconnect_interval = reconnect_interval,
  };
  return config_save(NVS_ID_WIFI_SETTINGS, CONFIG_TYPE_WIFI_SETTINGS,
                     WIFI_SETTINGS_VERSION, &settings, sizeof(settings));
}

int wifi_settings_load(struct wifi_settings *settings)
{
  if (!settings) {
    return -EINVAL;
  }

  uint16_t version;
  int rc = config_load(NVS_ID_WIFI_SETTINGS, CONFIG_TYPE_WIFI_SETTINGS,
                       &version, settings, sizeof(*settings));
  if (rc == -ENOENT) {
    settings->auto_connect = WIFI_DEFAULT_AUTO_CONNECT;
    settings->reconnect_interval = WIFI_DEFAULT_RECONNECT_INTERVAL;
    return 0;
  }
  return rc;
}
