# WiFi + JSON API + Web Frontend Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add WiFi connectivity, a JSON REST API, and an embedded web frontend to the auto-damper ESP32 firmware so the device can be controlled from a phone browser instead of serial.

**Architecture:** WiFi STA mode connects to user's network. Zephyr HTTP server on port 80 serves gzipped static frontend and JSON API endpoints. NVS stores WiFi credentials for auto-connect on boot. Uses Zephyr's built-in wifi shell for ad-hoc connections, custom `damper wifi save` for persistence.

**Tech Stack:** Zephyr HTTP server, NVS flash storage, Voidable web components (https://voidable.dev/), CMake `generate_inc_file_for_target` for gzip embedding.

**Reference implementations:**
- `apps/slate/` — WiFi management, NVS storage, HTTP server lifecycle (Slate master branch)
- `apps/roastee/packages/experiments/bucket/` — Gzip embedding, static serving, CMake pattern (roastee `stack-experiments` branch, commit `1c1ea85`)

---

### Task 1: NVS Config Storage

Adapted from Slate's `config.h` + `nvs_storage.c` pattern. Provides typed, versioned key-value storage backed by Zephyr NVS.

**Files:**
- Create: `include/auto_damper/config.h`
- Create: `src/config/nvs_storage.c`

**Step 1: Add NVS Kconfig to prj.conf**

Append to `prj.conf`:

```
# Flash Storage
CONFIG_FLASH=y

# NVS for persistent config
CONFIG_NVS=y
CONFIG_NVS_DATA_CRC=y
```

**Step 2: Create config header**

Create `include/auto_damper/config.h`:

```c
// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_CONFIG_H
#define AUTO_DAMPER_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define CONFIG_TYPE_WIFI_CREDENTIALS 0x0001
#define CONFIG_TYPE_WIFI_SETTINGS    0x0002

#define NVS_ID_WIFI_CREDENTIALS 0x0001
#define NVS_ID_WIFI_SETTINGS    0x0002

int config_init(void);
int config_save(uint16_t id, uint16_t type, uint16_t version,
                const void *data, size_t len);
int config_load(uint16_t id, uint16_t expected_type, uint16_t *version,
                void *data, size_t len);
int config_exists(uint16_t id);
int config_delete(uint16_t id);

#endif
```

**Step 3: Create NVS storage implementation**

Create `src/config/nvs_storage.c`:

```c
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>

#include <auto_damper/config.h>

LOG_MODULE_REGISTER(config, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define STORAGE_PARTITION       storage_partition
#define STORAGE_PARTITION_ID    FIXED_PARTITION_ID(STORAGE_PARTITION)
#define STORAGE_PARTITION_DEVICE FIXED_PARTITION_DEVICE(STORAGE_PARTITION)
#define STORAGE_PARTITION_OFFSET FIXED_PARTITION_OFFSET(STORAGE_PARTITION)

#define NVS_SECTOR_COUNT 3

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

struct config_header {
  uint16_t type;
  uint16_t version;
  uint16_t length;
};

static struct nvs_fs fs;
static bool initialized;

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int config_init(void)
{
  struct flash_pages_info info;

  if (initialized) {
    return 0;
  }

  fs.flash_device = STORAGE_PARTITION_DEVICE;
  if (!device_is_ready(fs.flash_device)) {
    LOG_ERR("Flash device not ready");
    return -ENODEV;
  }

  fs.offset = STORAGE_PARTITION_OFFSET;

  int rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
  if (rc) {
    LOG_ERR("Failed to get flash page info: %d", rc);
    return rc;
  }

  fs.sector_size = info.size;
  fs.sector_count = NVS_SECTOR_COUNT;

  rc = nvs_mount(&fs);
  if (rc) {
    LOG_ERR("Failed to mount NVS: %d", rc);
    return rc;
  }

  initialized = true;
  LOG_INF("Config storage initialized");
  return 0;
}

int config_save(uint16_t id, uint16_t type, uint16_t version,
                const void *data, size_t len)
{
  if (!initialized || !data || len == 0) {
    return -EINVAL;
  }

  struct config_header header = {
      .type = type, .version = version, .length = len};

  size_t total_len = sizeof(header) + len;
  uint8_t *buf = k_malloc(total_len);
  if (!buf) {
    return -ENOMEM;
  }

  memcpy(buf, &header, sizeof(header));
  memcpy(buf + sizeof(header), data, len);

  int rc = nvs_write(&fs, id, buf, total_len);
  k_free(buf);

  if (rc < 0) {
    LOG_ERR("Failed to write config 0x%04X: %d", id, rc);
    return rc;
  }

  return 0;
}

int config_load(uint16_t id, uint16_t expected_type, uint16_t *version,
                void *data, size_t len)
{
  if (!initialized || !data || !version || len == 0) {
    return -EINVAL;
  }

  size_t total_len = sizeof(struct config_header) + len;
  uint8_t *buf = k_malloc(total_len);
  if (!buf) {
    return -ENOMEM;
  }

  int rc = nvs_read(&fs, id, buf, total_len);
  if (rc < 0) {
    k_free(buf);
    return rc;
  }

  if (rc != total_len) {
    k_free(buf);
    return -EINVAL;
  }

  struct config_header header;
  memcpy(&header, buf, sizeof(header));

  if (header.type != expected_type || header.length != len) {
    k_free(buf);
    return -EINVAL;
  }

  memcpy(data, buf + sizeof(header), len);
  *version = header.version;
  k_free(buf);

  return 0;
}

int config_exists(uint16_t id)
{
  if (!initialized) {
    return -EINVAL;
  }

  struct config_header header;
  int rc = nvs_read(&fs, id, &header, sizeof(header));

  if (rc == sizeof(header)) {
    return 1;
  } else if (rc == -ENOENT) {
    return 0;
  }
  return rc;
}

int config_delete(uint16_t id)
{
  if (!initialized) {
    return -EINVAL;
  }

  int rc = nvs_delete(&fs, id);
  if (rc < 0 && rc != -ENOENT) {
    LOG_ERR("Failed to delete config 0x%04X: %d", id, rc);
    return rc;
  }
  return 0;
}
```

**Step 4: Call config_init() from main**

In `src/main.c`, add `#include <auto_damper/config.h>` to includes, and add to `main()` (create `main()` if it doesn't exist, or add at the start of existing init):

```c
int main(void)
{
  int rc = config_init();
  if (rc) {
    LOG_ERR("Config init failed: %d", rc);
  }
  return 0;
}
```

Note: `src/main.c` currently has no `main()` — the threads start via `K_THREAD_DEFINE`. Add `main()` at the end of the file.

**Step 5: Build to verify**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

Expected: Build succeeds. NVS links against the storage_partition defined in the ESP32 board's partition table (192KB at 0x3B0000).

**Step 6: Commit**

```
feat(auto-damper): add NVS config storage layer
```

---

### Task 2: WiFi Credential Persistence

**Files:**
- Create: `include/auto_damper/wifi_config.h`
- Create: `src/config/wifi_config.c`

**Step 1: Create wifi_config header**

Create `include/auto_damper/wifi_config.h`:

```c
// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_WIFI_CONFIG_H
#define AUTO_DAMPER_WIFI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASSWORD_MAX_LEN 64

struct wifi_credentials {
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char password[WIFI_PASSWORD_MAX_LEN + 1];
};

struct wifi_settings {
  bool auto_connect;
  uint32_t reconnect_interval;
};

int wifi_config_save(const char *ssid, const char *password);
int wifi_config_load(struct wifi_credentials *creds);
bool wifi_config_exists(void);
int wifi_config_delete(void);

int wifi_settings_save(bool auto_connect, uint32_t reconnect_interval);
int wifi_settings_load(struct wifi_settings *settings);

#endif
```

**Step 2: Create wifi_config implementation**

Create `src/config/wifi_config.c`:

```c
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <auto_damper/config.h>
#include <auto_damper/wifi_config.h>

LOG_MODULE_REGISTER(wifi_config, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define WIFI_CONFIG_VERSION   1
#define WIFI_SETTINGS_VERSION 1
#define WIFI_DEFAULT_AUTO_CONNECT true
#define WIFI_DEFAULT_RECONNECT_INTERVAL 30

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int wifi_config_save(const char *ssid, const char *password)
{
  if (!ssid || !password) {
    return -EINVAL;
  }

  if (strlen(ssid) == 0 || strlen(ssid) > WIFI_SSID_MAX_LEN) {
    return -EINVAL;
  }

  struct wifi_credentials creds = {0};
  strncpy(creds.ssid, ssid, WIFI_SSID_MAX_LEN);
  strncpy(creds.password, password, WIFI_PASSWORD_MAX_LEN);

  int rc = config_save(NVS_ID_WIFI_CREDENTIALS,
                       CONFIG_TYPE_WIFI_CREDENTIALS,
                       WIFI_CONFIG_VERSION, &creds, sizeof(creds));
  if (rc == 0) {
    LOG_INF("WiFi credentials saved: SSID='%s'", ssid);
  }
  return rc;
}

int wifi_config_load(struct wifi_credentials *creds)
{
  if (!creds) {
    return -EINVAL;
  }

  uint16_t version;
  int rc = config_load(NVS_ID_WIFI_CREDENTIALS,
                       CONFIG_TYPE_WIFI_CREDENTIALS,
                       &version, creds, sizeof(*creds));
  if (rc == 0) {
    creds->ssid[WIFI_SSID_MAX_LEN] = '\0';
    creds->password[WIFI_PASSWORD_MAX_LEN] = '\0';
  }
  return rc;
}

bool wifi_config_exists(void)
{
  return config_exists(NVS_ID_WIFI_CREDENTIALS) == 1;
}

int wifi_config_delete(void)
{
  return config_delete(NVS_ID_WIFI_CREDENTIALS);
}

int wifi_settings_save(bool auto_connect, uint32_t reconnect_interval)
{
  struct wifi_settings settings = {
      .auto_connect = auto_connect,
      .reconnect_interval = reconnect_interval,
  };
  return config_save(NVS_ID_WIFI_SETTINGS, CONFIG_TYPE_WIFI_SETTINGS,
                     WIFI_SETTINGS_VERSION, &settings, sizeof(settings));
}

int wifi_settings_load(struct wifi_settings *settings)
{
  if (!settings) {
    return -EINVAL;
  }

  uint16_t version;
  int rc = config_load(NVS_ID_WIFI_SETTINGS, CONFIG_TYPE_WIFI_SETTINGS,
                       &version, settings, sizeof(*settings));
  if (rc == -ENOENT) {
    settings->auto_connect = WIFI_DEFAULT_AUTO_CONNECT;
    settings->reconnect_interval = WIFI_DEFAULT_RECONNECT_INTERVAL;
    return 0;
  }
  return rc;
}
```

**Step 3: Build to verify**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

**Step 4: Commit**

```
feat(auto-damper): add WiFi credential persistence
```

---

### Task 3: WiFi Module

WiFi management with auto-connect from NVS. Adapted from Slate's wifi.c but simplified (no zbus command channel — shell commands call API directly).

**Files:**
- Create: `include/auto_damper/wifi.h`
- Create: `src/network/wifi.c`
- Modify: `prj.conf`

**Step 1: Add networking + WiFi Kconfig to prj.conf**

Append to `prj.conf`:

```
# Networking
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_POSIX_API=y
CONFIG_NET_DHCPV4=y
CONFIG_TEST_RANDOM_GENERATOR=y

# Stack Sizes (WiFi + HTTP need more)
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SHELL_STACK_SIZE=4096
CONFIG_NET_TX_STACK_SIZE=2048
CONFIG_NET_RX_STACK_SIZE=2048

# Network Buffers
CONFIG_NET_PKT_RX_COUNT=16
CONFIG_NET_PKT_TX_COUNT=16
CONFIG_NET_BUF_RX_COUNT=64
CONFIG_NET_BUF_TX_COUNT=64
CONFIG_NET_MAX_CONTEXTS=16
CONFIG_NET_MAX_CONN=16

# WiFi
CONFIG_WIFI=y
CONFIG_WIFI_LOG_LEVEL_ERR=y
CONFIG_NET_L2_WIFI_MGMT=y
CONFIG_NET_L2_WIFI_SHELL=y
CONFIG_NET_MGMT=y
CONFIG_NET_MGMT_EVENT=y
CONFIG_NET_MGMT_EVENT_INFO=y
CONFIG_NET_MGMT_EVENT_QUEUE_SIZE=16
CONFIG_NET_MGMT_EVENT_QUEUE_TIMEOUT=5000
CONFIG_NET_CONNECTION_MANAGER=y

# Hostname + mDNS
CONFIG_NET_HOSTNAME_ENABLE=y
CONFIG_NET_HOSTNAME_DYNAMIC=y
CONFIG_NET_HOSTNAME="auto-damper"
CONFIG_NET_HOSTNAME_MAX_LEN=63
CONFIG_MDNS_RESPONDER=y

# Hardware Info (for unique hostname suffix)
CONFIG_HWINFO=y

# Network debug
CONFIG_NET_SHELL=y
CONFIG_NET_LOG=y
```

**Step 2: Create wifi header**

Create `include/auto_damper/wifi.h`:

```c
// SPDX-License-Identifier: Apache-2.0
#ifndef AUTO_DAMPER_WIFI_H
#define AUTO_DAMPER_WIFI_H

#include <stdbool.h>
#include <stddef.h>

int wifi_connect(const char *ssid, const char *password);
int wifi_disconnect(void);
bool wifi_is_connected(void);
int wifi_get_ip_address(char *addr_str, size_t buf_len);

#endif
```

**Step 3: Create wifi implementation**

Create `src/network/wifi.c`:

```c
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>
#include <stdio.h>

#include <auto_damper/wifi.h>
#include <auto_damper/wifi_config.h>

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define WIFI_THREAD_STACK_SIZE 2048
#define WIFI_THREAD_PRIORITY 7
#define WIFI_LOOP_SLEEP_MS 1000

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

struct wifi_state {
  bool connected;
  bool connecting;
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char password[WIFI_PASSWORD_MAX_LEN + 1];
  bool has_credentials;
  bool auto_connect;
  uint32_t reconnect_interval;
  uint64_t last_connect_attempt_time;
};

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback net_addr_mgmt_cb;
static struct wifi_state wifi_state;
static K_THREAD_STACK_DEFINE(wifi_thread_stack, WIFI_THREAD_STACK_SIZE);
static struct k_thread wifi_thread_data;

//////////////////////////////////////////////////////////////
// HTTP Server Lifecycle
//////////////////////////////////////////////////////////////

extern void http_api_start(void);
extern void http_api_stop(void);

//////////////////////////////////////////////////////////////
// Forward Declarations
//////////////////////////////////////////////////////////////

static void wifi_thread(void *p1, void *p2, void *p3);
static int wifi_connect_internal(void);
static void set_hostname_from_device_id(void);

//////////////////////////////////////////////////////////////
// Event Handlers
//////////////////////////////////////////////////////////////

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *iface)
{
  ARG_UNUSED(iface);

  switch (mgmt_event) {
  case NET_EVENT_WIFI_CONNECT_RESULT: {
    const struct wifi_status *status =
        (const struct wifi_status *)cb->info;

    wifi_state.connecting = false;

    if (status && status->conn_status == WIFI_STATUS_CONN_SUCCESS) {
      LOG_INF("WiFi connected to '%s'", wifi_state.ssid);
      wifi_state.connected = true;
    } else {
      LOG_ERR("WiFi connection failed: %s",
              status ? wifi_conn_status_txt(status->conn_status)
                     : "unknown");
      wifi_state.connected = false;

      if (status &&
          status->conn_status == WIFI_STATUS_CONN_WRONG_PASSWORD) {
        LOG_ERR("Wrong password — disabling auto-connect");
        wifi_state.auto_connect = false;
      }
    }
    break;
  }

  case NET_EVENT_WIFI_DISCONNECT_RESULT:
    LOG_INF("WiFi disconnected");
    wifi_state.connected = false;
    wifi_state.connecting = false;
    http_api_stop();
    break;

  default:
    break;
  }
}

static void net_addr_event_handler(struct net_mgmt_event_callback *cb,
                                   uint64_t mgmt_event,
                                   struct net_if *iface)
{
  ARG_UNUSED(cb);
  ARG_UNUSED(iface);

  if (!wifi_state.connected) {
    return;
  }

  if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND ||
      mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
    char addr[16];
    if (wifi_get_ip_address(addr, sizeof(addr)) == 0) {
      LOG_INF("IP address: %s", addr);
    }
    http_api_start();
  }
}

//////////////////////////////////////////////////////////////
// WiFi Thread
//////////////////////////////////////////////////////////////

static void wifi_thread(void *p1, void *p2, void *p3)
{
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  LOG_INF("WiFi thread started");

  struct wifi_credentials creds;
  struct wifi_settings settings;

  if (wifi_config_load(&creds) == 0) {
    strncpy(wifi_state.ssid, creds.ssid, WIFI_SSID_MAX_LEN);
    strncpy(wifi_state.password, creds.password, WIFI_PASSWORD_MAX_LEN);
    wifi_state.has_credentials = true;
    LOG_INF("WiFi credentials loaded: SSID='%s'", wifi_state.ssid);
  }

  if (wifi_settings_load(&settings) == 0) {
    wifi_state.auto_connect = settings.auto_connect;
    wifi_state.reconnect_interval = settings.reconnect_interval;
  }

  set_hostname_from_device_id();

  if (wifi_state.auto_connect && wifi_state.has_credentials) {
    LOG_INF("Auto-connecting to WiFi...");
    wifi_connect_internal();
  }

  while (true) {
    if (wifi_state.auto_connect && wifi_state.has_credentials &&
        !wifi_state.connected && !wifi_state.connecting) {
      uint64_t elapsed =
          k_uptime_get() - wifi_state.last_connect_attempt_time;
      if (elapsed >= wifi_state.reconnect_interval * 1000ULL) {
        LOG_INF("Retrying WiFi connection...");
        wifi_connect_internal();
      }
    }
    k_sleep(K_MSEC(WIFI_LOOP_SLEEP_MS));
  }
}

//////////////////////////////////////////////////////////////
// Internal Functions
//////////////////////////////////////////////////////////////

static int wifi_connect_internal(void)
{
  if (!wifi_state.has_credentials || wifi_state.connecting) {
    return -EINVAL;
  }

  struct net_if *iface = net_if_get_wifi_sta();
  if (!iface) {
    LOG_ERR("WiFi STA interface not available");
    return -ENODEV;
  }

  wifi_state.connecting = true;
  wifi_state.last_connect_attempt_time = k_uptime_get();

  struct wifi_connect_req_params params = {
      .ssid = (const uint8_t *)wifi_state.ssid,
      .ssid_length = strlen(wifi_state.ssid),
      .psk = strlen(wifi_state.password) > 0
                 ? (const uint8_t *)wifi_state.password
                 : NULL,
      .psk_length = strlen(wifi_state.password),
      .security = strlen(wifi_state.password) > 0
                      ? WIFI_SECURITY_TYPE_PSK
                      : WIFI_SECURITY_TYPE_NONE,
      .band = WIFI_FREQ_BAND_2_4_GHZ,
      .channel = WIFI_CHANNEL_ANY,
      .timeout = SYS_FOREVER_MS,
  };

  LOG_INF("Connecting to '%s'...", wifi_state.ssid);

  int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
                     sizeof(params));
  if (ret) {
    LOG_ERR("WiFi connect request failed: %d", ret);
    wifi_state.connecting = false;
    return ret;
  }

  return 0;
}

static void set_hostname_from_device_id(void)
{
  uint8_t device_id[16];
  ssize_t id_len = hwinfo_get_device_id(device_id, sizeof(device_id));
  if (id_len < 2) {
    return;
  }

  char hostname[64];
  snprintf(hostname, sizeof(hostname), CONFIG_NET_HOSTNAME "-%02x%02x",
           device_id[id_len - 2], device_id[id_len - 1]);

  net_hostname_set(hostname, strlen(hostname));
  LOG_INF("Hostname: %s", hostname);
}

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int wifi_connect(const char *ssid, const char *password)
{
  if (!ssid) {
    return -EINVAL;
  }

  strncpy(wifi_state.ssid, ssid, WIFI_SSID_MAX_LEN);
  wifi_state.ssid[WIFI_SSID_MAX_LEN] = '\0';

  if (password) {
    strncpy(wifi_state.password, password, WIFI_PASSWORD_MAX_LEN);
    wifi_state.password[WIFI_PASSWORD_MAX_LEN] = '\0';
  } else {
    wifi_state.password[0] = '\0';
  }

  wifi_state.has_credentials = true;
  return wifi_connect_internal();
}

int wifi_disconnect(void)
{
  struct net_if *iface = net_if_get_wifi_sta();
  if (!iface) {
    return -ENODEV;
  }
  return net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
}

bool wifi_is_connected(void)
{
  return wifi_state.connected;
}

int wifi_get_ip_address(char *addr_str, size_t buf_len)
{
  if (!addr_str || buf_len < 16) {
    return -EINVAL;
  }

  struct net_if *iface = net_if_get_wifi_sta();
  if (!iface) {
    return -ENODEV;
  }

  struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
  if (!ipv4) {
    return -ENOENT;
  }

  struct net_if_addr *unicast = &ipv4->unicast[0].ipv4;
  if (!unicast || !unicast->is_used) {
    return -ENOENT;
  }

  net_addr_ntop(AF_INET, &unicast->address.in_addr, addr_str, buf_len);
  return 0;
}

//////////////////////////////////////////////////////////////
// Init
//////////////////////////////////////////////////////////////

static int wifi_init(void)
{
  net_mgmt_init_event_callback(
      &wifi_mgmt_cb, wifi_event_handler,
      NET_EVENT_WIFI_CONNECT_RESULT |
          NET_EVENT_WIFI_DISCONNECT_RESULT);
  net_mgmt_add_event_callback(&wifi_mgmt_cb);

  net_mgmt_init_event_callback(
      &net_addr_mgmt_cb, net_addr_event_handler,
      NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_DHCP_BOUND);
  net_mgmt_add_event_callback(&net_addr_mgmt_cb);

  k_thread_create(&wifi_thread_data, wifi_thread_stack,
                  K_THREAD_STACK_SIZEOF(wifi_thread_stack), wifi_thread,
                  NULL, NULL, NULL, WIFI_THREAD_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&wifi_thread_data, "wifi");

  return 0;
}

SYS_INIT(wifi_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

**Step 4: Build to verify**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

Expected: Build succeeds. WiFi + BLE coexist on ESP32 (both enabled in board DTS).

**Step 5: Commit**

```
feat(auto-damper): add WiFi module with auto-connect
```

---

### Task 4: HTTP Server + JSON API

Dynamic HTTP resource handlers serve JSON responses. The HTTP server starts when WiFi gets an IP address and stops on disconnect.

**Files:**
- Create: `src/http_api.c`
- Create: `sections-rom.ld`
- Modify: `CMakeLists.txt`
- Modify: `prj.conf`

**Step 1: Add HTTP server Kconfig to prj.conf**

Append to `prj.conf`:

```
# HTTP Server
CONFIG_HTTP_PARSER=y
CONFIG_HTTP_PARSER_URL=y
CONFIG_HTTP_SERVER=y
CONFIG_HTTP_SERVER_MAX_CLIENTS=4
CONFIG_HTTP_SERVER_MAX_URL_LENGTH=128

# POSIX File Descriptors
CONFIG_ZVFS_OPEN_MAX=36
CONFIG_ZVFS_POLL_MAX=32
CONFIG_EVENTFD=y
CONFIG_ZVFS_EVENTFD_MAX=8
```

**Step 2: Create linker section**

Create `sections-rom.ld`:

```
/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_damper_http_service, Z_LINK_ITERABLE_SUBALIGN)
```

**Step 3: Update CMakeLists.txt**

Replace the contents of `CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(auto-damper)

FILE(GLOB_RECURSE app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE include)

# Linker section for HTTP resources
zephyr_linker_sources(SECTIONS sections-rom.ld)

# Web resources (gzip-compressed, embedded in firmware)
set(WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated/)

if(EXISTS ${WEB_DIR}/index.html)
  generate_inc_file_for_target(app
    ${WEB_DIR}/index.html ${GEN_DIR}/index.html.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_INDEX_HTML=1)
else()
  target_compile_definitions(app PRIVATE HAS_INDEX_HTML=0)
endif()

if(EXISTS ${WEB_DIR}/app.js)
  generate_inc_file_for_target(app
    ${WEB_DIR}/app.js ${GEN_DIR}/app.js.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_APP_JS=1)
else()
  target_compile_definitions(app PRIVATE HAS_APP_JS=0)
endif()

if(EXISTS ${WEB_DIR}/app.css)
  generate_inc_file_for_target(app
    ${WEB_DIR}/app.css ${GEN_DIR}/app.css.gz.inc --gzip)
  target_compile_definitions(app PRIVATE HAS_APP_CSS=1)
else()
  target_compile_definitions(app PRIVATE HAS_APP_CSS=0)
endif()
```

**Step 4: Create HTTP API implementation**

Create `src/http_api.c`:

```c
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/zbus/zbus.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <auto_damper/damper.h>
#include <auto_damper/heater.h>
#include <auto_damper/wifi.h>
#include <auto_damper/zbus.h>

LOG_MODULE_REGISTER(http_api, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// BLE Externs (from heater_ble.c and shell.c)
//////////////////////////////////////////////////////////////

extern int heater_ble_scan(int timeout_sec);
extern int heater_ble_scan_stop(void);
extern int heater_ble_connect(int index);
extern int heater_ble_disconnect(void);
extern void heater_ble_set_protocol(const struct heater_protocol *proto);
extern const struct heater_protocol *heater_ble_get_protocol(void);
extern bool heater_ble_is_connected(void);
extern bool heater_ble_is_scanning(void);
extern int heater_ble_get_scan_count(void);
extern int heater_ble_send_power(bool on);
extern int heater_ble_send_set_temp(int temp_c);

struct ble_scan_result {
  bt_addr_le_t addr;
  char name[32];
  int8_t rssi;
  const struct heater_protocol *protocol;
};

extern const struct ble_scan_result *heater_ble_get_scan_result(int index);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define JSON_BUF_SIZE 1024
#define BODY_BUF_SIZE 256
#define PUB_TIMEOUT K_MSEC(100)

//////////////////////////////////////////////////////////////
// HTTP Service
//////////////////////////////////////////////////////////////

#define HTTP_PORT 80

static uint16_t http_port = HTTP_PORT;
static bool server_running;

HTTP_SERVICE_DEFINE(damper_http_service, NULL, &http_port,
                    CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL, NULL);

//////////////////////////////////////////////////////////////
// JSON Response Helpers
//////////////////////////////////////////////////////////////

static int send_json(struct http_response_ctx *rsp, char *buf, int len)
{
  rsp->body = (const uint8_t *)buf;
  rsp->body_len = len;
  rsp->final_chunk = true;
  rsp->status = HTTP_200_OK;
  rsp->header_count = 0;
  return 0;
}

static int send_json_error(struct http_response_ctx *rsp, char *buf,
                           int status_code, const char *message)
{
  int len = snprintf(buf, JSON_BUF_SIZE,
                     "{\"error\":\"%s\"}", message);
  rsp->body = (const uint8_t *)buf;
  rsp->body_len = len;
  rsp->final_chunk = true;
  rsp->status = status_code == 400 ? HTTP_400_BAD_REQ : HTTP_500_INTERNAL_ERROR;
  rsp->header_count = 0;
  return 0;
}

//////////////////////////////////////////////////////////////
// GET /api/status
//////////////////////////////////////////////////////////////

static char status_buf[JSON_BUF_SIZE];

static int handle_api_status(struct http_client_ctx *client,
                             enum http_data_status status,
                             const struct http_request_ctx *req,
                             struct http_response_ctx *rsp,
                             void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  struct damper_config *cfg = damper_config_get();
  struct temperature_data temp = {0};
  struct damper_data data = {0};

  zbus_chan_read(&temperature_data_chan, &temp, PUB_TIMEOUT);
  zbus_chan_read(&damper_data_chan, &data, PUB_TIMEOUT);

  int len = snprintf(status_buf, sizeof(status_buf),
      "{"
      "\"temperature\":%.1f,"
      "\"route\":\"%s\","
      "\"mode\":\"%s\","
      "\"servo_degrees\":%.1f,"
      "\"config\":{"
        "\"temp_high\":%.1f,"
        "\"temp_low\":%.1f,"
        "\"servo_inside\":%.1f,"
        "\"servo_outside\":%.1f"
      "}"
      "}",
      temp.celsius,
      data.route == DAMPER_ROUTE_INSIDE ? "inside" : "outside",
      data.mode == DAMPER_MODE_AUTO ? "auto" : "override",
      data.servo_degrees,
      cfg->temp_high, cfg->temp_low,
      cfg->servo_inside_deg, cfg->servo_outside_deg);

  return send_json(rsp, status_buf, len);
}

static struct http_resource_detail_dynamic api_status_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_type = "application/json",
    },
    .cb = handle_api_status,
};

