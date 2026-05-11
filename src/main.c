// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/config.h>
#include <auto_damper/damper.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Hardware Devices
//////////////////////////////////////////////////////////////

static const struct pwm_dt_spec servo =
    PWM_DT_SPEC_GET(DT_NODELABEL(servo));

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

extern void damper_thread(void *, void *, void *);

//////////////////////////////////////////////////////////////
// Thread: Damper Controller
//////////////////////////////////////////////////////////////

#define DAMPER_STACK_SIZE 2048
#define DAMPER_PRIORITY 4

K_THREAD_DEFINE(damper_thread_id, DAMPER_STACK_SIZE,
                damper_thread, NULL, NULL, NULL,
                DAMPER_PRIORITY, 0, 0);

//////////////////////////////////////////////////////////////
// Servo Helpers
//////////////////////////////////////////////////////////////

static uint32_t degrees_to_pulse_ns(double degrees)
{
  struct servo_config *cfg = servo_config_get();

  double fraction = degrees / cfg->max_deg;
  double pulse_us = cfg->min_us +
                    fraction * (cfg->max_us - cfg->min_us);

  return (uint32_t)(pulse_us * 1000);
}

int servo_set_degrees(double degrees)
{
  uint32_t pulse_ns = degrees_to_pulse_ns(degrees);
  return pwm_set_pulse_dt(&servo, pulse_ns);
}

int servo_init(void)
{
  if (!pwm_is_ready_dt(&servo)) {
    LOG_ERR("Servo PWM not ready");
    return -ENODEV;
  }
  LOG_INF("Servo initialized");
  return 0;
}

int main(void)
{
  int rc = config_init();
  if (rc) {
    LOG_ERR("Config init failed: %d", rc);
  }

  struct heater_command cmd = {.type = HEATER_CMD_SCAN, .scan_timeout = 5};
  zbus_chan_pub(&heater_command_chan, &cmd, K_MSEC(500));

  return 0;
}
