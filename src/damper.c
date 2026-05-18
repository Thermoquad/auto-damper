// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/config.h>
#include <auto_damper/damper.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(damper, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define NVS_ID_SERVO_CONFIG  0x0030
#define NVS_TYPE_SERVO       0x0030
#define NVS_ID_DAMPER_CONFIG 0x0040
#define NVS_TYPE_DAMPER      0x0040
#define DAMPER_CONFIG_VERSION 2

static struct servo_config servo_cfg = {
    .min_us = 500,
    .max_us = 2500,
    .max_deg = 270.0,
};

static struct damper_config damper_cfg = {
    .inside_angle = 0.0,
    .outside_angle = 270.0,
    .core_threshold = 150.0,
    .heater_name = "",
    .cool_setpoint = 25.0,
    .cool_hysteresis = 4.0,
};

struct servo_config *servo_config_get(void)
{
  return &servo_cfg;
}

int servo_config_save(void)
{
  return config_save(NVS_ID_SERVO_CONFIG, NVS_TYPE_SERVO, 1,
                     &servo_cfg, sizeof(servo_cfg));
}

int servo_config_load(void)
{
  uint16_t version;
  return config_load(NVS_ID_SERVO_CONFIG, NVS_TYPE_SERVO,
                     &version, &servo_cfg, sizeof(servo_cfg));
}

struct damper_config *damper_config_get(void)
{
  return &damper_cfg;
}

int damper_config_save(void)
{
  return config_save(NVS_ID_DAMPER_CONFIG, NVS_TYPE_DAMPER,
                     DAMPER_CONFIG_VERSION,
                     &damper_cfg, sizeof(damper_cfg));
}

int damper_config_load(void)
{
  uint16_t version;
  return config_load(NVS_ID_DAMPER_CONFIG, NVS_TYPE_DAMPER,
                     &version, &damper_cfg, sizeof(damper_cfg));
}

//////////////////////////////////////////////////////////////
// State Machine Context
//////////////////////////////////////////////////////////////

enum damper_state {
  DAMPER_STATE_AUTO,
  DAMPER_STATE_MANUAL,
  DAMPER_STATE_HEATING,
  DAMPER_STATE_COOLING,
};

struct damper_ctx {
  struct smf_ctx ctx;
  double current_angle;
  enum damper_mode mode;
  enum damper_route route;
  bool cooling_active;
};

static struct damper_ctx s;
static K_MUTEX_DEFINE(damper_mutex);
static volatile int config_result;

#define LOOP_SLEEP K_MSEC(250)
#define MUTEX_WAIT K_FOREVER

extern int servo_set_degrees(double degrees);
extern int servo_init(void);

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static const struct smf_state states[];

//////////////////////////////////////////////////////////////
// Helper: Publish Damper State
//////////////////////////////////////////////////////////////

static void publish_damper_data(void)
{
  struct damper_data data = {
      .mode = s.mode,
      .route = s.route,
      .angle = s.current_angle,
      .inside_angle = damper_cfg.inside_angle,
      .outside_angle = damper_cfg.outside_angle,
      .core_threshold = damper_cfg.core_threshold,
      .cool_setpoint = damper_cfg.cool_setpoint,
      .cool_hysteresis = damper_cfg.cool_hysteresis,
      .timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks()),
  };
  memcpy(data.heater_name, damper_cfg.heater_name,
         sizeof(data.heater_name));
  zbus_chan_pub(&damper_data_chan, &data, K_MSEC(100));
}

//////////////////////////////////////////////////////////////
// Helper: Move Servo
//////////////////////////////////////////////////////////////

static void move_to_angle(double angle)
{
  if (angle == s.current_angle) {
    return;
  }

  int ret = servo_set_degrees(angle);
  if (ret < 0) {
    LOG_ERR("Failed to set servo: %d", ret);
    return;
  }

  s.current_angle = angle;
  LOG_INF("Servo: %.1f deg", angle);
  publish_damper_data();
}

static void route_to(enum damper_route route)
{
  double angle = (route == DAMPER_ROUTE_INSIDE)
      ? damper_cfg.inside_angle
      : damper_cfg.outside_angle;

  if (s.route != route) {
    s.route = route;
    LOG_INF("Route: %s", route == DAMPER_ROUTE_INSIDE ? "INSIDE" : "OUTSIDE");
  }

  move_to_angle(angle);
}

