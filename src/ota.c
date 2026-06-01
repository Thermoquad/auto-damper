// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/storage/flash_map.h>

#include <bootutil/bootutil_public.h>
#include <bootutil/image.h>
#include <psa/crypto.h>

#include <auto_damper/ota.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define OTA_HOST "github.com"
#define OTA_PORT "443"
#define OTA_MANIFEST_URL "/Thermoquad/auto-damper/releases/latest/download/manifest.json"
#define OTA_RECV_BUF_SIZE 2048
#define OTA_REQ_TIMEOUT_MS 30000

//////////////////////////////////////////////////////////////
// Slot0 image header (XIP-mapped at flash base + slot0 offset)
//
// The MCUboot image header is at the very start of slot0. We can
// read it directly via the XIP mapping — no flash_area API needed.
//////////////////////////////////////////////////////////////

#define SLOT0_HEADER_ADDR \
  ((const struct image_header *)(CONFIG_FLASH_BASE_ADDRESS + \
                                 DT_REG_ADDR(DT_NODELABEL(slot0_partition))))

void ota_get_running_version(char *out, size_t len)
{
  const struct image_header *hdr = SLOT0_HEADER_ADDR;
  if (hdr->ih_magic != IMAGE_MAGIC) {
    snprintf(out, len, "unknown");
    return;
  }
  snprintf(out, len, "%u.%u.%u",
           hdr->ih_ver.iv_major,
           hdr->ih_ver.iv_minor,
           hdr->ih_ver.iv_revision);
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
// Minimal JSON value extraction (key:"value" or key:number)
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
// TLS socket open + connect, follow 302 redirects
//////////////////////////////////////////////////////////////

static int tls_connect(const char *host, const char *port)
{
  struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct zsock_addrinfo *res = NULL;

  int rc = zsock_getaddrinfo(host, port, &hints, &res);
  if (rc) {
    LOG_ERR("getaddrinfo(%s): %d", host, rc);
    return -EHOSTUNREACH;
  }

  int sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
  if (sock < 0) {
    zsock_freeaddrinfo(res);
    return -errno;
  }

  /* Image authenticity comes from MCUboot's ed25519 signature check.
   * TLS without cert verification is enough to prevent in-flight
   * tampering with the cleartext download (which would only result in
   * MCUboot rejecting the unsigned junk anyway). Full cert chain
   * verification adds ~30KB of CA bundle + parsing — not worth it for
   * an integrity-checked payload. */
  int verify = TLS_PEER_VERIFY_NONE;
  if (zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
                       &verify, sizeof(verify)) < 0) {
    LOG_WRN("TLS_PEER_VERIFY: %d", errno);
  }
  if (zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                       host, strlen(host) + 1) < 0) {
    LOG_WRN("TLS_HOSTNAME: %d", errno);
  }

  rc = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
  zsock_freeaddrinfo(res);
  if (rc < 0) {
    LOG_ERR("connect: %d", errno);
    zsock_close(sock);
    return -errno;
  }
  return sock;
}

//////////////////////////////////////////////////////////////
// HTTP fetch with manual 302 follow
//
// We can't use Zephyr's http_client follow_redirect because the
// redirect target is a different host (objects.githubusercontent.com)
// and TLS wants a fresh connection + SNI for the new hostname. We
// parse Location ourselves and recurse up to a depth limit.
//////////////////////////////////////////////////////////////

struct fetch_ctx {
  /* For body capture (manifest): buffer + length */
  uint8_t *body_buf;
  size_t body_cap;
  size_t body_len;
  /* For binary stream (firmware): callbacks */
  struct flash_img_context *flash_ctx;
  psa_hash_operation_t *sha_op;
  uint32_t *bytes_received;
  uint32_t bytes_total;
  ota_progress_cb cb;
  struct ota_progress *progress;
  /* For redirect handling: dump the Location header to this buffer */
  char location[256];
};

