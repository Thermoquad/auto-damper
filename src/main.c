// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/config.h>
#include <auto_damper/damper.h>
#include <auto_damper/positions.h>
#include <auto_damper/targets.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Hardware Devices
//////////////////////////////////////////////////////////////

static const struct device *const thermocouple =
    DEVICE_DT_GET_ONE(maxim_max6675);

static const struct pwm_dt_spec servo =
    PWM_DT_SPEC_GET(DT_NODELABEL(servo));

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

extern void damper_thread(void *, void *, void *);
extern void temperature_thread(void *, void *, void *);

//////////////////////////////////////////////////////////////
// Thread: Temperature Reader
//////////////////////////////////////////////////////////////

#define TEMP_STACK_SIZE 1024
#define TEMP_PRIORITY 5
#define TEMP_READ_INTERVAL_MS 500

K_THREAD_DEFINE(temperature_thread_id, TEMP_STACK_SIZE,
                temperature_thread, NULL, NULL, NULL,
                TEMP_PRIORITY, 0, 0);

void temperature_thread(void *p1, void *p2, void *p3)
{
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  if (!device_is_ready(thermocouple)) {
    LOG_ERR("MAX6675 not ready");
    return;
  }

  LOG_INF("Temperature reader started");

  while (1) {
    struct sensor_value val;
    int ret;

    ret = sensor_sample_fetch_chan(thermocouple, SENSOR_CHAN_AMBIENT_TEMP);
    if (ret < 0) {
      LOG_WRN("Failed to fetch temperature: %d", ret);
      k_sleep(K_MSEC(TEMP_READ_INTERVAL_MS));
      continue;
    }

    ret = sensor_channel_get(thermocouple, SENSOR_CHAN_AMBIENT_TEMP, &val);
    if (ret < 0) {
      LOG_WRN("Failed to read temperature: %d", ret);
      k_sleep(K_MSEC(TEMP_READ_INTERVAL_MS));
      continue;
    }

    struct temperature_data data = {
        .celsius = sensor_value_to_double(&val),
        .timestamp_us = k_ticks_to_us_ceil64(k_uptime_ticks()),
    };

    zbus_chan_pub(&temperature_data_chan, &data, K_MSEC(100));

    k_sleep(K_MSEC(TEMP_READ_INTERVAL_MS));
  }
}

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

  positions_init();
  targets_init();

  return 0;
}