HTTP_RESOURCE_DEFINE(api_status_res, damper_http_service,
                     "/api/status", &api_status_detail);

//////////////////////////////////////////////////////////////
// POST /api/config
//////////////////////////////////////////////////////////////

static char config_body[BODY_BUF_SIZE];
static char config_resp[JSON_BUF_SIZE];

static int handle_api_config(struct http_client_ctx *client,
                             enum http_data_status status,
                             const struct http_request_ctx *req,
                             struct http_response_ctx *rsp,
                             void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, config_resp, 400, "invalid body");
  }

  memcpy(config_body, req->data, req->data_len);
  config_body[req->data_len] = '\0';

  char param[32] = {0};
  double value = 0;

  char *p = strstr(config_body, "\"param\"");
  char *v = strstr(config_body, "\"value\"");

  if (!p || !v) {
    return send_json_error(rsp, config_resp, 400,
                           "need param and value");
  }

  char *q1 = strchr(p + 7, '"');
  if (q1) {
    char *q2 = strchr(q1 + 1, '"');
    if (q2 && (q2 - q1 - 1) < (int)sizeof(param)) {
      memcpy(param, q1 + 1, q2 - q1 - 1);
    }
  }

  char *colon = strchr(v + 7, ':');
  if (colon) {
    value = strtod(colon + 1, NULL);
  }

  if (param[0] == '\0') {
    return send_json_error(rsp, config_resp, 400, "invalid param");
  }

  struct damper_command cmd;

  if (strcmp(param, "temp_high") == 0) {
    cmd.type = DAMPER_CMD_SET_TEMP_HIGH;
  } else if (strcmp(param, "temp_low") == 0) {
    cmd.type = DAMPER_CMD_SET_TEMP_LOW;
  } else if (strcmp(param, "servo_inside") == 0) {
    cmd.type = DAMPER_CMD_SET_SERVO_INSIDE;
  } else if (strcmp(param, "servo_outside") == 0) {
    cmd.type = DAMPER_CMD_SET_SERVO_OUTSIDE;
  } else {
    return send_json_error(rsp, config_resp, 400, "unknown param");
  }

  cmd.value = value;
  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  int len = snprintf(config_resp, sizeof(config_resp),
                     "{\"ok\":true,\"param\":\"%s\",\"value\":%.1f}",
                     param, value);
  return send_json(rsp, config_resp, len);
}

