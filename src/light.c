/* SPDX-License-Identifier: Apache-2.0 */

// Includes
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <auto_damper/light.h>

LOG_MODULE_REGISTER(light, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

/* Ring buffer sized to hold every edge from a couple of button-press
 * sequences without wrapping. Each event is ~12 bytes; 512 events is
 * ~6KB, cheap and covers a lot of button mashing. */
#define SNIFF_BUF_SIZE 512

/* Automated geometric sweep - matches the widths the research report
 * suggested for identifying button-press pulse widths. */
static const uint32_t sweep_widths_ms[] = {
    10, 20, 40, 80, 160, 320, 640, 1280, 2000,
};
#define SWEEP_GAP_MS 2000

/* Polling-mode sniff: the edge-interrupt approach dropped events
 * (only 7 of 20 expected edges captured for 10 button taps) AND the
 * gpio_pin_get_raw() readback lies on RP2350 in this Zephyr build
 * (returns the OUT register, not the actual pad state). Polling with
 * a direct SIO register read fixes both.
 *
 * 200us poll period = 5 kHz. Well below the shortest observed pulse
 * (~250 ms) but still low CPU load (a single-word memory read
 * per iteration). */
#define POLL_PERIOD_US 200

/* Direct read of the RP2350 SIO GPIO_IN register - authoritative
 * for the real pad state, independent of Zephyr's GPIO driver
 * quirks. GP0-31 map to bits 0-31. */
#define RP2350_SIO_GPIO_IN (*(volatile uint32_t *)0xD0000004U)

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

static const struct gpio_dt_spec bench_gpio =
    GPIO_DT_SPEC_GET(DT_NODELABEL(bench_light), gpios);

enum pin_mode {
  MODE_OUTPUT,
  MODE_SNIFF,
};
static enum pin_mode current_mode = MODE_OUTPUT;

static struct light_sniff_event sniff_buf[SNIFF_BUF_SIZE];
static atomic_t sniff_head; /* monotonic write index */
static atomic_t sniff_tail; /* monotonic read index */

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static int enter_output_mode(void);
static int enter_sniff_mode(void);
static bool read_pad(void);

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int light_bench_high(void)
{
  int rc = enter_output_mode();
  if (rc) return rc;
  /* "Inactive" per the ACTIVE_LOW binding = HIGH on the wire. */
  return gpio_pin_set_dt(&bench_gpio, 0);
}

int light_bench_low(void)
{
  int rc = enter_output_mode();
  if (rc) return rc;
  return gpio_pin_set_dt(&bench_gpio, 1);
}

int light_bench_pulse(uint32_t ms)
{
  int rc = enter_output_mode();
  if (rc) return rc;
  rc = gpio_pin_set_dt(&bench_gpio, 1);
  if (rc) return rc;
  k_msleep(ms);
  return gpio_pin_set_dt(&bench_gpio, 0);
}

int light_bench_sweep(void)
{
  int rc = enter_output_mode();
  if (rc) return rc;
  /* Idle HIGH before the first pulse. */
  rc = gpio_pin_set_dt(&bench_gpio, 0);
  if (rc) return rc;

  for (size_t i = 0; i < ARRAY_SIZE(sweep_widths_ms); i++) {
    k_msleep(SWEEP_GAP_MS);
    LOG_INF("sweep: pulse %u ms", sweep_widths_ms[i]);
    rc = light_bench_pulse(sweep_widths_ms[i]);
    if (rc) return rc;
  }
  k_msleep(SWEEP_GAP_MS);
  LOG_INF("sweep: done");
  return 0;
}

int light_bench_sniff_start(void)
{
  atomic_set(&sniff_head, 0);
  atomic_set(&sniff_tail, 0);
  return enter_sniff_mode();
}

int light_bench_sniff_stop(void)
{
  return enter_output_mode();
}

size_t light_bench_sniff_drain(struct light_sniff_event *out,
                              size_t max_events)
{
  atomic_val_t head = atomic_get(&sniff_head);
  atomic_val_t tail = atomic_get(&sniff_tail);
  size_t available = (size_t)(head - tail);
  size_t to_copy = (available < max_events) ? available : max_events;

  for (size_t i = 0; i < to_copy; i++) {
    out[i] = sniff_buf[(tail + i) % SNIFF_BUF_SIZE];
  }
  atomic_set(&sniff_tail, tail + to_copy);
  return to_copy;
}

const char *light_bench_state_str(void)
{
  /* Read via SIO register - Zephyr's gpio_pin_get_raw() is unreliable
   * on RP2350 in this build (returns cached OUT rather than pad IN),
   * which was the source of the misleading "always LOW" reports we
   * saw during output-mode testing. */
  bool high = read_pad();
  if (current_mode == MODE_OUTPUT) {
    return high ? "output / HIGH (idle)" : "output / LOW (holding press)";
  }
  return high ? "sniff / idle-HIGH" : "sniff / idle-LOW";
}

//////////////////////////////////////////////////////////////
// Init Functions
//////////////////////////////////////////////////////////////

static int light_init(void)
{
  if (!gpio_is_ready_dt(&bench_gpio)) {
    LOG_ERR("bench_light GPIO not ready");
    return -ENODEV;
  }
  /* Default: output mode, driving HIGH (idle). Safe for both wiring
   * setups - won't fight the controller even if it's plugged in. */
  return enter_output_mode();
}
SYS_INIT(light_init, APPLICATION, 90);

//////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////

static bool read_pad(void)
{
  return (RP2350_SIO_GPIO_IN & (1U << bench_gpio.pin)) != 0;
}

static int enter_output_mode(void)
{
  int rc = gpio_pin_configure_dt(&bench_gpio, GPIO_OUTPUT_INACTIVE);
  if (rc) {
    LOG_ERR("configure OUTPUT: %d", rc);
    return rc;
  }
  current_mode = MODE_OUTPUT;
  return 0;
}

static int enter_sniff_mode(void)
{
  int rc = gpio_pin_configure_dt(&bench_gpio, GPIO_INPUT);
  if (rc) {
    LOG_ERR("configure INPUT: %d", rc);
    return rc;
  }
  current_mode = MODE_SNIFF;
  return 0;
}

//////////////////////////////////////////////////////////////
// Sniff polling thread
//
// Polls SIO_GPIO_IN every POLL_PERIOD_US and records every transition
// to the ring buffer. Runs continuously; when the mode isn't SNIFF
// it just waits long enough to check mode again, wasting negligible
// CPU.
//////////////////////////////////////////////////////////////

static void sniff_poll_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
  bool last = read_pad();
  while (1) {
    if (current_mode != MODE_SNIFF) {
      k_sleep(K_MSEC(50));
      last = read_pad();
      continue;
    }
    bool now = read_pad();
    if (now != last) {
      last = now;
      atomic_val_t idx = atomic_inc(&sniff_head);
      sniff_buf[idx % SNIFF_BUF_SIZE] = (struct light_sniff_event){
          .timestamp_us = k_ticks_to_us_floor64(k_uptime_ticks()),
          .level_high = now,
      };
    }
    k_busy_wait(POLL_PERIOD_US);
  }
}

K_THREAD_DEFINE(sniff_tid, 1024, sniff_poll_thread, NULL, NULL, NULL,
                7, 0, 1000);
