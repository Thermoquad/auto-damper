// SPDX-License-Identifier: Apache-2.0

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/flash_img.h>

#include <bootutil/bootutil_public.h>
#include <bootutil/image.h>
#include <psa/crypto.h>

#include <zephyr/zbus/zbus.h>

#include <auto_damper/config.h>
#include <auto_damper/heater.h>
#include <auto_damper/ota.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

/* NVS slot for OTA settings. Picked 0x0050 to leave headroom around
 * the damper (0x0040) and servo (0x0030) blocks. */
#define NVS_ID_OTA_SETTINGS  0x0050
#define NVS_TYPE_OTA         0x0050
#define OTA_SETTINGS_VERSION 1

struct ota_settings {
  bool auto_revert_enabled;
};

static struct ota_settings ota_settings_cache = {
    .auto_revert_enabled = true,
};
static bool ota_settings_loaded;

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define OTA_HOST "github.com"
#define OTA_PORT "443"
#define OTA_MANIFEST_URL "/Thermoquad/auto-damper/releases/latest/download/manifest.json"
#define OTA_RECV_CHUNK 1024
#define OTA_MAX_REDIRECTS 5

//////////////////////////////////////////////////////////////
// Image version (read from MCUboot header in slot0)
//////////////////////////////////////////////////////////////

#define SLOT0_HEADER_ADDR \
  ((const struct image_header *)(CONFIG_FLASH_BASE_ADDRESS + \
                                 DT_REG_ADDR(DT_NODELABEL(slot0_partition))))
#define SLOT1_HEADER_ADDR \
  ((const struct image_header *)(CONFIG_FLASH_BASE_ADDRESS + \
                                 DT_REG_ADDR(DT_NODELABEL(slot1_partition))))

static void format_version(const struct image_header *hdr,
                           char *out, size_t len)
{
  if (hdr->ih_magic != IMAGE_MAGIC) {
    snprintf(out, len, "%s", "");
    return;
  }
  snprintf(out, len, "%u.%u.%u",
           hdr->ih_ver.iv_major,
           hdr->ih_ver.iv_minor,
           hdr->ih_ver.iv_revision);
}

void ota_get_running_version(char *out, size_t len)
{
  const struct image_header *hdr = SLOT0_HEADER_ADDR;
  if (hdr->ih_magic != IMAGE_MAGIC) {
    snprintf(out, len, "unknown");
    return;
  }
  format_version(hdr, out, len);
}

void ota_get_previous_version(char *out, size_t len)
{
  /* Slot1 holds whatever swap-using-offset moved out of slot0 on the
   * last install. On a virgin device it's blank flash (magic !=
   * IMAGE_MAGIC) and we return an empty string so the UI can hide the
   * Revert affordance. */
  format_version(SLOT1_HEADER_ADDR, out, len);
}

//////////////////////////////////////////////////////////////
// Version comparison
//////////////////////////////////////////////////////////////

static int parse_version(const char *s, uint32_t *out)
{
  unsigned int maj, min, rev;
  if (sscanf(s, "%u.%u.%u", &maj, &min, &rev) != 3) {
    return -EINVAL;
  }
  *out = (maj << 16) | (min << 8) | rev;
  return 0;
}

//////////////////////////////////////////////////////////////
// Minimal JSON value extraction
//////////////////////////////////////////////////////////////

static bool json_get_string(const char *body, const char *key,
                            char *out, size_t out_len)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char *p = strstr(body, search);
  if (!p) return false;
  const char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;
  const char *q1 = strchr(colon, '"');
  if (!q1) return false;
  const char *q2 = strchr(q1 + 1, '"');
  if (!q2 || (size_t)(q2 - q1 - 1) >= out_len) return false;
  memcpy(out, q1 + 1, q2 - q1 - 1);
  out[q2 - q1 - 1] = '\0';
  return true;
}

static bool json_get_uint(const char *body, const char *key, uint32_t *out)
{
  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char *p = strstr(body, search);
  if (!p) return false;
  const char *colon = strchr(p + strlen(search), ':');
  if (!colon) return false;
  *out = (uint32_t)strtoul(colon + 1, NULL, 10);
  return true;
}