static struct http_resource_detail_dynamic api_config_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_config,
};

HTTP_RESOURCE_DEFINE(api_config_res, damper_http_service,
                     "/api/config", &api_config_detail);

//////////////////////////////////////////////////////////////
// POST /api/override
//////////////////////////////////////////////////////////////

static char override_body[BODY_BUF_SIZE];
static char override_resp[JSON_BUF_SIZE];

static int handle_api_override(struct http_client_ctx *client,
                               enum http_data_status status,
                               const struct http_request_ctx *req,
                               struct http_response_ctx *rsp,
                               void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, override_resp, 400, "invalid body");
  }

  memcpy(override_body, req->data, req->data_len);
  override_body[req->data_len] = '\0';

  struct damper_command cmd = {.value = 0.0};

  if (strstr(override_body, "\"inside\"")) {
    cmd.type = DAMPER_CMD_OVERRIDE_INSIDE;
  } else if (strstr(override_body, "\"outside\"")) {
    cmd.type = DAMPER_CMD_OVERRIDE_OUTSIDE;
  } else if (strstr(override_body, "\"auto\"")) {
    cmd.type = DAMPER_CMD_SET_AUTO;
  } else {
    return send_json_error(rsp, override_resp, 400,
                           "mode must be inside, outside, or auto");
  }

  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  const char *mode_str =
      cmd.type == DAMPER_CMD_OVERRIDE_INSIDE    ? "inside"
      : cmd.type == DAMPER_CMD_OVERRIDE_OUTSIDE ? "outside"
                                                : "auto";
  int len = snprintf(override_resp, sizeof(override_resp),
                     "{\"ok\":true,\"mode\":\"%s\"}", mode_str);
  return send_json(rsp, override_resp, len);
}