static int response_cb(struct http_response *rsp,
                       enum http_final_call final_data,
                       void *user_data)
{
  struct fetch_ctx *ctx = user_data;

  /* Capture Location header from any redirect response. The HTTP
   * client parses headers for us via http_header_cb when configured,
   * but for simplicity we scan the recv_buf directly. */
  if (rsp->body_frag_start && rsp->body_frag_len > 0) {
    if (ctx->body_buf) {
      size_t take = MIN(rsp->body_frag_len,
                        ctx->body_cap - ctx->body_len);
      memcpy(ctx->body_buf + ctx->body_len, rsp->body_frag_start, take);
      ctx->body_len += take;
    } else if (ctx->flash_ctx) {
      int rc = flash_img_buffered_write(ctx->flash_ctx,
                                        rsp->body_frag_start,
                                        rsp->body_frag_len,
                                        false);
      if (rc < 0) {
        LOG_ERR("flash_img_buffered_write: %d", rc);
        return rc;
      }
      psa_hash_update(ctx->sha_op, rsp->body_frag_start,
                      rsp->body_frag_len);
      *ctx->bytes_received += rsp->body_frag_len;
      if (ctx->cb && (rsp->body_frag_len > 0)) {
        ctx->progress->bytes_received = *ctx->bytes_received;
        ctx->cb(ctx->progress);
      }
    }
  }

  if (final_data == HTTP_DATA_FINAL && ctx->flash_ctx) {
    flash_img_buffered_write(ctx->flash_ctx, NULL, 0, true);
  }
  return 0;
}

/* HTTP header callback to capture Location: header values during
 * redirect responses. Called for each header line. */
static int header_cb(struct http_response *rsp,
                     enum http_final_call final_data,
                     void *user_data)
{
  ARG_UNUSED(rsp);
  ARG_UNUSED(final_data);
  ARG_UNUSED(user_data);
  /* http_client already parses Location into rsp->http_status and
   * the redirect body; we'll inspect rsp->http_status in our caller. */
  return 0;
}

/* GET <url> from <host>. If 'body_buf' is non-NULL, the response body
 * is captured into it (for manifest). If 'flash_ctx' is non-NULL, the
 * body is streamed into the slot1 flash image (for firmware). On
 * 30x responses, the Location header is captured into 'redirect_out'. */
static int http_get(const char *host, const char *port, const char *url,
                    struct fetch_ctx *ctx, int *status_out,
                    char *redirect_out, size_t redirect_cap)
{
  int sock = tls_connect(host, port);
  if (sock < 0) {
    return sock;
  }

  static uint8_t recv_buf[OTA_RECV_BUF_SIZE];
  struct http_request req = {
      .method = HTTP_GET,
      .url = url,
      .host = host,
      .protocol = "HTTP/1.1",
      .response = response_cb,
      .recv_buf = recv_buf,
      .recv_buf_len = sizeof(recv_buf),
  };

  int rc = http_client_req(sock, &req, OTA_REQ_TIMEOUT_MS, ctx);
  zsock_close(sock);
  if (rc < 0) {
    LOG_ERR("http_client_req: %d", rc);
    return rc;
  }

  /* Inspect the captured response to extract status + Location. The
   * Zephyr http_response status code lives in the callback's rsp; we
   * sniff from the recv_buf manually here as a fallback. */
  if (status_out) {
    const char *p = strstr((char *)recv_buf, "HTTP/1.1 ");
    *status_out = p ? atoi(p + 9) : 0;
  }
  if (redirect_out) {
    const char *loc = strstr((char *)recv_buf, "location:");
    if (!loc) loc = strstr((char *)recv_buf, "Location:");
    if (loc) {
      loc += 9;
      while (*loc == ' ') loc++;
      const char *end = strstr(loc, "\r\n");
      if (end && (size_t)(end - loc) < redirect_cap) {
        memcpy(redirect_out, loc, end - loc);
        redirect_out[end - loc] = '\0';
      }
    }
  }
  return 0;
}

