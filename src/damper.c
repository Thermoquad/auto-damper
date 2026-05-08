// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/damper.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(damper, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

static struct damper_config config = {
    .temp_high = 80.0,
    .temp_low = 70.0,
    .servo_inside_deg = 0.0,
    .servo_outside_deg = 270.0,
    .servo_min_us = 500,
    .servo_max_us = 2500,
    .servo_max_deg = 270.0,
};

struct damper_config *damper_config_get(void)
{
  return &config;
}

//////////////////////////////////////////////////////////////
// State Names
//////////////////////////////////////////////////////////////

static const char *state_names[] = {
    [DAMPER_STATE_IDLE] = "IDLE",
    [DAMPER_STATE_ROUTING_INSIDE] = "ROUTING_INSIDE",
    [DAMPER_STATE_ROUTING_OUTSIDE] = "ROUTING_OUTSIDE",
};

const char *damper_state_name(enum damper_state state)
{
  if (state > DAMPER_STATE_ROUTING_OUTSIDE) {
    return "UNKNOWN";
  }
  return state_names[state];
}

//////////////////////////////////////////////////////////////
// State Machine Context
//////////////////////////////////////////////////////////////

struct damper_ctx {
  struct smf_ctx ctx;
  double current_temp;
  enum damper_mode mode;
  enum damper_route route;
};

static struct damper_ctx s;
static K_MUTEX_DEFINE(damper_mutex);

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
      .route = s.route,
      .mode = s.mode,
      .servo_degrees = (s.route == DAMPER_ROUTE_INSIDE)
                           ? config.servo_inside_deg
                           : config.servo_outside_deg,
      .timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks()),
  };
  zbus_chan_pub(&damper_data_chan, &data, K_MSEC(100));
}

//////////////////////////////////////////////////////////////
// Helper: Set Servo to Route
//////////////////////////////////////////////////////////////

static void apply_route(enum damper_route route)
{
  s.route = route;

  double deg = (route == DAMPER_ROUTE_INSIDE) ? config.servo_inside_deg
                                              : config.servo_outside_deg;
  int ret = servo_set_degrees(deg);
  if (ret < 0) {
    LOG_ERR("Failed to set servo: %d", ret);
  }

  LOG_INF("Route: %s (%.1f deg)",
          route == DAMPER_ROUTE_INSIDE ? "INSIDE" : "OUTSIDE", deg);
  publish_damper_data();
}

//////////////////////////////////////////////////////////////
// State: IDLE
//////////////////////////////////////////////////////////////

static void idle_entry(void *ctx)
{
  LOG_INF("State: IDLE");
  apply_route(DAMPER_ROUTE_OUTSIDE);
}

static enum smf_state_result idle_run(void *ctx)
{
  struct damper_ctx *d = ctx;

  if (d->mode == DAMPER_MODE_OVERRIDE) {
    return SMF_EVENT_HANDLED;
  }

  if (d->current_temp >= config.temp_high) {
    smf_set_state(SMF_CTX(d), &states[DAMPER_STATE_ROUTING_INSIDE]);
  } else if (d->current_temp > 0.0) {
    smf_set_state(SMF_CTX(d), &states[DAMPER_STATE_ROUTING_OUTSIDE]);
  }

  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State: ROUTING_INSIDE
//////////////////////////////////////////////////////////////

static void routing_inside_entry(void *ctx)
{
  LOG_INF("State: ROUTING_INSIDE");
  apply_route(DAMPER_ROUTE_INSIDE);
}

static enum smf_state_result routing_inside_run(void *ctx)
{
  struct damper_ctx *d = ctx;

  if (d->mode == DAMPER_MODE_OVERRIDE) {
    return SMF_EVENT_HANDLED;
  }

  if (d->current_temp < config.temp_low) {
    smf_set_state(SMF_CTX(d), &states[DAMPER_STATE_ROUTING_OUTSIDE]);
  }

  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State: ROUTING_OUTSIDE
//////////////////////////////////////////////////////////////

static void routing_outside_entry(void *ctx)
{
  LOG_INF("State: ROUTING_OUTSIDE");
  apply_route(DAMPER_ROUTE_OUTSIDE);
}

static enum smf_state_result routing_outside_run(void *ctx)
{
  struct damper_ctx *d = ctx;

  if (d->mode == DAMPER_MODE_OVERRIDE) {
    return SMF_EVENT_HANDLED;
  }

