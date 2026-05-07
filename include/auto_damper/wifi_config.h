// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_WIFI_CONFIG_H
#define AUTO_DAMPER_WIFI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASSWORD_MAX_LEN 64

struct wifi_credentials {
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char password[WIFI_PASSWORD_MAX_LEN + 1];
};

struct wifi_settings {
  bool auto_connect;
  uint32_t reconnect_interval;
};

int wifi_config_save(const char *ssid, const char *password);
int wifi_config_load(struct wifi_credentials *creds);
bool wifi_config_exists(void);
int wifi_config_delete(void);

int wifi_settings_save(bool auto_connect, uint32_t reconnect_interval);
int wifi_settings_load(struct wifi_settings *settings);

#endif
