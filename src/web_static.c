// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

LOG_MODULE_REGISTER(web_static, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// HTTP Service (defined in http_api.c)
//////////////////////////////////////////////////////////////

extern const struct http_service_desc damper_http_service;

//////////////////////////////////////////////////////////////
// Embedded Assets
//////////////////////////////////////////////////////////////

#if HAS_INDEX_HTML
static const uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};
#endif

#if WEB_ASSET_COUNT > 0
#include "web_assets.h"
#endif

//////////////////////////////////////////////////////////////
// Index HTML Resource (no-cache — references hashed assets)
//////////////////////////////////////////////////////////////

#if HAS_INDEX_HTML
static struct http_resource_detail_static index_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_STATIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_encoding = "gzip",
        .content_type = "text/html",
    },
    .static_data = index_html_gz,
    .static_data_len = sizeof(index_html_gz),
};

HTTP_RESOURCE_DEFINE(web_index, damper_http_service,
                     "/", &index_resource_detail);
HTTP_RESOURCE_DEFINE(web_index_html, damper_http_service,
                     "/index.html", &index_resource_detail);
#endif

//////////////////////////////////////////////////////////////
// Hashed Asset Handler (immutable cache)
//////////////////////////////////////////////////////////////

#if WEB_ASSET_COUNT > 0

static struct http_header asset_hdrs[3] = {
    {.name = "Content-Encoding", .value = "gzip"},
    {.name = "Cache-Control", .value = "public, max-age=31536000, immutable"},
    {.name = "Content-Type", .value = NULL},
};

static int handle_asset(struct http_client_ctx *client,
                        enum http_transaction_status status,
                        const struct http_request_ctx *request_ctx,
                        struct http_response_ctx *response_ctx,
                        void *user_data)
{
  if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
    return 0;
  }

  const char *url = client->url_buffer;

  for (int i = 0; i < WEB_ASSET_COUNT; i++) {
    if (strcmp(url, web_assets[i].path) == 0) {
      const struct web_asset *a = &web_assets[i];

      LOG_INF("Serving %s (%zu bytes, content-type %s)", a->path, a->len,
              a->content_type);

      response_ctx->body = a->data;
      response_ctx->body_len = a->len;
      response_ctx->final_chunk = true;
      response_ctx->status = HTTP_200_OK;
      asset_hdrs[2].value = a->content_type;
      response_ctx->headers = asset_hdrs;
      response_ctx->header_count = ARRAY_SIZE(asset_hdrs);
      return 0;
    }
  }

  response_ctx->status = HTTP_404_NOT_FOUND;
  response_ctx->final_chunk = true;
  return 0;
}

static struct http_resource_detail_dynamic asset_resource_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
    },
    .cb = handle_asset,
};

HTTP_RESOURCE_DEFINE(web_assets_handler, damper_http_service,
                     "/assets/*", &asset_resource_detail);

#endif
