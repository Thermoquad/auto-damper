// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_WIFI_H
#define AUTO_DAMPER_WIFI_H

#include <stdbool.h>
#include <stddef.h>

int wifi_connect(const char *ssid, const char *password);
int wifi_disconnect(void);
bool wifi_is_connected(void);
int wifi_get_ip_address(char *addr_str, size_t buf_len);

#endif
