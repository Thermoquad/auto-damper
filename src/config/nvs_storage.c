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
