// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_TARGETS_H
#define AUTO_DAMPER_TARGETS_H

#include <stdbool.h>

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define TARGET_MAX_SLOTS 10

//////////////////////////////////////////////////////////////
// Target Data
//////////////////////////////////////////////////////////////

struct target {
  bool active;
  double range_low;
  double range_high;
  int position_id;
};

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int targets_init(void);
const struct target *targets_get(int id);
int targets_set(int id, double range_low, double range_high, int position_id);
int targets_delete(int id);
int targets_count(void);
bool targets_position_referenced(int position_id);
const struct target *targets_find_by_temp(double temp_c);

#endif