static struct http_resource_detail_dynamic api_override_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_override,
};

HTTP_RESOURCE_DEFINE(api_override_res, damper_http_service,
                     "/api/override", &api_override_detail);

//////////////////////////////////////////////////////////////
// GET /api/ble/status
//////////////////////////////////////////////////////////////

static char ble_status_buf[JSON_BUF_SIZE];

static int handle_api_ble_status(struct http_client_ctx *client,
                                 enum http_data_status status,
                                 const struct http_request_ctx *req,
                                 struct http_response_ctx *rsp,
                                 void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  const struct heater_protocol *p = heater_ble_get_protocol();
  bool connected = heater_ble_is_connected();

  int len;

  if (connected) {
    struct heater_data hdata = {0};
    zbus_chan_read(&heater_data_chan, &hdata, K_MSEC(100));

    len = snprintf(ble_status_buf, sizeof(ble_status_buf),
        "{"
        "\"connected\":true,"
        "\"protocol\":\"%s\","
        "\"scanning\":%s,"
        "\"telemetry\":{"
          "\"power\":\"%s\","
          "\"step\":\"%s\","
          "\"exhaust_temp\":%.1f,"
          "\"ambient_temp\":%.1f,"
          "\"voltage\":%.1f,"
          "\"target_temp\":%d,"
          "\"gear\":%d,"
          "\"error\":%d"
        "}"
        "}",
        p ? p->name : "none",
        heater_ble_is_scanning() ? "true" : "false",
        heater_power_state_str(hdata.power),
        heater_run_step_str(hdata.step),
        hdata.exhaust_temp_c, hdata.ambient_temp_c,
        hdata.voltage, hdata.target_temp,
        hdata.gear_level, hdata.error_code);
  } else {
    len = snprintf(ble_status_buf, sizeof(ble_status_buf),
        "{"
        "\"connected\":false,"
        "\"protocol\":\"%s\","
        "\"scanning\":%s"
        "}",
        p ? p->name : "none",
        heater_ble_is_scanning() ? "true" : "false");
  }

  return send_json(rsp, ble_status_buf, len);
}

