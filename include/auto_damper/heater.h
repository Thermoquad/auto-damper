// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_HEATER_H
#define AUTO_DAMPER_HEATER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//////////////////////////////////////////////////////////////
// Universal Heater Types
//////////////////////////////////////////////////////////////

enum heater_power_state {
  HEATER_POWER_OFF,
  HEATER_POWER_STARTING,
  HEATER_POWER_RUNNING,
  HEATER_POWER_SHUTTING_DOWN,
};

enum heater_run_step {
  HEATER_STEP_IDLE,
  HEATER_STEP_SELF_CHECK,
  HEATER_STEP_PREHEAT,
  HEATER_STEP_HEATING,
  HEATER_STEP_COOLING,
};

struct heater_data {
  enum heater_power_state power;
  enum heater_run_step step;
  double exhaust_temp_c;
  double ambient_temp_c;
  double voltage;
  int error_code;
  int target_temp;
  int gear_level;
  bool connected;
  int64_t timestamp_us;
};

//////////////////////////////////////////////////////////////
// Protocol Driver Interface
//////////////////////////////////////////////////////////////

struct heater_protocol {
  const char *name;

  bool (*match)(const char *device_name);

  uint16_t service_uuid;
  uint16_t write_char_uuid;
  uint16_t notify_char_uuid;

  uint32_t heartbeat_ms;

  int (*decode)(const uint8_t *buf, size_t len, struct heater_data *data);
  int (*encode_ping)(uint8_t *buf, size_t len);
  int (*encode_power)(uint8_t *buf, size_t len, bool on);
  int (*encode_set_temp)(uint8_t *buf, size_t len, int temp_c);
};

//////////////////////////////////////////////////////////////
// Protocol Registry
//////////////////////////////////////////////////////////////

extern const struct heater_protocol heater_protocol_byd;
extern const struct heater_protocol heater_protocol_cc;

const char *heater_power_state_str(enum heater_power_state s);
const char *heater_run_step_str(enum heater_run_step s);

#endif
