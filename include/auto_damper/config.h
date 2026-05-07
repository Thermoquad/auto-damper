// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_CONFIG_H
#define AUTO_DAMPER_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define CONFIG_TYPE_WIFI_CREDENTIALS 0x0001
#define CONFIG_TYPE_WIFI_SETTINGS    0x0002

#define NVS_ID_WIFI_CREDENTIALS 0x0001
#define NVS_ID_WIFI_SETTINGS    0x0002

int config_init(void);
int config_save(uint16_t id, uint16_t type, uint16_t version,
                const void *data, size_t len);
int config_load(uint16_t id, uint16_t expected_type, uint16_t *version,
                void *data, size_t len);
int config_exists(uint16_t id);
int config_delete(uint16_t id);

#endif