//////////////////////////////////////////////////////////////
// Helper: Send Heater Commands
//////////////////////////////////////////////////////////////

static void send_heater_mode(enum heater_run_mode mode)
{
  struct heater_command cmd = {.type = HEATER_CMD_SET_MODE, .mode = mode};
  zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(100));
}

static void send_heater_power(bool on)
{
  struct heater_command cmd = {.type = HEATER_CMD_POWER, .power_on = on};
  zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(100));
}

//////////////////////////////////////////////////////////////
// Helper: Read Heater State
//////////////////////////////////////////////////////////////

static bool get_heater_state(struct heater_data *hdata)
{
  zbus_chan_read(&heater_data_chan, hdata, K_NO_WAIT);

  if (!hdata->connected ||
      strcmp(hdata->name, damper_cfg.heater_name) != 0) {
    return false;
  }

  return true;
}

//////////////////////////////////////////////////////////////
// Heating Logic (shared by AUTO and HEATING modes)
//////////////////////////////////////////////////////////////

static void heating_run_logic(struct heater_data *hdata)
{
  bool should_route_inside =
      hdata->core_temp_c > damper_cfg.core_threshold &&
      hdata->ambient_temp_c < (double)hdata->target_temp;

  if (should_route_inside) {
    route_to(DAMPER_ROUTE_INSIDE);
  } else {
    route_to(DAMPER_ROUTE_OUTSIDE);
  }
}

//////////////////////////////////////////////////////////////
// State: AUTO (combines heating + cooling)
//////////////////////////////////////////////////////////////

static void auto_entry(void *ctx)
{
  struct damper_ctx *d = ctx;
  d->mode = DAMPER_MODE_AUTO;
  d->route = DAMPER_ROUTE_OUTSIDE;
  d->cooling_active = false;
  LOG_INF("Mode: AUTO");
  route_to(DAMPER_ROUTE_OUTSIDE);
  publish_damper_data();
}

static enum smf_state_result auto_run(void *ctx)
{
  struct damper_ctx *d = ctx;

  if (damper_cfg.heater_name[0] == '\0') {
    return SMF_EVENT_HANDLED;
  }

  struct heater_data hdata;
  if (!get_heater_state(&hdata)) {
    route_to(DAMPER_ROUTE_OUTSIDE);
    d->cooling_active = false;
    return SMF_EVENT_HANDLED;
  }

  if (d->cooling_active) {
    if (hdata.ambient_temp_c <
        damper_cfg.cool_setpoint - damper_cfg.cool_hysteresis) {
      LOG_INF("Auto: cooling complete, ambient %.1f < %.1f",
              hdata.ambient_temp_c,
              damper_cfg.cool_setpoint - damper_cfg.cool_hysteresis);
      d->cooling_active = false;
      route_to(DAMPER_ROUTE_OUTSIDE);
      send_heater_power(false);
    } else {
      route_to(DAMPER_ROUTE_INSIDE);
    }
  } else {
    if (hdata.mode == HEATER_MODE_FAN &&
        hdata.power != HEATER_POWER_OFF &&
        hdata.ambient_temp_c >=
            damper_cfg.cool_setpoint - damper_cfg.cool_hysteresis) {
      LOG_INF("Auto: cooling resumed, fan already running, ambient %.1f",
              hdata.ambient_temp_c);
      d->cooling_active = true;
      route_to(DAMPER_ROUTE_INSIDE);
    } else if (hdata.power == HEATER_POWER_OFF &&
               hdata.ambient_temp_c >= damper_cfg.cool_setpoint) {
      LOG_INF("Auto: cooling started, ambient %.1f >= %.1f",
              hdata.ambient_temp_c, damper_cfg.cool_setpoint);
      d->cooling_active = true;
      send_heater_mode(HEATER_MODE_FAN);
      route_to(DAMPER_ROUTE_INSIDE);
    } else {
      heating_run_logic(&hdata);
    }
  }

  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State: MANUAL
//////////////////////////////////////////////////////////////

static void manual_entry(void *ctx)
{
  struct damper_ctx *d = ctx;
  d->mode = DAMPER_MODE_MANUAL;
  LOG_INF("Mode: MANUAL");
  publish_damper_data();
}

static enum smf_state_result manual_run(void *ctx)
{
  ARG_UNUSED(ctx);
  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State: HEATING (dedicated heating-only mode)
//////////////////////////////////////////////////////////////

static void heating_entry(void *ctx)
{
  struct damper_ctx *d = ctx;
  d->mode = DAMPER_MODE_HEATING;
  d->route = DAMPER_ROUTE_OUTSIDE;
  LOG_INF("Mode: HEATING");
  route_to(DAMPER_ROUTE_OUTSIDE);
  publish_damper_data();
}

static enum smf_state_result heating_run(void *ctx)
{
  ARG_UNUSED(ctx);

