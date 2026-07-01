/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AUTO_DAMPER_LIGHT_H
#define AUTO_DAMPER_LIGHT_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal LED-strip control surface. The desk strip's on-board driver
 * IC decodes brightness + color temperature from a single PWM control
 * line. Until the encoding is nailed down empirically, this API just
 * exposes the raw knobs: frequency and duty. Once we know which axis
 * is which, higher-level "brightness" / "color" wrappers will sit on
 * top. */

int light_desk_set_freq_hz(uint32_t hz);
int light_desk_set_duty_pct(uint8_t duty_pct);

uint32_t light_desk_freq_hz(void);
uint8_t light_desk_duty_pct(void);

#endif
