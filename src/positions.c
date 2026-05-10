// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <auto_damper/config.h>
#include <auto_damper/positions.h>

LOG_MODULE_REGISTER(positions, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define NVS_ID_POSITION_BASE 0x0010
#define NVS_TYPE_POSITION    0x0010

//////////////////////////////////////////////////////////////
// NVS Data Layout
//////////////////////////////////////////////////////////////

struct position_nvs {
  char label[POSITION_LABEL_MAX + 1];
  double angle;
};

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

static struct position slots[POSITION_MAX_SLOTS];

//////////////////////////////////////////////////////////////
// NVS Helpers
//////////////////////////////////////////////////////////////

static int load_from_nvs(int id)
{
  struct position_nvs nvs_data;
  uint16_t version;

  int rc = config_load(NVS_ID_POSITION_BASE + id, NVS_TYPE_POSITION,
                       &version, &nvs_data, sizeof(nvs_data));
  if (rc < 0) {
    return rc;
  }

  slots[id].active = true;
  memcpy(slots[id].label, nvs_data.label, sizeof(nvs_data.label));
  slots[id].angle = nvs_data.angle;
  return 0;
}

static int save_to_nvs(int id)
{
  struct position_nvs nvs_data = {0};

  memcpy(nvs_data.label, slots[id].label, sizeof(nvs_data.label));
  nvs_data.angle = slots[id].angle;

  return config_save(NVS_ID_POSITION_BASE + id, NVS_TYPE_POSITION, 1,
                     &nvs_data, sizeof(nvs_data));
}

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int positions_init(void)
{
  for (int i = 0; i < POSITION_MAX_SLOTS; i++) {
    slots[i].active = false;
    if (config_exists(NVS_ID_POSITION_BASE + i) > 0) {
      load_from_nvs(i);
    }
  }

  int count = positions_count();
  if (count > 0) {
    LOG_INF("Loaded %d position(s) from NVS", count);
  }
  return 0;
}

const struct position *positions_get(int id)
{
  if (id < 0 || id >= POSITION_MAX_SLOTS) {
    return NULL;
  }
  if (!slots[id].active) {
    return NULL;
  }
  return &slots[id];
}

int positions_set(int id, const char *label, double angle)
{
  if (id < 0 || id >= POSITION_MAX_SLOTS) {
    return -EINVAL;
  }
  if (!label || strlen(label) == 0 || strlen(label) > POSITION_LABEL_MAX) {
    return -EINVAL;
  }

  slots[id].active = true;
  strncpy(slots[id].label, label, POSITION_LABEL_MAX);
  slots[id].label[POSITION_LABEL_MAX] = '\0';
  slots[id].angle = angle;

  int rc = save_to_nvs(id);
  if (rc < 0) {
    LOG_ERR("Failed to save position %d: %d", id, rc);
    return rc;
  }

  LOG_INF("Position %d: \"%s\" @ %.1f deg", id, label, angle);
  return 0;
}

int positions_delete(int id)
{
  if (id < 0 || id >= POSITION_MAX_SLOTS) {
    return -EINVAL;
  }
  if (!slots[id].active) {
    return -ENOENT;
  }

  slots[id].active = false;
  config_delete(NVS_ID_POSITION_BASE + id);
  LOG_INF("Position %d deleted", id);
  return 0;
}

int positions_count(void)
{
  int count = 0;
  for (int i = 0; i < POSITION_MAX_SLOTS; i++) {
    if (slots[i].active) {
      count++;
    }
  }
  return count;
}
