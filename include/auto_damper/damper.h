// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_DAMPER_H
#define AUTO_DAMPER_DAMPER_H

#include <zephyr/smf.h>

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

struct damper_config {
  double temp_high;
  double temp_low;
  double servo_inside_deg;
  double servo_outside_deg;
  uint32_t servo_min_us;
  uint32_t servo_max_us;
  double servo_max_deg;
};

struct damper_config *damper_config_get(void);

//////////////////////////////////////////////////////////////
// State Machine
//////////////////////////////////////////////////////////////

enum damper_state {
  DAMPER_STATE_IDLE,
  DAMPER_STATE_ROUTING_INSIDE,
  DAMPER_STATE_ROUTING_OUTSIDE,
};

const char *damper_state_name(enum damper_state state);

#endif