static struct http_resource_detail_dynamic api_ble_status_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_status,
};

HTTP_RESOURCE_DEFINE(api_ble_status_res, damper_http_service,
                     "/api/ble/status", &api_ble_status_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/scan
//////////////////////////////////////////////////////////////

static char scan_resp[JSON_BUF_SIZE];

static int handle_api_ble_scan(struct http_client_ctx *client,
                               enum http_data_status status,
                               const struct http_request_ctx *req,
                               struct http_response_ctx *rsp,
                               void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  int timeout = 5;

  if (req->data_len > 0 && req->data_len < BODY_BUF_SIZE) {
    char body[BODY_BUF_SIZE];
    memcpy(body, req->data, req->data_len);
    body[req->data_len] = '\0';

    char *t = strstr(body, "\"timeout\"");
    if (t) {
      char *colon = strchr(t + 9, ':');
      if (colon) {
        int val = atoi(colon + 1);
        if (val > 0 && val <= 30) {
          timeout = val;
        }
      }
    }
  }

  int err = heater_ble_scan(timeout);
  if (err == -EALREADY) {
    return send_json_error(rsp, scan_resp, 400, "already scanning");
  } else if (err) {
    return send_json_error(rsp, scan_resp, 500, "scan failed");
  }

  int len = snprintf(scan_resp, sizeof(scan_resp),
                     "{\"ok\":true,\"timeout\":%d}", timeout);
  return send_json(rsp, scan_resp, len);
}

static struct http_resource_detail_dynamic api_ble_scan_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_scan,
};

