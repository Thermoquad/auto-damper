// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/config.h>
#include <auto_damper/damper.h>
#include <auto_damper/positions.h>
#include <auto_damper/targets.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(damper, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define NVS_ID_SERVO_CONFIG 0x0030
#define NVS_TYPE_SERVO      0x0030

static struct servo_config servo_cfg = {
    .min_us = 500,
    .max_us = 2500,
    .max_deg = 270.0,
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

//////////////////////////////////////////////////////////////
// State Machine Context
//////////////////////////////////////////////////////////////

enum damper_state {
  DAMPER_STATE_AUTO,
  DAMPER_STATE_MANUAL,
};

struct damper_ctx {
  struct smf_ctx ctx;
  double current_temp;
  double current_angle;
  int current_position_id;
  enum damper_mode mode;
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
      .angle = s.current_angle,
      .position_id = s.current_position_id,
      .timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks()),
  };
  zbus_chan_pub(&damper_data_chan, &data, K_MSEC(100));
}

//////////////////////////////////////////////////////////////
// Helper: Move Servo
//////////////////////////////////////////////////////////////

static void move_to_angle(double angle, int position_id)
{
  if (angle == s.current_angle && position_id == s.current_position_id) {
    return;
  }

  int ret = servo_set_degrees(angle);
  if (ret < 0) {
    LOG_ERR("Failed to set servo: %d", ret);
    return;
  }

  s.current_angle = angle;
  s.current_position_id = position_id;

  LOG_INF("Servo: %.1f deg (position %d)", angle, position_id);
  publish_damper_data();
}

//////////////////////////////////////////////////////////////
// State: AUTO
//////////////////////////////////////////////////////////////

static void auto_entry(void *ctx)
{
  struct damper_ctx *d = ctx;
  d->mode = DAMPER_MODE_AUTO;
  LOG_INF("Mode: AUTO");
  publish_damper_data();
}

static enum smf_state_result auto_run(void *ctx)
{
  struct damper_ctx *d = ctx;

  const struct target *t = targets_find_by_temp(d->current_temp);
  if (t) {
    const struct position *p = positions_get(t->position_id);
    if (p) {
      move_to_angle(p->angle, t->position_id);
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
// State Table
//////////////////////////////////////////////////////////////

static const struct smf_state states[] = {
    [DAMPER_STATE_AUTO] =
        SMF_CREATE_STATE(auto_entry, auto_run, NULL, NULL, NULL),
    [DAMPER_STATE_MANUAL] =
        SMF_CREATE_STATE(manual_entry, manual_run, NULL, NULL, NULL),
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

  config_result = 0;

  switch (cmd->type) {
  case DAMPER_CMD_SET_AUTO:
    smf_set_state(SMF_CTX(&s), &states[DAMPER_STATE_AUTO]);
    break;

  case DAMPER_CMD_SET_POSITION: {
    const struct position *p = positions_get(cmd->position_id);
    if (p) {
      smf_set_state(SMF_CTX(&s), &states[DAMPER_STATE_MANUAL]);
      move_to_angle(p->angle, cmd->position_id);
    } else {
      LOG_WRN("Unknown position %d", cmd->position_id);
      config_result = -ENOENT;
    }
    break;
  }

  case DAMPER_CMD_SET_ANGLE:
    smf_set_state(SMF_CTX(&s), &states[DAMPER_STATE_MANUAL]);
    move_to_angle(cmd->angle, -1);
    break;

  case DAMPER_CMD_POSITION_SET:
    config_result = positions_set(cmd->position_id, cmd->label, cmd->angle);
    break;

  case DAMPER_CMD_POSITION_DELETE:
    config_result = positions_delete(cmd->position_id);
    break;

  case DAMPER_CMD_TARGET_SET:
    config_result = targets_set(cmd->target_id, cmd->range_low, cmd->range_high, cmd->position_id);
    break;

  case DAMPER_CMD_TARGET_DELETE:
    config_result = targets_delete(cmd->target_id);
    break;
  }

  k_mutex_unlock(&damper_mutex);
}

int damper_last_config_result(void)
{
  return config_result;
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
                ZBUS_MSG_INIT(.type = DAMPER_CMD_SET_AUTO));

ZBUS_CHAN_DEFINE(damper_data_chan,
                struct damper_data,
                NULL, NULL,
                ZBUS_OBSERVERS_EMPTY,
                ZBUS_MSG_INIT(.mode = DAMPER_MODE_AUTO,
                              .angle = 0.0, .position_id = -1,
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

  s.current_angle = -1.0;
  s.current_position_id = -1;
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
