/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AUTO_DAMPER_LIGHT_H
#define AUTO_DAMPER_LIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ironman-family LED strip driver.
 *
 * The strip's on-board driver IC decodes button-press pulse widths on
 * a single wire. The OEM controller emulates its three physical
 * buttons (power/color-cycle, brightness+, brightness-) as distinct
 * LOW-pulse widths on the wire. This module lets us either observe
 * those pulses (sniff mode) or generate them (output mode) on GP15.
 */

/* Output-mode primitives. Any of these implicitly leaves the pin in
 * output mode; call light_desk_sniff_start() afterward to re-enter
 * sniff. */
int light_desk_high(void);          /* release: pin idles HIGH via shifter */
int light_desk_low(void);           /* hold LOW indefinitely */
int light_desk_pulse(uint32_t ms);  /* LOW for ms, then HIGH */

/* Automated geometric sweep of pulse widths (10, 20, 40, ... 2000ms)
 * with 2000ms HIGH between each. Blocks the caller until done. */
int light_desk_sweep(void);

/* Sniff mode. GP15 becomes a high-Z input with an edge callback that
 * records timestamps of every rising and falling edge into a ring
 * buffer. */
int light_desk_sniff_start(void);
int light_desk_sniff_stop(void);

/* Sniff event: microsecond timestamp of a transition, and the new
 * pin level after that transition. */
struct light_sniff_event {
  uint64_t timestamp_us;
  bool level_high;
};

/* Pull recorded events out of the ring buffer, oldest first. Empties
 * what it returns from the buffer. Returns the number of events
 * written. */
size_t light_desk_sniff_drain(struct light_sniff_event *out,
                              size_t max_events);

/* Current pin state: "output/high", "output/low", "sniff/idle-high",
 * "sniff/idle-low". */
const char *light_desk_state_str(void);

#endif