HTTP_RESOURCE_DEFINE(api_ble_scan_res, damper_http_service,
                     "/api/ble/scan", &api_ble_scan_detail);

//////////////////////////////////////////////////////////////
// GET /api/ble/devices
//////////////////////////////////////////////////////////////

static char devices_buf[JSON_BUF_SIZE];

static int handle_api_ble_devices(struct http_client_ctx *client,
                                  enum http_data_status status,
                                  const struct http_request_ctx *req,
                                  struct http_response_ctx *rsp,
                                  void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  heater_ble_scan_stop();

  int count = heater_ble_get_scan_count();
  int off = snprintf(devices_buf, sizeof(devices_buf),
                     "{\"count\":%d,\"devices\":[", count);

  for (int i = 0; i < count && off < (int)sizeof(devices_buf) - 100; i++) {
    const struct ble_scan_result *r = heater_ble_get_scan_result(i);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&r->addr, addr_str, sizeof(addr_str));

    off += snprintf(devices_buf + off, sizeof(devices_buf) - off,
        "%s{\"index\":%d,\"addr\":\"%s\",\"name\":\"%s\","
        "\"rssi\":%d,\"protocol\":\"%s\"}",
        i > 0 ? "," : "", i, addr_str, r->name, r->rssi,
        r->protocol ? r->protocol->name : "unknown");
  }

  off += snprintf(devices_buf + off, sizeof(devices_buf) - off, "]}");
  return send_json(rsp, devices_buf, off);
}

static struct http_resource_detail_dynamic api_ble_devices_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_devices,
};

HTTP_RESOURCE_DEFINE(api_ble_devices_res, damper_http_service,
                     "/api/ble/devices", &api_ble_devices_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/connect
//////////////////////////////////////////////////////////////

static char connect_body[BODY_BUF_SIZE];
static char connect_resp[JSON_BUF_SIZE];

static int handle_api_ble_connect(struct http_client_ctx *client,
                                  enum http_data_status status,
                                  const struct http_request_ctx *req,
                                  struct http_response_ctx *rsp,
                                  void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, connect_resp, 400, "invalid body");
  }

  memcpy(connect_body, req->data, req->data_len);
  connect_body[req->data_len] = '\0';

  int index = -1;
  char *idx = strstr(connect_body, "\"index\"");
  if (idx) {
    char *colon = strchr(idx + 7, ':');
    if (colon) {
      index = atoi(colon + 1);
    }
  }

  if (index < 0) {
    return send_json_error(rsp, connect_resp, 400, "need index");
  }

  int err = heater_ble_connect(index);
  if (err == -EALREADY) {
    return send_json_error(rsp, connect_resp, 400, "already connected");
  } else if (err == -EINVAL) {
    return send_json_error(rsp, connect_resp, 400, "invalid index");
  } else if (err) {
    return send_json_error(rsp, connect_resp, 500, "connect failed");
  }

  int len = snprintf(connect_resp, sizeof(connect_resp),
                     "{\"ok\":true,\"index\":%d}", index);
  return send_json(rsp, connect_resp, len);
}

static struct http_resource_detail_dynamic api_ble_connect_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_connect,
};