//////////////////////////////////////////////////////////////
// Raw HTTP over TLS using sockets directly.
//
// Zephyr's http_client API compacts its recv buffer as the parser
// consumes headers, which makes it impossible to reliably extract the
// Location header from a redirect response from outside the parser.
// The canonical Zephyr way for OTA-style HTTP downloads (per
// samples/net/sockets/big_http_download) is to drive the socket
// directly: send the request as a string, parse the response status
// line + headers byte-by-byte from recv() output, then drain the body.
// That's what this module does.
//////////////////////////////////////////////////////////////

/* GitHub's CDN signed-URL redirects carry ~1KB of JWT + signature
 * query parameters. recv_line() also reuses this for the status
 * line (well under 100 bytes), so making both wide keeps things
 * simple. */
static char redirect_url[2048];
static char line[2048];

/* sendall: write the whole buffer or return error. */
static int sendall(int sock, const void *buf, size_t len)
{
  const uint8_t *p = buf;
  while (len > 0) {
    ssize_t n = zsock_send(sock, p, len, 0);
    if (n < 0) {
      return -errno;
    }
    p += n;
    len -= n;
  }
  return 0;
}

/* Read one CRLF-terminated line into 'line' (NUL-terminated, CRLF
 * stripped). Returns line length, 0 on connection close, or -errno.
 *
 * Lines longer than the buffer are silently truncated - we keep
 * reading until CRLF so the stream stays aligned, but discard the
 * excess. GitHub's content-security-policy header is ~3KB and we
 * don't need to parse it; only Location and the status line matter.
 * Returning E2BIG would abort the whole flow. */
static int recv_line(int sock)
{
  size_t i = 0;
  int prev_was_cr = 0;
  bool truncated = false;
  for (;;) {
    char c;
    ssize_t n = zsock_recv(sock, &c, 1, 0);
    if (n < 0) return -errno;
    if (n == 0) return 0;
    if (c == '\n' && prev_was_cr) {
      if (truncated) {
        line[sizeof(line) - 1] = '\0';
        return (int)(sizeof(line) - 1);
      }
      line[i - 1] = '\0';  /* trim the CR */
      return (int)(i - 1);
    }
    if (i + 1 < sizeof(line)) {
      line[i++] = c;
    } else {
      truncated = true;
    }
    prev_was_cr = (c == '\r');
  }
}

/* Open a TLS socket to host:port. Image authenticity comes from
 * MCUboot's ed25519 signature verification, so we skip cert chain
 * validation to keep the firmware footprint small. */
static int tls_open(const char *host, const char *port)
{
  struct zsock_addrinfo hints = {
      .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct zsock_addrinfo *res = NULL;

  int rc = zsock_getaddrinfo(host, port, &hints, &res);
  if (rc) {
    LOG_ERR("getaddrinfo(%s): %d", host, rc);
    return -EHOSTUNREACH;
  }

  int sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
  if (sock < 0) {
    int e = errno;
    zsock_freeaddrinfo(res);
    return -e;
  }

  int verify = TLS_PEER_VERIFY_NONE;
  zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
  zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, host, strlen(host) + 1);
  struct zsock_timeval to = {.tv_sec = 15};
  zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

  rc = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
  zsock_freeaddrinfo(res);
  if (rc < 0) {
    int e = errno;
    zsock_close(sock);
    return -e;
  }
  return sock;
}

/* Send a minimal HTTP/1.0 GET. HTTP/1.0 keeps things simple - server
 * closes the connection after the body which makes EOF the natural
 * "end of body" signal. SLIT macro avoids manual byte counts on
 * string literals (a 33-vs-34 off-by-one truncated the final \n on
 * the original version and the server hung waiting for the rest of
 * the header block). */
#define SLIT(s) (s), (sizeof(s) - 1)

static int send_get(int sock, const char *host, const char *path)
{
  int rc;
  rc = sendall(sock, SLIT("GET "));                          if (rc) return rc;
  rc = sendall(sock, path, strlen(path));                    if (rc) return rc;
  rc = sendall(sock, SLIT(" HTTP/1.0\r\nHost: "));           if (rc) return rc;
  rc = sendall(sock, host, strlen(host));                    if (rc) return rc;
  rc = sendall(sock, SLIT("\r\nUser-Agent: auto-damper-ota/1\r\n"
                          "Accept: */*\r\nConnection: close\r\n\r\n"));
  return rc;
}