  if (damper_cfg.heater_name[0] == '\0') {
    return SMF_EVENT_HANDLED;
  }

  struct heater_data hdata;
  if (!get_heater_state(&hdata)) {
    route_to(DAMPER_ROUTE_OUTSIDE);
    return SMF_EVENT_HANDLED;
  }

  heating_run_logic(&hdata);
  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State: COOLING (fan mode ventilation)
//////////////////////////////////////////////////////////////

static void cooling_entry(void *ctx)
{
  struct damper_ctx *d = ctx;
  d->mode = DAMPER_MODE_COOLING;
  d->route = DAMPER_ROUTE_OUTSIDE;
  d->cooling_active = false;
  LOG_INF("Mode: COOLING");
  route_to(DAMPER_ROUTE_OUTSIDE);
  publish_damper_data();
}

static enum smf_state_result cooling_run(void *ctx)
{
  struct damper_ctx *d = ctx;

  if (damper_cfg.heater_name[0] == '\0') {
    return SMF_EVENT_HANDLED;
  }

  struct heater_data hdata;
  if (!get_heater_state(&hdata)) {
    route_to(DAMPER_ROUTE_OUTSIDE);
    d->cooling_active = false;
    return SMF_EVENT_HANDLED;
  }

  if (d->cooling_active) {
    if (hdata.ambient_temp_c <
        damper_cfg.cool_setpoint - damper_cfg.cool_hysteresis) {
      LOG_INF("Cooling complete, ambient %.1f < %.1f",
              hdata.ambient_temp_c,
              damper_cfg.cool_setpoint - damper_cfg.cool_hysteresis);
      d->cooling_active = false;
      route_to(DAMPER_ROUTE_OUTSIDE);
      send_heater_power(false);
    }
  } else {
    if (hdata.mode == HEATER_MODE_FAN &&
        hdata.power != HEATER_POWER_OFF &&
        hdata.ambient_temp_c >=
            damper_cfg.cool_setpoint - damper_cfg.cool_hysteresis) {
      LOG_INF("Cooling resumed, fan already running, ambient %.1f",
              hdata.ambient_temp_c);
      d->cooling_active = true;
      route_to(DAMPER_ROUTE_INSIDE);
    } else if (hdata.ambient_temp_c >= damper_cfg.cool_setpoint) {
      LOG_INF("Cooling started, ambient %.1f >= %.1f",
              hdata.ambient_temp_c, damper_cfg.cool_setpoint);
      d->cooling_active = true;
      send_heater_mode(HEATER_MODE_FAN);
      route_to(DAMPER_ROUTE_INSIDE);
    }
  }

  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State Table
//////////////////////////////////////////////////////////////

static const struct smf_state states[] = {
    [DAMPER_STATE_AUTO] =
        SMF_CREATE_STATE(auto_entry, auto_run, NULL, NULL, NULL),
    [DAMPER_STATE_MANUAL] =
        SMF_CREATE_STATE(manual_entry, manual_run, NULL, NULL, NULL),
    [DAMPER_STATE_HEATING] =
        SMF_CREATE_STATE(heating_entry, heating_run, NULL, NULL, NULL),
    [DAMPER_STATE_COOLING] =
        SMF_CREATE_STATE(cooling_entry, cooling_run, NULL, NULL, NULL),
};

static enum damper_state mode_to_state(enum damper_mode mode)
{
  switch (mode) {
  case DAMPER_MODE_AUTO: return DAMPER_STATE_AUTO;
  case DAMPER_MODE_HEATING: return DAMPER_STATE_HEATING;
  case DAMPER_MODE_COOLING: return DAMPER_STATE_COOLING;
  case DAMPER_MODE_MANUAL:
  default: return DAMPER_STATE_MANUAL;
  }
}

//////////////////////////////////////////////////////////////
// Zbus Listener: Command Handler
//////////////////////////////////////////////////////////////

static void command_callback(const struct zbus_channel *chan)
{
  const struct damper_command *cmd = zbus_chan_const_msg(chan);

  k_mutex_lock(&damper_mutex, MUTEX_WAIT);

  config_result = 0;

  switch (cmd->type) {
  case DAMPER_CMD_SET_MODE:
    smf_set_state(SMF_CTX(&s), &states[mode_to_state(cmd->mode)]);
    break;

  case DAMPER_CMD_SET_ANGLE:
    smf_set_state(SMF_CTX(&s), &states[DAMPER_STATE_MANUAL]);
    move_to_angle(cmd->angle);
    break;

  case DAMPER_CMD_SET_CONFIG:
    damper_cfg.inside_angle = cmd->inside_angle;
    damper_cfg.outside_angle = cmd->outside_angle;
    damper_cfg.core_threshold = cmd->core_threshold;
    damper_cfg.cool_setpoint = cmd->cool_setpoint;
    damper_cfg.cool_hysteresis = cmd->cool_hysteresis;
    config_result = damper_config_save();
    if (config_result == 0) {
      LOG_INF("Config: inside=%.1f outside=%.1f threshold=%.1f "
              "cool=%.1f hyst=%.1f",
              damper_cfg.inside_angle, damper_cfg.outside_angle,
              damper_cfg.core_threshold, damper_cfg.cool_setpoint,
              damper_cfg.cool_hysteresis);
    }
    publish_damper_data();
    break;

  case DAMPER_CMD_SET_HEATER:
    memcpy(damper_cfg.heater_name, cmd->heater_name,
           sizeof(damper_cfg.heater_name));
    damper_cfg.heater_name[sizeof(damper_cfg.heater_name) - 1] = '\0';
    config_result = damper_config_save();
    if (config_result == 0) {
      LOG_INF("Heater: %s",
              damper_cfg.heater_name[0] ? damper_cfg.heater_name : "(none)");
    }
    publish_damper_data();
    break;
  }

  k_mutex_unlock(&damper_mutex);
}

int damper_last_config_result(void)
{
  return config_result;
}

ZBUS_LISTENER_DEFINE(damper_cmd_listener, command_callback);

ZBUS_CHAN_DEFINE(damper_command_chan,
                struct damper_command,
                NULL, NULL,
                ZBUS_OBSERVERS(damper_cmd_listener),
                ZBUS_MSG_INIT(.type = DAMPER_CMD_SET_MODE,
                              .mode = DAMPER_MODE_AUTO));

ZBUS_CHAN_DEFINE(damper_data_chan,
                struct damper_data,
                NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.mode = DAMPER_MODE_AUTO,
                              .route = DAMPER_ROUTE_OUTSIDE,
                              .angle = 0.0,
                              .timestamp_us = 0));

//////////////////////////////////////////////////////////////
// Thread Entry Point
//////////////////////////////////////////////////////////////

void damper_thread(void *p1, void *p2, void *p3)
{
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  int ret = servo_init();
  if (ret < 0) {
    LOG_ERR("Servo init failed, damper disabled");
    return;
  }

  if (config_exists(NVS_ID_SERVO_CONFIG) > 0) {
    servo_config_load();
  }

  if (config_exists(NVS_ID_DAMPER_CONFIG) > 0) {
    damper_config_load();
    LOG_INF("Loaded config: inside=%.1f outside=%.1f threshold=%.1f "
            "cool=%.1f hyst=%.1f heater=%s",
            damper_cfg.inside_angle, damper_cfg.outside_angle,
            damper_cfg.core_threshold,
            damper_cfg.cool_setpoint, damper_cfg.cool_hysteresis,
            damper_cfg.heater_name[0] ? damper_cfg.heater_name : "(none)");
  }

  servo_set_degrees(damper_cfg.outside_angle);
  s.current_angle = damper_cfg.outside_angle;
  s.route = DAMPER_ROUTE_OUTSIDE;
  smf_set_initial(SMF_CTX(&s), &states[DAMPER_STATE_AUTO]);

  LOG_INF("Damper controller started");

  int res = 0;
  while (res == 0) {
    k_mutex_lock(&damper_mutex, MUTEX_WAIT);
    res = smf_run_state(SMF_CTX(&s));
    k_mutex_unlock(&damper_mutex);
    k_sleep(LOOP_SLEEP);
  }

  LOG_ERR("State machine terminated: %d", res);
}