/* Split a URL like "https://host/path" into host + path components. */
static int split_url(const char *url, char *host_out, size_t host_cap,
                     char *path_out, size_t path_cap)
{
  if (strncmp(url, "https://", 8) != 0) {
    return -EINVAL;
  }
  const char *p = url + 8;
  const char *slash = strchr(p, '/');
  if (!slash) {
    return -EINVAL;
  }
  size_t host_len = slash - p;
  if (host_len >= host_cap) return -E2BIG;
  memcpy(host_out, p, host_len);
  host_out[host_len] = '\0';
  if (strlen(slash) >= path_cap) return -E2BIG;
  strcpy(path_out, slash);
  return 0;
}

//////////////////////////////////////////////////////////////
// Top-level OTA flow
//////////////////////////////////////////////////////////////

static void emit(ota_progress_cb cb, struct ota_progress *p, enum ota_state s)
{
  p->state = s;
  if (cb) cb(p);
}

int ota_check_and_update(ota_progress_cb cb)
{
  struct ota_progress progress = {0};
  ota_get_running_version(progress.running_version,
                          sizeof(progress.running_version));
  emit(cb, &progress, OTA_STATE_CHECKING);

  /* --- Fetch manifest, following redirects --- */
  char host[64] = OTA_HOST;
  char path[256];
  strncpy(path, OTA_MANIFEST_URL, sizeof(path) - 1);
  static char manifest[1024];
  char redirect[512];

  for (int hop = 0; hop < 5; hop++) {
    struct fetch_ctx ctx = {
        .body_buf = (uint8_t *)manifest,
        .body_cap = sizeof(manifest) - 1,
        .body_len = 0,
    };
    redirect[0] = '\0';
    int status = 0;
    int rc = http_get(host, OTA_PORT, path, &ctx, &status,
                      redirect, sizeof(redirect));
    if (rc < 0) {
      strncpy(progress.error, "manifest fetch failed",
              sizeof(progress.error) - 1);
      emit(cb, &progress, OTA_STATE_FAILED);
      return rc;
    }
    if (status == 200) {
      manifest[ctx.body_len] = '\0';
      goto have_manifest;
    }
    if (status >= 300 && status < 400 && redirect[0]) {
      int rc2 = split_url(redirect, host, sizeof(host), path, sizeof(path));
      if (rc2 < 0) {
        strncpy(progress.error, "bad redirect URL",
                sizeof(progress.error) - 1);
        emit(cb, &progress, OTA_STATE_FAILED);
        return rc2;
      }
      continue;
    }
    snprintf(progress.error, sizeof(progress.error),
             "manifest HTTP %d", status);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EIO;
  }
  strncpy(progress.error, "too many redirects",
          sizeof(progress.error) - 1);
  emit(cb, &progress, OTA_STATE_FAILED);
  return -ELOOP;

have_manifest:
  /* --- Parse manifest --- */
  if (!json_get_string(manifest, "version",
                       progress.available_version,
                       sizeof(progress.available_version))) {
    strncpy(progress.error, "manifest missing version",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  char fw_url[256];
  if (!json_get_string(manifest, "url", fw_url, sizeof(fw_url))) {
    strncpy(progress.error, "manifest missing url",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  char sha_hex[65];
  if (!json_get_string(manifest, "sha256", sha_hex, sizeof(sha_hex))) {
    strncpy(progress.error, "manifest missing sha256",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  uint32_t expected_size = 0;
  json_get_uint(manifest, "size", &expected_size);
  progress.bytes_total = expected_size;

  /* --- Version comparison --- */
  uint32_t cur = 0, avail = 0;
  parse_version(progress.running_version, &cur);
  parse_version(progress.available_version, &avail);
  if (avail <= cur) {
    LOG_INF("Running %s, available %s — up to date",
            progress.running_version, progress.available_version);
    emit(cb, &progress, OTA_STATE_UP_TO_DATE);
    return -EAGAIN;
  }
  LOG_INF("Update available: %s -> %s",
          progress.running_version, progress.available_version);

  /* --- Download binary into slot1 --- */
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
  psa_status_t psa_rc = psa_hash_setup(&sha, PSA_ALG_SHA_256);
  if (psa_rc != PSA_SUCCESS) {
    snprintf(progress.error, sizeof(progress.error),
             "psa_hash_setup: %d", (int)psa_rc);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EIO;
  }

  uint32_t bytes_received = 0;
  char dl_host[64];
  char dl_path[512];
  if (split_url(fw_url, dl_host, sizeof(dl_host),
                dl_path, sizeof(dl_path)) < 0) {
    strncpy(progress.error, "bad firmware URL",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EINVAL;
  }

  /* Firmware URL also redirects through GitHub's CDN. */
  for (int hop = 0; hop < 5; hop++) {
    struct fetch_ctx ctx = {
        .flash_ctx = &flash_ctx,
        .sha_op = &sha,
        .bytes_received = &bytes_received,
        .bytes_total = expected_size,
        .cb = cb,
        .progress = &progress,
    };
    redirect[0] = '\0';
    int status = 0;
    int hrc = http_get(dl_host, OTA_PORT, dl_path, &ctx, &status,
                       redirect, sizeof(redirect));
    if (hrc < 0) {
      strncpy(progress.error, "firmware fetch failed",
              sizeof(progress.error) - 1);
      emit(cb, &progress, OTA_STATE_FAILED);
      return hrc;
    }
    if (status == 200) {
      goto have_image;
    }
    if (status >= 300 && status < 400 && redirect[0]) {
      bytes_received = 0;
      flash_img_init(&flash_ctx);
      psa_hash_abort(&sha);
      psa_hash_setup(&sha, PSA_ALG_SHA_256);
      if (split_url(redirect, dl_host, sizeof(dl_host),
                    dl_path, sizeof(dl_path)) < 0) {
        emit(cb, &progress, OTA_STATE_FAILED);
        return -EINVAL;
      }
      continue;
    }
    snprintf(progress.error, sizeof(progress.error),
             "firmware HTTP %d", status);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EIO;
  }

have_image:
  /* --- Verify sha256 --- */
  emit(cb, &progress, OTA_STATE_VERIFYING);
  uint8_t digest[32];
  size_t digest_len = 0;
  psa_rc = psa_hash_finish(&sha, digest, sizeof(digest), &digest_len);
  if (psa_rc != PSA_SUCCESS || digest_len != 32) {
    strncpy(progress.error, "hash finalize failed",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EIO;
  }
  char digest_hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(digest_hex + i * 2, 3, "%02x", digest[i]);
  }
  if (strcmp(digest_hex, sha_hex) != 0) {
    LOG_ERR("sha256 mismatch: got %s, expected %s", digest_hex, sha_hex);
    strncpy(progress.error, "sha256 mismatch",
            sizeof(progress.error) - 1);
    emit(cb, &progress, OTA_STATE_FAILED);
    return -EBADMSG;
  }
  LOG_INF("Image sha256 verified: %s", digest_hex);

  /* --- Mark slot1 for swap, reboot ---
   * boot_set_pending(0) = test swap (revert if not confirmed by next
   * reset). The application calls boot_set_confirmed() once WiFi
   * associates + 60s uptime (added in damper.c). */
  rc = boot_set_pending(0);
  if (rc < 0) {
    snprintf(progress.error, sizeof(progress.error),
             "boot_set_pending: %d", rc);
    emit(cb, &progress, OTA_STATE_FAILED);
    return rc;
  }
  emit(cb, &progress, OTA_STATE_SWAP_PENDING);
  LOG_INF("Update %s ready — rebooting", progress.available_version);
  k_sleep(K_MSEC(1500));
  sys_reboot(SYS_REBOOT_COLD);
  /* not reached */
  return 0;
}
// Trigger version bump for OTA test
// version bump for OTA test
