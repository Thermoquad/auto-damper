// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_OTA_H
#define AUTO_DAMPER_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* OTA states reported via ota_progress_cb. */
enum ota_state {
  OTA_STATE_IDLE,
  OTA_STATE_CHECKING,        /* fetching manifest */
  OTA_STATE_UP_TO_DATE,      /* no newer version */
  OTA_STATE_DOWNLOADING,     /* streaming binary into slot1 */
  OTA_STATE_VERIFYING,       /* sha256 verification */
  OTA_STATE_SWAP_PENDING,    /* boot_set_pending succeeded, about to reboot */
  OTA_STATE_FAILED,
};

struct ota_progress {
  enum ota_state state;
  /* For DOWNLOADING: bytes received / total. For other states the
   * fields hold context-specific data (e.g. version comparison). */
  uint32_t bytes_received;
  uint32_t bytes_total;
  char running_version[16];
  char available_version[16];
  char error[64];            /* set when state == OTA_STATE_FAILED */
};

typedef void (*ota_progress_cb)(const struct ota_progress *p);

/* Check for an OTA update and apply it if one is available.
 * Blocks for the duration of the operation. Should be called from
 * its own thread, not from a callback context.
 *
 * Returns 0 on success (image installed, reboot pending), -EAGAIN if
 * already up-to-date, negative errno on failure. */
int ota_check_and_update(ota_progress_cb cb);

/* Read the MCUboot image version of the currently-running primary
 * slot. version_out must be at least 16 bytes. */
void ota_get_running_version(char *version_out, size_t len);

#endif