/* Parse "HTTP/x.y NNN ..." status line. Returns status code or <0. */
static int parse_status_line(void)
{
  const char *p = strchr(line, ' ');
  if (!p) return -EBADMSG;
  return atoi(p + 1);
}

/* Lowercase string in place. */
static void str_lower(char *s)
{
  for (; *s; s++) *s = tolower((unsigned char)*s);
}

/* Read response status + headers. On 3xx, captures the Location URL
 * into redirect_url. Returns the HTTP status code or negative errno. */
static int read_headers(int sock)
{
  int status = -1;
  redirect_url[0] = '\0';

  /* Status line. */
  int len = recv_line(sock);
  if (len < 0) return len;
  if (len == 0) return -ECONNRESET;
  status = parse_status_line();
  if (status < 0) return status;

  /* Header lines. Blank line ends headers. */
  for (;;) {
    len = recv_line(sock);
    if (len < 0) return len;
    if (len == 0) {
      /* Either blank line (headers done) or connection closed. The
       * recv_line strips CRLF, so a blank line returns 0 length but
       * we got here from a successful read. Treat as end of headers. */
      break;
    }

    /* Split "Field: value" at colon. */
    char *colon = strchr(line, ':');
    if (!colon) continue;
    *colon = '\0';
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t') value++;

    str_lower(line);
    if (strcmp(line, "location") == 0) {
      strncpy(redirect_url, value, sizeof(redirect_url) - 1);
      redirect_url[sizeof(redirect_url) - 1] = '\0';
    }
  }

  return status;
}

/* Split "https://host/path" into host + path. Path defaults to "/" if
 * the URL has no path component. Returns 0 or -EINVAL. */