  if (d->current_temp >= config.temp_high) {
    smf_set_state(SMF_CTX(d), &states[DAMPER_STATE_ROUTING_INSIDE]);
  }

  return SMF_EVENT_HANDLED;
}

//////////////////////////////////////////////////////////////
// State Table
//////////////////////////////////////////////////////////////

static const struct smf_state states[] = {
    [DAMPER_STATE_IDLE] =
        SMF_CREATE_STATE(idle_entry, idle_run, NULL, NULL, NULL),
    [DAMPER_STATE_ROUTING_INSIDE] =
        SMF_CREATE_STATE(routing_inside_entry, routing_inside_run, NULL, NULL,
                         NULL),
    [DAMPER_STATE_ROUTING_OUTSIDE] =
        SMF_CREATE_STATE(routing_outside_entry, routing_outside_run, NULL, NULL,
                         NULL),
};

//////////////////////////////////////////////////////////////
// Zbus Listeners
//////////////////////////////////////////////////////////////

static void temperature_callback(const struct zbus_channel *chan)
{
  const struct temperature_data *temp = zbus_chan_const_msg(chan);

  k_mutex_lock(&damper_mutex, MUTEX_WAIT);
  s.current_temp = temp->celsius;
  k_mutex_unlock(&damper_mutex);
}

static void command_callback(const struct zbus_channel *chan)
{
  const struct damper_command *cmd = zbus_chan_const_msg(chan);

  k_mutex_lock(&damper_mutex, MUTEX_WAIT);

  switch (cmd->type) {
  case DAMPER_CMD_SET_AUTO:
    s.mode = DAMPER_MODE_AUTO;
    LOG_INF("Mode: AUTO");
    /* Re-evaluate routing based on current temperature so the state
     * machine exits any override-held state immediately. */
    if (s.current_temp >= config.temp_high) {
      smf_set_state(SMF_CTX(&s), &states[DAMPER_STATE_ROUTING_INSIDE]);
    } else {
      smf_set_state(SMF_CTX(&s), &states[DAMPER_STATE_ROUTING_OUTSIDE]);
    }
    break;
  case DAMPER_CMD_OVERRIDE_INSIDE:
    s.mode = DAMPER_MODE_OVERRIDE;
    apply_route(DAMPER_ROUTE_INSIDE);
    break;
  case DAMPER_CMD_OVERRIDE_OUTSIDE:
    s.mode = DAMPER_MODE_OVERRIDE;
    apply_route(DAMPER_ROUTE_OUTSIDE);
    break;
  case DAMPER_CMD_SET_TEMP_HIGH:
    config.temp_high = cmd->value;
    LOG_INF("temp_high = %.1f", config.temp_high);
    break;
  case DAMPER_CMD_SET_TEMP_LOW:
    config.temp_low = cmd->value;
    LOG_INF("temp_low = %.1f", config.temp_low);
    break;
  case DAMPER_CMD_SET_SERVO_INSIDE:
    config.servo_inside_deg = cmd->value;
    LOG_INF("servo_inside = %.1f deg", config.servo_inside_deg);
    break;
  case DAMPER_CMD_SET_SERVO_OUTSIDE:
    config.servo_outside_deg = cmd->value;
    LOG_INF("servo_outside = %.1f deg", config.servo_outside_deg);
    break;
  }

  k_mutex_unlock(&damper_mutex);
}

ZBUS_LISTENER_DEFINE(damper_temp_listener, temperature_callback);
ZBUS_LISTENER_DEFINE(damper_cmd_listener, command_callback);

ZBUS_CHAN_DEFINE(temperature_data_chan,
                struct temperature_data,
                NULL, NULL,
                ZBUS_OBSERVERS(damper_temp_listener),
                ZBUS_MSG_INIT(.celsius = 0.0, .timestamp_us = 0));

ZBUS_CHAN_DEFINE(damper_command_chan,
                struct damper_command,
                NULL, NULL,
                ZBUS_OBSERVERS(damper_cmd_listener),
                ZBUS_MSG_INIT(.type = DAMPER_CMD_SET_AUTO, .value = 0.0));

ZBUS_CHAN_DEFINE(damper_data_chan,
                struct damper_data,
                NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.route = DAMPER_ROUTE_OUTSIDE,
                              .mode = DAMPER_MODE_AUTO,
                              .servo_degrees = 0.0,
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

  smf_set_initial(SMF_CTX(&s), &states[DAMPER_STATE_IDLE]);
  s.mode = DAMPER_MODE_AUTO;

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
