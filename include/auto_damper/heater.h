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
  HEATER_STEP_BLOWING,
};

enum heater_run_mode {
  HEATER_MODE_MANUAL = 1,
  HEATER_MODE_AUTOMATIC = 2,
  HEATER_MODE_FAN = 3,
};

struct heater_data {
  enum heater_power_state power;
  enum heater_run_step step;
  enum heater_run_mode mode;
  double exhaust_temp_c;
  double ambient_temp_c;
  double voltage;
  int error_code;
  int target_temp;
  int power_level;
  bool altitude_mode;
  int startup_offset;
  int shutdown_offset;
  bool connected;
  char name[32];
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
  int (*encode_set_mode)(uint8_t *buf, size_t len, enum heater_run_mode mode);
  int (*encode_adjust_power)(uint8_t *buf, size_t len, int delta);
  int (*encode_altitude)(uint8_t *buf, size_t len);
  int (*encode_set_auto_offsets)(uint8_t *buf, size_t len,
                                 int startup, int shutdown);
  int (*encode_query_auto_offsets)(uint8_t *buf, size_t len);
};

//////////////////////////////////////////////////////////////
// Protocol Registry
//////////////////////////////////////////////////////////////

extern const struct heater_protocol heater_protocol_byd;
extern const struct heater_protocol heater_protocol_cc;

const char *heater_power_state_str(enum heater_power_state s);
const char *heater_run_step_str(enum heater_run_step s);
const char *heater_run_mode_str(enum heater_run_mode m);

#endif
