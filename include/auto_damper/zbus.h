// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_ZBUS_H
#define AUTO_DAMPER_ZBUS_H

#include <zephyr/zbus/zbus.h>
#include <auto_damper/heater.h>

//////////////////////////////////////////////////////////////
// Temperature Channel
//////////////////////////////////////////////////////////////

struct temperature_data {
  double celsius;
  int64_t timestamp_us;
};

ZBUS_CHAN_DECLARE(temperature_data_chan);

//////////////////////////////////////////////////////////////
// Damper Command Channel
//////////////////////////////////////////////////////////////

enum damper_command_type {
  DAMPER_CMD_SET_AUTO,
  DAMPER_CMD_OVERRIDE_INSIDE,
  DAMPER_CMD_OVERRIDE_OUTSIDE,
  DAMPER_CMD_SET_TEMP_HIGH,
  DAMPER_CMD_SET_TEMP_LOW,
  DAMPER_CMD_SET_SERVO_INSIDE,
  DAMPER_CMD_SET_SERVO_OUTSIDE,
};

struct damper_command {
  enum damper_command_type type;
  double value;
};

ZBUS_CHAN_DECLARE(damper_command_chan);

//////////////////////////////////////////////////////////////
// Damper Data Channel
//////////////////////////////////////////////////////////////

enum damper_route {
  DAMPER_ROUTE_INSIDE,
  DAMPER_ROUTE_OUTSIDE,
};

enum damper_mode {
  DAMPER_MODE_AUTO,
  DAMPER_MODE_OVERRIDE,
};

struct damper_data {
  enum damper_route route;
  enum damper_mode mode;
  double servo_degrees;
  int64_t timestamp_us;
};

ZBUS_CHAN_DECLARE(damper_data_chan);

//////////////////////////////////////////////////////////////
// Heater Data Channel
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DECLARE(heater_data_chan);

#endif
