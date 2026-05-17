// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_DAMPER_H
#define AUTO_DAMPER_DAMPER_H

#include <stdint.h>

//////////////////////////////////////////////////////////////
// Servo Config
//////////////////////////////////////////////////////////////

struct servo_config {
  uint32_t min_us;
  uint32_t max_us;
  double max_deg;
};

struct servo_config *servo_config_get(void);
int servo_config_save(void);
int servo_config_load(void);

//////////////////////////////////////////////////////////////
// Damper Config
//////////////////////////////////////////////////////////////

struct damper_config {
  double inside_angle;
  double outside_angle;
  double core_threshold;
  char heater_name[32];
  double cool_setpoint;
  double cool_hysteresis;
};

struct damper_config *damper_config_get(void);
int damper_config_save(void);
int damper_config_load(void);

int damper_last_config_result(void);

#endif
