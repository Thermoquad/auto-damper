// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_ZBUS_H
#define AUTO_DAMPER_ZBUS_H

#include <zephyr/zbus/zbus.h>
#include <auto_damper/heater.h>

//////////////////////////////////////////////////////////////
// Damper Types
//////////////////////////////////////////////////////////////

enum damper_mode {
  DAMPER_MODE_AUTO,
  DAMPER_MODE_MANUAL,
  DAMPER_MODE_HEATING,
  DAMPER_MODE_COOLING,
};

enum damper_route {
  DAMPER_ROUTE_OUTSIDE,
  DAMPER_ROUTE_INSIDE,
};

//////////////////////////////////////////////////////////////
// Damper Command Channel
//////////////////////////////////////////////////////////////

enum damper_command_type {
  DAMPER_CMD_SET_MODE,
  DAMPER_CMD_SET_ANGLE,
  DAMPER_CMD_SET_CONFIG,
  DAMPER_CMD_SET_HEATER,
};

struct damper_command {
  enum damper_command_type type;
  enum damper_mode mode;
  double angle;
  double inside_angle;
  double outside_angle;
  double core_threshold;
  double cool_setpoint;
  double cool_hysteresis;
  char heater_name[32];
};

ZBUS_CHAN_DECLARE(damper_command_chan);

//////////////////////////////////////////////////////////////
// Damper Data Channel
//////////////////////////////////////////////////////////////

struct damper_data {
  enum damper_mode mode;
  enum damper_route route;
  double angle;
  double inside_angle;
  double outside_angle;
  double core_threshold;
  double cool_setpoint;
  double cool_hysteresis;
  char heater_name[32];
  int64_t timestamp_us;
};

ZBUS_CHAN_DECLARE(damper_data_chan);

//////////////////////////////////////////////////////////////
// Heater Command Channel
//////////////////////////////////////////////////////////////

enum heater_cmd_type {
  HEATER_CMD_SCAN,
  HEATER_CMD_SCAN_STOP,
  HEATER_CMD_CONNECT,
  HEATER_CMD_POWER,
  HEATER_CMD_SET_MODE,
  HEATER_CMD_SET_TEMP,
  HEATER_CMD_ADJUST_POWER,
  HEATER_CMD_ALTITUDE,
  HEATER_CMD_SET_AUTO_OFFSETS,
  HEATER_CMD_QUERY_AUTO_OFFSETS,
  HEATER_CMD_RAW,
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
    struct {
      int startup;
      int shutdown;
    } auto_offsets;
    struct {
      uint8_t data[16];
      uint8_t len;
    } raw;
  };
};

ZBUS_CHAN_DECLARE(heater_command_chan);

//////////////////////////////////////////////////////////////
// Heater Devices Channel
//////////////////////////////////////////////////////////////

#define HEATER_DEVICES_MAX 8

struct heater_device_info {
  char name[32];
  int8_t rssi;
  char protocol[12];
};

struct heater_devices {
  struct heater_device_info devices[HEATER_DEVICES_MAX];
  int count;
  int connected_index;
};

ZBUS_CHAN_DECLARE(heater_devices_chan);

//////////////////////////////////////////////////////////////
// Heater Data Channel
//////////////////////////////////////////////////////////////

ZBUS_CHAN_DECLARE(heater_data_chan);

#endif
