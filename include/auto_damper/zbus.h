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
  DAMPER_CMD_SET_POSITION,
  DAMPER_CMD_SET_ANGLE,
};

struct damper_command {
  enum damper_command_type type;
  int position_id;
  double angle;
};

ZBUS_CHAN_DECLARE(damper_command_chan);

//////////////////////////////////////////////////////////////
// Damper Data Channel
//////////////////////////////////////////////////////////////

enum damper_mode {
  DAMPER_MODE_AUTO,
  DAMPER_MODE_MANUAL,
};

struct damper_data {
  enum damper_mode mode;
  double angle;
  int position_id;
  int64_t timestamp_us;
};

ZBUS_CHAN_DECLARE(damper_data_chan);

//////////////////////////////////////////////////////////////
// Heater Command Channel
//////////////////////////////////////////////////////////////

enum heater_cmd_type {
  HEATER_CMD_SCAN,
  HEATER_CMD_CONNECT,
  HEATER_CMD_DISCONNECT,
  HEATER_CMD_POWER,
  HEATER_CMD_SET_MODE,
  HEATER_CMD_SET_TEMP,
  HEATER_CMD_ADJUST_POWER,
};

struct heater_command {
  enum heater_cmd_type type;
  union {
    int scan_timeout;
    int connect_index;
    bool power_on;
    enum heater_run_mode mode;
    int temp;
    int power_delta;
  };
};

ZBUS_CHAN_DECLARE(heater_command_chan);

//////////////////////////////////////////////////////////////
// Heater Data Channel
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DECLARE(heater_data_chan);

#endif
