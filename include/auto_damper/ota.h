// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_OTA_H
#define AUTO_DAMPER_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* OTA states reported via ota_progress_cb. */
enum ota_state {
  OTA_STATE_IDLE,
  OTA_STATE_CHECKING,          /* fetching manifest */
  OTA_STATE_UP_TO_DATE,        /* no newer version */
  OTA_STATE_UPDATE_AVAILABLE,  /* manifest version > running version; user
                                * confirmation needed before install */
  OTA_STATE_DOWNLOADING,       /* streaming binary into slot1 */
  OTA_STATE_VERIFYING,         /* sha256 verification */
  OTA_STATE_SWAP_PENDING,      /* boot_set_pending succeeded, about to reboot */
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

/* Fetch the release manifest, compare versions, and report the
 * result via the progress callback. Does NOT download or install
 * anything - the caller can pass the result through to a UI for
 * user confirmation before calling ota_install_pending().
 *
 * On UPDATE_AVAILABLE, the parsed manifest is cached for a short
 * window so ota_install_pending() can resume without re-fetching.
 *
 * Returns 0 if an update is available, -EAGAIN if up-to-date,
 * negative errno on failure. */
int ota_check(ota_progress_cb cb);

/* Download + verify + commit the image whose manifest was just
 * fetched by ota_check(). If the cached manifest has expired or
 * ota_check() wasn't called first, this re-runs the check.
 *
 * Returns 0 on success (image installed, reboot pending),
 * negative errno on failure. */
int ota_install_pending(ota_progress_cb cb);

/* Read the MCUboot image version of the currently-running primary
 * slot. version_out must be at least 16 bytes. */
void ota_get_running_version(char *version_out, size_t len);

/* Read the MCUboot image version of the secondary slot (slot1), which
 * holds the previously-running image after a successful swap. Used by
 * the Revert UI/shell to tell the user what they'd roll back to.
 *
 * Writes an empty string to version_out if slot1 is empty or its
 * header is unreadable (no previous image, or first-flash device). */
void ota_get_previous_version(char *version_out, size_t len);

/* Manual revert: swap slot0 and slot1, reboot. Slot1 must hold a
 * valid signed image (the previous version preserved by MCUboot's
 * swap-using-offset). Currently-running image must be confirmed
 * (boot_swap_type() == NONE) - reverting during a pending-test state
 * is undefined and refused.
 *
 * On success this function does not return (sys_reboot). Returns
 * negative errno only on validation failure. */
int ota_revert(ota_progress_cb cb);

/* Auto-revert watchdog enable flag, persisted to NVS. Default true.
 * When true, future device-side health checks will trigger an
 * automatic revert if a freshly-installed image looks broken. When
 * false, only ota_revert() (shell or web button) reverts. */
bool ota_auto_revert_enabled(void);
int ota_set_auto_revert_enabled(bool enabled);

#endif