HTTP_RESOURCE_DEFINE(api_ble_connect_res, damper_http_service,
                     "/api/ble/connect", &api_ble_connect_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/disconnect
//////////////////////////////////////////////////////////////

static char disconnect_resp[256];

static int handle_api_ble_disconnect(struct http_client_ctx *client,
                                     enum http_data_status status,
                                     const struct http_request_ctx *req,
                                     struct http_response_ctx *rsp,
                                     void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  int err = heater_ble_disconnect();
  if (err == -ENOTCONN) {
    return send_json_error(rsp, disconnect_resp, 400, "not connected");
  } else if (err) {
    return send_json_error(rsp, disconnect_resp, 500, "disconnect failed");
  }

  int len = snprintf(disconnect_resp, sizeof(disconnect_resp),
                     "{\"ok\":true}");
  return send_json(rsp, disconnect_resp, len);
}

static struct http_resource_detail_dynamic api_ble_disconnect_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_disconnect,
};

HTTP_RESOURCE_DEFINE(api_ble_disconnect_res, damper_http_service,
                     "/api/ble/disconnect", &api_ble_disconnect_detail);

//////////////////////////////////////////////////////////////
// POST /api/ble/protocol
//////////////////////////////////////////////////////////////

static char proto_body[BODY_BUF_SIZE];
static char proto_resp[256];

static int handle_api_ble_protocol(struct http_client_ctx *client,
                                   enum http_data_status status,
                                   const struct http_request_ctx *req,
                                   struct http_response_ctx *rsp,
                                   void *user_data)
{
  if (status != HTTP_SERVER_DATA_FINAL) {
    return 0;
  }

  if (req->data_len == 0 || req->data_len >= BODY_BUF_SIZE) {
    return send_json_error(rsp, proto_resp, 400, "invalid body");
  }

  memcpy(proto_body, req->data, req->data_len);
  proto_body[req->data_len] = '\0';

  if (strstr(proto_body, "\"byd\"")) {
    heater_ble_set_protocol(&heater_protocol_byd);
  } else if (strstr(proto_body, "\"cc\"")) {
    heater_ble_set_protocol(&heater_protocol_cc);
  } else if (strstr(proto_body, "\"auto\"")) {
    heater_ble_set_protocol(NULL);
  } else {
    return send_json_error(rsp, proto_resp, 400,
                           "protocol must be byd, cc, or auto");
  }

  const struct heater_protocol *p = heater_ble_get_protocol();
  int len = snprintf(proto_resp, sizeof(proto_resp),
                     "{\"ok\":true,\"protocol\":\"%s\"}",
                     p ? p->name : "auto");
  return send_json(rsp, proto_resp, len);
}

static struct http_resource_detail_dynamic api_ble_protocol_detail = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_DYNAMIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_POST),
        .content_type = "application/json",
    },
    .cb = handle_api_ble_protocol,
};

HTTP_RESOURCE_DEFINE(api_ble_protocol_res, damper_http_service,
                     "/api/ble/protocol", &api_ble_protocol_detail);

//////////////////////////////////////////////////////////////
// Static Web Resources
//////////////////////////////////////////////////////////////

#if HAS_INDEX_HTML
static uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static struct http_resource_detail_static index_resource = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_STATIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_encoding = "gzip",
        .content_type = "text/html",
    },
    .static_data = index_html_gz,
    .static_data_len = sizeof(index_html_gz),
};

HTTP_RESOURCE_DEFINE(index_res, damper_http_service, "/",
                     &index_resource);
#endif

#if HAS_APP_JS
static uint8_t app_js_gz[] = {
#include "app.js.gz.inc"
};

static struct http_resource_detail_static app_js_resource = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_STATIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_encoding = "gzip",
        .content_type = "application/javascript",
    },
    .static_data = app_js_gz,
    .static_data_len = sizeof(app_js_gz),
};

HTTP_RESOURCE_DEFINE(app_js_res, damper_http_service, "/app.js",
                     &app_js_resource);
#endif

#if HAS_APP_CSS
static uint8_t app_css_gz[] = {
#include "app.css.gz.inc"
};

static struct http_resource_detail_static app_css_resource = {
    .common = {
        .type = HTTP_RESOURCE_TYPE_STATIC,
        .bitmask_of_supported_http_methods = BIT(HTTP_GET),
        .content_encoding = "gzip",
        .content_type = "text/css",
    },
    .static_data = app_css_gz,
    .static_data_len = sizeof(app_css_gz),
};

HTTP_RESOURCE_DEFINE(app_css_res, damper_http_service, "/app.css",
                     &app_css_resource);
#endif

//////////////////////////////////////////////////////////////
// Server Lifecycle
//////////////////////////////////////////////////////////////

void http_api_start(void)
{
  if (server_running) {
    return;
  }

  int ret = http_server_start();
  if (ret < 0) {
    LOG_ERR("Failed to start HTTP server: %d", ret);
    return;
  }

  server_running = true;
  LOG_INF("HTTP server started on port %d", http_port);
}

void http_api_stop(void)
{
  if (!server_running) {
    return;
  }

  http_server_stop();
  server_running = false;
  LOG_INF("HTTP server stopped");
}
```

**Step 5: Build to verify**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

Expected: Build succeeds. No `web/` directory yet so HAS_INDEX_HTML=0 etc.

**Step 6: Commit**

```
feat(auto-damper): add HTTP server with JSON API endpoints
```

---

### Task 5: WiFi Shell Commands

Add `damper wifi save` and `damper wifi status` to the shell.

**Files:**
- Modify: `src/shell.c`

**Step 1: Add WiFi shell commands**

In `src/shell.c`, add includes at the top (after existing includes):

```c
#include <auto_damper/wifi.h>
#include <auto_damper/wifi_config.h>
```

Add the WiFi command handlers (before shell registration section):

```c
//////////////////////////////////////////////////////////////
// damper wifi save <ssid> <password>
//////////////////////////////////////////////////////////////

static int cmd_wifi_save(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 3) {
    shell_error(sh, "Usage: damper wifi save <ssid> <password>");
    return -EINVAL;
  }

  int rc = wifi_config_save(argv[1], argv[2]);
  if (rc) {
    shell_error(sh, "Failed to save: %d", rc);
    return rc;
  }

  shell_print(sh, "Credentials saved, connecting...");

  rc = wifi_connect(argv[1], argv[2]);
  if (rc) {
    shell_error(sh, "Connect failed: %d", rc);
  }
  return rc;
}

//////////////////////////////////////////////////////////////
// damper wifi status
//////////////////////////////////////////////////////////////

