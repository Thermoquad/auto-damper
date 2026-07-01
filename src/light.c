/* SPDX-License-Identifier: Apache-2.0 */

// Includes
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <auto_damper/light.h>

LOG_MODULE_REGISTER(light, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

/* Default: 10kHz, 0% duty. 10kHz sits above any audible frequency and
 * well below where the level shifter's edge rates become a problem
 * (BSS138 + 10k pullup rise time ~1us). 0% duty means the shifter's
 * pullup holds the strip's control input HIGH, which is the strip's
 * "default cool white" state, matching the physically-wired starting
 * condition. */
#define DESK_DEFAULT_FREQ_HZ  10000
#define DESK_DEFAULT_DUTY_PCT 0

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

static const struct pwm_dt_spec desk_pwm =
    PWM_DT_SPEC_GET(DT_NODELABEL(desk_light));

static uint32_t desk_freq_hz  = DESK_DEFAULT_FREQ_HZ;
static uint8_t  desk_duty_pct = DESK_DEFAULT_DUTY_PCT;

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static int apply_desk(void);

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int light_desk_set_freq_hz(uint32_t hz)
{
  if (hz < 10 || hz > 1000000U) {
    return -EINVAL;
  }
  desk_freq_hz = hz;
  return apply_desk();
}

int light_desk_set_duty_pct(uint8_t duty_pct)
{
  if (duty_pct > 100) {
    return -EINVAL;
  }
  desk_duty_pct = duty_pct;
  return apply_desk();
}

uint32_t light_desk_freq_hz(void)  { return desk_freq_hz; }
uint8_t  light_desk_duty_pct(void) { return desk_duty_pct; }

//////////////////////////////////////////////////////////////
// Init Functions
//////////////////////////////////////////////////////////////

static int light_init(void)
{
  if (!pwm_is_ready_dt(&desk_pwm)) {
    LOG_ERR("desk_light PWM device not ready");
    return -ENODEV;
  }
  int rc = apply_desk();
  if (rc) {
    LOG_ERR("apply_desk: %d", rc);
  }
  return rc;
}
SYS_INIT(light_init, APPLICATION, 90);

//////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////

static int apply_desk(void)
{
  /* period_ns = 1e9 / hz. Kept in 64-bit to avoid overflow at low
   * frequencies. */
  uint64_t period_ns = 1000000000ULL / desk_freq_hz;
  uint64_t pulse_ns  = period_ns * desk_duty_pct / 100U;
  return pwm_set(desk_pwm.dev, desk_pwm.channel,
                 (uint32_t)period_ns, (uint32_t)pulse_ns, desk_pwm.flags);
}