static int split_url(const char *url, char *host_out, size_t host_cap,
                     char *path_out, size_t path_cap)
{
  const char *p;
  if (strncmp(url, "https://", 8) == 0) {
    p = url + 8;
  } else if (strncmp(url, "http://", 7) == 0) {
    p = url + 7;
  } else {
    return -EINVAL;
  }
  const char *slash = strchr(p, '/');
  size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
  if (host_len == 0 || host_len >= host_cap) return -EINVAL;
  memcpy(host_out, p, host_len);
  host_out[host_len] = '\0';
  if (slash) {
    if (strlen(slash) >= path_cap) return -E2BIG;
    strcpy(path_out, slash);
  } else {
    if (path_cap < 2) return -E2BIG;
    strcpy(path_out, "/");
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// High-level fetch helpers
//////////////////////////////////////////////////////////////

/* Fetch a URL, following up to OTA_MAX_REDIRECTS hops. The caller
 * provides ONE of:
 *   - body_buf (small): accumulate the body into this buffer
 *   - flash_ctx + sha + progress: stream the body into slot1 image,
 *     updating SHA-256 and emitting download progress
 * Returns 0 on success, negative errno on failure. */
static int fetch(const char *initial_url,
                 uint8_t *body_buf, size_t body_cap, size_t *body_len_out,
                 struct flash_img_context *flash_ctx,
                 psa_hash_operation_t *sha,
                 uint32_t *bytes_received,
                 uint32_t expected_bytes,
                 ota_progress_cb cb,
                 struct ota_progress *progress)
{
  char host[64];
  char path[2048];  /* signed CDN URLs have ~1KB query strings */
  int rc = split_url(initial_url, host, sizeof(host), path, sizeof(path));
  if (rc) return rc;

  for (int hop = 0; hop < OTA_MAX_REDIRECTS; hop++) {
    int sock = tls_open(host, OTA_PORT);
    if (sock < 0) return sock;

    rc = send_get(sock, host, path);
    if (rc) { zsock_close(sock); return rc; }

    int status = read_headers(sock);
    if (status < 0) { zsock_close(sock); return status; }

    if (status >= 300 && status < 400) {
      zsock_close(sock);
      if (redirect_url[0] == '\0') return -ENOENT;
      LOG_INF("redirect %d -> %s", status, redirect_url);
      rc = split_url(redirect_url, host, sizeof(host), path, sizeof(path));
      if (rc) return rc;
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP %d", status);
      zsock_close(sock);
      return -EIO;
    }

    /* Drain the body. */
    if (body_len_out) *body_len_out = 0;
    uint8_t buf[OTA_RECV_CHUNK];
    while (1) {
      ssize_t n = zsock_recv(sock, buf, sizeof(buf), 0);
      if (n < 0) {
        zsock_close(sock);
        return -errno;
      }
      if (n == 0) break;  /* EOF - body complete (HTTP/1.0) */

      if (body_buf) {
        size_t take = MIN((size_t)n, body_cap - *body_len_out);
        memcpy(body_buf + *body_len_out, buf, take);
        *body_len_out += take;
      } else if (flash_ctx) {
        int frc = flash_img_buffered_write(flash_ctx, buf, n, false);
        if (frc < 0) {
          LOG_ERR("flash_img_buffered_write: %d", frc);
          zsock_close(sock);
          return frc;
        }
        psa_hash_update(sha, buf, n);
        *bytes_received += n;
        if (cb && (*bytes_received & 0x3FFF) < (uint32_t)n) {
          /* progress every ~16KB */
          progress->bytes_received = *bytes_received;
          progress->bytes_total = expected_bytes;
          cb(progress);
        }
      }
    }
    if (flash_ctx) {
      flash_img_buffered_write(flash_ctx, NULL, 0, true);
    }

    zsock_close(sock);
    return 0;
  }
  return -ELOOP;
}

//////////////////////////////////////////////////////////////
// Top-level OTA flow
//
// Split into two phases so the user can confirm before the device
// actually installs anything:
//
//   1. ota_check()           - fetch manifest, compare versions, set
//                              cached_manifest if newer version exists
//   2. ota_install_pending() - re-validate cache freshness, then
//                              download, verify, swap, reboot
//
// Between the two phases the device shows UPDATE_AVAILABLE state.
// The user has to explicitly click "Install" to start phase 2.
//////////////////////////////////////////////////////////////

static struct {
  bool valid;
  int64_t fetched_at_ms;  /* k_uptime_get() when last set */
  char available_version[16];
  char fw_url[256];
  char sha_hex[65];
  uint32_t expected_size;
} cached_manifest;

#define MANIFEST_CACHE_TTL_MS (5 * 60 * 1000)  /* 5 minutes */

static void emit(ota_progress_cb cb, struct ota_progress *p, enum ota_state s)
{
  p->state = s;
  if (cb) cb(p);
}

static int fetch_manifest(struct ota_progress *progress, ota_progress_cb cb)
{
  static char manifest[1024];
  size_t mlen = 0;
  char start_url[256];
  snprintf(start_url, sizeof(start_url), "https://%s%s",
           OTA_HOST, OTA_MANIFEST_URL);

  int rc = fetch(start_url, (uint8_t *)manifest, sizeof(manifest) - 1, &mlen,
                 NULL, NULL, NULL, 0, NULL, NULL);
  if (rc < 0) {
    snprintf(progress->error, sizeof(progress->error),
             "manifest fetch failed: %d", rc);
    emit(cb, progress, OTA_STATE_FAILED);
    return rc;
  }
  manifest[mlen] = '\0';
  LOG_INF("manifest (%u bytes)", (unsigned)mlen);

  char available[16];
  if (!json_get_string(manifest, "version", available, sizeof(available))) {
    strncpy(progress->error, "manifest missing version",
            sizeof(progress->error) - 1);
    emit(cb, progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  char fw_url[256];
  if (!json_get_string(manifest, "url", fw_url, sizeof(fw_url))) {
    strncpy(progress->error, "manifest missing url",
            sizeof(progress->error) - 1);
    emit(cb, progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  char sha_hex[65];
  if (!json_get_string(manifest, "sha256", sha_hex, sizeof(sha_hex))) {
    strncpy(progress->error, "manifest missing sha256",
            sizeof(progress->error) - 1);
    emit(cb, progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  uint32_t expected_size = 0;
  json_get_uint(manifest, "size", &expected_size);

  /* Populate cache */
  cached_manifest.valid = true;
  cached_manifest.fetched_at_ms = k_uptime_get();
  strncpy(cached_manifest.available_version, available,
          sizeof(cached_manifest.available_version) - 1);
  cached_manifest.available_version[sizeof(cached_manifest.available_version) - 1] = '\0';
  strncpy(cached_manifest.fw_url, fw_url, sizeof(cached_manifest.fw_url) - 1);
  cached_manifest.fw_url[sizeof(cached_manifest.fw_url) - 1] = '\0';
  strncpy(cached_manifest.sha_hex, sha_hex, sizeof(cached_manifest.sha_hex) - 1);
  cached_manifest.sha_hex[sizeof(cached_manifest.sha_hex) - 1] = '\0';
  cached_manifest.expected_size = expected_size;

  /* Mirror into progress so the callback sees it. */
  strncpy(progress->available_version, available,
          sizeof(progress->available_version) - 1);
  progress->bytes_total = expected_size;
  return 0;
}

int ota_check(ota_progress_cb cb)
{
  struct ota_progress progress = {0};
  ota_get_running_version(progress.running_version,
                          sizeof(progress.running_version));
  emit(cb, &progress, OTA_STATE_CHECKING);

  int rc = fetch_manifest(&progress, cb);
  if (rc < 0) {
    cached_manifest.valid = false;
    return rc;
  }

  uint32_t cur = 0, avail = 0;
  parse_version(progress.running_version, &cur);
  parse_version(progress.available_version, &avail);
  if (avail <= cur) {
    LOG_INF("Up to date: %s (latest %s)",
            progress.running_version, progress.available_version);
    emit(cb, &progress, OTA_STATE_UP_TO_DATE);
    /* Keep cache so the UI can still see the version; it's just not
     * newer. Install would fast-path to UP_TO_DATE if called. */
    return -EAGAIN;
  }
  LOG_INF("Update available: %s -> %s",
          progress.running_version, progress.available_version);
  emit(cb, &progress, OTA_STATE_UPDATE_AVAILABLE);
  return 0;
}

int ota_install_pending(ota_progress_cb cb)
{
  struct ota_progress progress = {0};
  ota_get_running_version(progress.running_version,
                          sizeof(progress.running_version));

  /* Re-validate the cached manifest. If stale or missing, re-fetch. */
  bool need_recheck = !cached_manifest.valid ||
      (k_uptime_get() - cached_manifest.fetched_at_ms) > MANIFEST_CACHE_TTL_MS;

  if (need_recheck) {
    emit(cb, &progress, OTA_STATE_CHECKING);
    int rc = fetch_manifest(&progress, cb);
    if (rc < 0) {
      cached_manifest.valid = false;
      return rc;
    }
    uint32_t cur = 0, avail = 0;
    parse_version(progress.running_version, &cur);
    parse_version(progress.available_version, &avail);
    if (avail <= cur) {
      emit(cb, &progress, OTA_STATE_UP_TO_DATE);
      return -EAGAIN;
    }
  } else {
    /* Use the cached values */
    strncpy(progress.available_version, cached_manifest.available_version,
            sizeof(progress.available_version) - 1);
    progress.bytes_total = cached_manifest.expected_size;
  }

  emit(cb, &progress, OTA_STATE_DOWNLOADING);
  struct flash_img_context flash_ctx;
  int rc = flash_img_init(&flash_ctx);
  if (rc < 0) {
    snprintf(progress.error, sizeof(progress.error),
             "flash_img_init: %d", rc);
    emit(cb, &progress, OTA_STATE_FAILED);
    return rc;
  }

  psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
    strncpy(progress.error, "psa_hash_setup failed",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EIO;
  }

  uint32_t bytes_received = 0;
  rc = fetch(cached_manifest.fw_url, NULL, 0, NULL, &flash_ctx, &sha,
             &bytes_received, cached_manifest.expected_size, cb, &progress);
  if (rc < 0) {
    snprintf(progress.error, sizeof(progress.error),
             "firmware fetch failed: %d", rc);
    emit(cb, &progress, OTA_STATE_FAILED);
    return rc;
  }

  emit(cb, &progress, OTA_STATE_VERIFYING);
  uint8_t digest[32];
  size_t digest_len = 0;
  if (psa_hash_finish(&sha, digest, sizeof(digest), &digest_len) != PSA_SUCCESS
      || digest_len != 32) {
    strncpy(progress.error, "hash finalize failed",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EIO;
  }
  char digest_hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(digest_hex + i * 2, 3, "%02x", digest[i]);
  }
  if (strcmp(digest_hex, cached_manifest.sha_hex) != 0) {
    LOG_ERR("sha256 mismatch: got %s, expected %s",
            digest_hex, cached_manifest.sha_hex);
    strncpy(progress.error, "sha256 mismatch",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  LOG_INF("Image sha256 verified: %s", digest_hex);

  rc = boot_set_pending(0);
  if (rc < 0) {
    snprintf(progress.error, sizeof(progress.error),
             "boot_set_pending: %d", rc);
    emit(cb, &progress, OTA_STATE_FAILED);
    return rc;
  }
  emit(cb, &progress, OTA_STATE_SWAP_PENDING);
  LOG_INF("Update %s ready - rebooting", progress.available_version);
  k_sleep(K_MSEC(1500));
  sys_reboot(SYS_REBOOT_COLD);
  return 0;
}

//////////////////////////////////////////////////////////////
// Manual revert
//////////////////////////////////////////////////////////////

int ota_revert(ota_progress_cb cb)
{
  struct ota_progress progress = {0};
  ota_get_running_version(progress.running_version,
                          sizeof(progress.running_version));
  ota_get_previous_version(progress.available_version,
                           sizeof(progress.available_version));

  /* Slot1 must hold a valid image to revert into. On a virgin device
   * slot1 is blank flash and there's nothing to roll back to. */
  if (progress.available_version[0] == '\0') {
    strncpy(progress.error, "no previous image in slot1",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -ENOENT;
  }

  /* Refuse to revert while a swap is already pending or in test mode.
   * The right action then is "confirm or reboot" - not stacking another
   * swap on top of a swap. */
  int swap_type = boot_swap_type();
  if (swap_type != BOOT_SWAP_TYPE_NONE) {
    snprintf(progress.error, sizeof(progress.error),
             "swap already pending (type %d)", swap_type);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EBUSY;
  }

  int rc = boot_set_pending(0);
  if (rc < 0) {
    snprintf(progress.error, sizeof(progress.error),
             "boot_set_pending: %d", rc);
    emit(cb, &progress, OTA_STATE_FAILED);
    return rc;
  }

  emit(cb, &progress, OTA_STATE_SWAP_PENDING);
  LOG_INF("Reverting %s -> %s, rebooting",
          progress.running_version, progress.available_version);
  k_sleep(K_MSEC(1500));
  sys_reboot(SYS_REBOOT_COLD);
  return 0;
}

//////////////////////////////////////////////////////////////
// Auto-revert NVS setting
//////////////////////////////////////////////////////////////

/* Protects ota_settings_cache against concurrent access. Both the
 * zbus listeners (which reach here through ota_auto_revert_enabled()
 * on the WiFi and BT publisher threads) and the shell / websocket
 * paths that set settings serialize through this mutex, so the lazy
 * load and the read/write of the cache remain atomic across contexts. */
static K_MUTEX_DEFINE(settings_mutex);

static void ensure_settings_loaded_locked(void)
{
  if (ota_settings_loaded) return;
  uint16_t version;
  struct ota_settings loaded;
  int rc = config_load(NVS_ID_OTA_SETTINGS, NVS_TYPE_OTA,
                       &version, &loaded, sizeof(loaded));
  if (rc == 0 && version == OTA_SETTINGS_VERSION) {
    ota_settings_cache = loaded;
  }
  /* On miss or version mismatch we keep the compile-time default
   * (auto_revert_enabled = true). */
  ota_settings_loaded = true;
}

bool ota_auto_revert_enabled(void)
{
  k_mutex_lock(&settings_mutex, K_FOREVER);
  ensure_settings_loaded_locked();
  bool val = ota_settings_cache.auto_revert_enabled;
  k_mutex_unlock(&settings_mutex);
  return val;
}

int ota_set_auto_revert_enabled(bool enabled)
{
  k_mutex_lock(&settings_mutex, K_FOREVER);
  ensure_settings_loaded_locked();
  ota_settings_cache.auto_revert_enabled = enabled;
  int rc = config_save(NVS_ID_OTA_SETTINGS, NVS_TYPE_OTA,
                       OTA_SETTINGS_VERSION, &ota_settings_cache,
                       sizeof(ota_settings_cache));
  k_mutex_unlock(&settings_mutex);
  return rc;
}

//////////////////////////////////////////////////////////////
// Auto-confirm on radio-up
//
// When MCUboot boots into slot0 after a fresh install the swap type
// is REVERT until boot_set_confirmed() is called. Left untouched it
// reverts on the next reboot. That's the safety net for an image
// that comes up but is broken in some catastrophic way.
//
// A running image is judged healthy once either radio stack has been
// alive for a settle window: WiFi association + DHCP bind, or BLE
// GATT connection to a heater. The zbus listeners fire earlier than
// those signals - on WIFI_STATUS_CONN_SUCCESS (before DHCP) and on
// L2CAP connect (before GATT ready) - so we schedule a delayed work
// item and let the settle window elapse before actually flipping the
// confirmed bit. If the connection died in the interim, swap type
// would remain REVERT, but that's fine: the work simply retries on
// the next connect event.
//
// The delayed work runs on the system workqueue, off the publisher's
// thread. That avoids the "calling boot_set_confirmed from BT HCI
// callback context" hazard the initial version had, and gives us a
// natural serialization point for the check-then-write of the swap
// bit (only one instance of the delayed work runs at a time).
//////////////////////////////////////////////////////////////

/* Settle window before we trust "radio is up" enough to auto-confirm.
 * Chosen to comfortably span DHCP bind (typical 1-2 s) and GATT
 * discover + subscribe (typical 0.5-1 s) on this hardware. */
#define AUTOCONFIRM_SETTLE_MS 3000

static void autoconfirm_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(autoconfirm_work, autoconfirm_work_handler);

static void autoconfirm_work_handler(struct k_work *work)
{
  ARG_UNUSED(work);
  if (!ota_auto_revert_enabled()) return;
  int swap = boot_swap_type();
  if (swap != BOOT_SWAP_TYPE_REVERT) return;
  int rc = boot_set_confirmed();
  if (rc == 0) {
    LOG_INF("Auto-confirmed running image on radio-up");
  } else {
    LOG_WRN("Auto-confirm boot_set_confirmed failed: %d", rc);
  }
}

static void wifi_up_listener_cb(const struct zbus_channel *chan)
{
  const struct radio_status *rs = zbus_chan_const_msg(chan);
  if (!rs->wifi_connected) return;
  /* Schedule (not submit) - if the work is already queued, this
   * reschedules to the later deadline, which is exactly what we
   * want: latest radio-up event resets the settle timer. */
  k_work_schedule(&autoconfirm_work, K_MSEC(AUTOCONFIRM_SETTLE_MS));
}
ZBUS_LISTENER_DEFINE(ota_wifi_listener, wifi_up_listener_cb);
ZBUS_CHAN_ADD_OBS(radio_status_chan, ota_wifi_listener, 5);

static void ble_up_listener_cb(const struct zbus_channel *chan)
{
  /* heater_states_chan is broadcast on any per-heater state change.
   * A single connected slot is enough of an indication the BT stack
   * is live end-to-end after the settle window. */
  const struct heater_states *hs = zbus_chan_const_msg(chan);
  for (int i = 0; i < hs->count; i++) {
    if (hs->heaters[i].connected) {
      k_work_schedule(&autoconfirm_work, K_MSEC(AUTOCONFIRM_SETTLE_MS));
      return;
    }
  }
}
ZBUS_LISTENER_DEFINE(ota_ble_listener, ble_up_listener_cb);
ZBUS_CHAN_ADD_OBS(heater_states_chan, ota_ble_listener, 5);