static int cmd_wifi_status(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  shell_print(sh, "WiFi Status:");
  shell_print(sh, "  Connected: %s", wifi_is_connected() ? "yes" : "no");

  if (wifi_is_connected()) {
    char addr[16];
    if (wifi_get_ip_address(addr, sizeof(addr)) == 0) {
      shell_print(sh, "  IP:        %s", addr);
    }
  }

  shell_print(sh, "  Stored:    %s",
              wifi_config_exists() ? "yes" : "no");
  return 0;
}
```

Add the WiFi subcmd set and update damper_cmds registration:

```c
SHELL_STATIC_SUBCMD_SET_CREATE(
    wifi_cmds,
    SHELL_CMD_ARG(save, NULL, "Save credentials: damper wifi save <ssid> <pw>",
                  cmd_wifi_save, 3, 0),
    SHELL_CMD(status, NULL, "Show WiFi status", cmd_wifi_status),
    SHELL_SUBCMD_SET_END);
```

Add the WiFi entry to the existing `damper_cmds` set (add before `SHELL_SUBCMD_SET_END`):

```c
SHELL_CMD(wifi, &wifi_cmds, "WiFi commands", NULL),
```

**Step 2: Build to verify**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

**Step 3: Commit**

```
feat(auto-damper): add WiFi shell commands
```

---

### Task 6: Placeholder Web Frontend

Create a minimal placeholder page to verify the full pipeline (CMake gzip → C array → HTTP server → browser). The real Voidable frontend will be built separately and its output dropped into `web/`.

**Files:**
- Create: `web/index.html`

**Step 1: Create placeholder frontend**

Create `web/index.html`:

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Auto-Damper</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: system-ui, sans-serif; background: #111; color: #eee;
         padding: 16px; max-width: 480px; margin: 0 auto; }
  h1 { font-size: 1.2em; margin-bottom: 12px; }
  .card { background: #1a1a1a; border-radius: 8px; padding: 12px;
          margin-bottom: 12px; }
  .label { color: #888; font-size: 0.8em; }
  .value { font-size: 1.4em; font-weight: bold; }
  .row { display: flex; justify-content: space-between; margin: 4px 0; }
  button { background: #333; color: #eee; border: none; padding: 8px 16px;
           border-radius: 4px; cursor: pointer; font-size: 0.9em; }
  button:active { background: #555; }
  .btn-row { display: flex; gap: 8px; margin-top: 8px; }
  #status { color: #888; font-size: 0.8em; margin-top: 8px; }
</style>
</head>
<body>
<h1>Auto-Damper</h1>

<div class="card" id="damper-card">
  <div class="row">
    <span class="label">Duct Temp</span>
    <span class="value" id="temp">--</span>
  </div>
  <div class="row">
    <span class="label">Route</span>
    <span class="value" id="route">--</span>
  </div>
  <div class="row">
    <span class="label">Mode</span>
    <span class="value" id="mode">--</span>
  </div>
</div>

<div class="btn-row">
  <button onclick="override('inside')">Inside</button>
  <button onclick="override('outside')">Outside</button>
  <button onclick="override('auto')">Auto</button>
</div>

<div id="status">Connecting...</div>

<script>
async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('temp').textContent = d.temperature.toFixed(1) + '°C';
    document.getElementById('route').textContent = d.route.toUpperCase();
    document.getElementById('mode').textContent = d.mode.toUpperCase();
    document.getElementById('status').textContent = 'Connected';
  } catch (e) {
    document.getElementById('status').textContent = 'Error: ' + e.message;
  }
}

async function override(mode) {
  try {
    await fetch('/api/override', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({mode})
    });
    fetchStatus();
  } catch (e) {
    document.getElementById('status').textContent = 'Error: ' + e.message;
  }
}

fetchStatus();
setInterval(fetchStatus, 2000);
</script>
</body>
</html>
```

**Step 2: Build to verify**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

Expected: Build succeeds. Build output should show `HAS_INDEX_HTML=1`. Check with:

```bash
grep "HAS_INDEX_HTML" build/CMakeCache.txt
```

**Step 3: Commit**

```
feat(auto-damper): add placeholder web frontend
```

---

### Task 7: Flash + End-to-End Test

**Step 1: Build clean**

```bash
west build -b esp32_devkitc/esp32/procpu -p always
```

**Step 2: Flash** (user executes)

```bash
west flash --esp-device /dev/ttyUSB0
```

**Step 3: Connect to WiFi via shell**

```
damper wifi save <your_ssid> <your_password>
```

Expected: "Credentials saved, connecting..." followed by log messages showing WiFi connection and IP address assignment.

**Step 4: Test JSON API with curl**

```bash
# Get damper status
curl http://<ip>/api/status

# Override to inside
curl -X POST http://<ip>/api/override -d '{"mode":"inside"}'

# Set temp threshold
curl -X POST http://<ip>/api/config -d '{"param":"temp_high","value":85.0}'

# BLE scan
curl -X POST http://<ip>/api/ble/scan -d '{"timeout":5}'

# Get scan results (after timeout)
curl http://<ip>/api/ble/devices

# BLE status
curl http://<ip>/api/ble/status
```

**Step 5: Test web frontend**

Open `http://<ip>/` in a phone browser. Should show temperature, route, mode, and override buttons.

**Step 6: Commit**

```
feat(auto-damper): WiFi + JSON API + web frontend complete
```

---

## API Reference

| Method | Endpoint | Body | Response |
|--------|----------|------|----------|
| GET | `/api/status` | — | `{temperature, route, mode, servo_degrees, config: {...}}` |
| POST | `/api/config` | `{param, value}` | `{ok, param, value}` |
| POST | `/api/override` | `{mode}` | `{ok, mode}` |
| GET | `/api/ble/status` | — | `{connected, protocol, scanning, telemetry?: {...}}` |
| POST | `/api/ble/scan` | `{timeout?}` | `{ok, timeout}` |
| GET | `/api/ble/devices` | — | `{count, devices: [{index, addr, name, rssi, protocol}]}` |
| POST | `/api/ble/connect` | `{index}` | `{ok, index}` |
| POST | `/api/ble/disconnect` | — | `{ok}` |
| POST | `/api/ble/protocol` | `{protocol}` | `{ok, protocol}` |
| GET | `/` | — | Embedded HTML frontend (gzip) |

## Notes

- The built-in Zephyr `wifi` shell (`CONFIG_NET_L2_WIFI_SHELL`) is also available for debugging: `wifi connect -s "ssid" -p "psk" -k 1`
- mDNS advertises `auto-damper-XXXX.local` where XXXX is derived from the ESP32's hardware ID
- HTTP server starts automatically when WiFi gets a DHCP address, stops on disconnect
- All JSON response buffers are static (no heap allocation in HTTP handlers)
- Web frontend at `web/` is gzipped at compile time using Zephyr's `generate_inc_file_for_target` CMake function — same pattern as Bucket
- The real Voidable frontend will replace the placeholder in `web/` once built
