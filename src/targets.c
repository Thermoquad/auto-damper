// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <auto_damper/config.h>
#include <auto_damper/positions.h>
#include <auto_damper/targets.h>

LOG_MODULE_REGISTER(targets, LOG_LEVEL_INF);

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

#define NVS_ID_TARGET_BASE 0x0020
#define NVS_TYPE_TARGET    0x0020

//////////////////////////////////////////////////////////////
// NVS Data Layout
//////////////////////////////////////////////////////////////

struct target_nvs {
  double range_low;
  double range_high;
  int position_id;
};

//////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////

static struct target slots[TARGET_MAX_SLOTS];

//////////////////////////////////////////////////////////////
// NVS Helpers
//////////////////////////////////////////////////////////////

static int load_from_nvs(int id)
{
  struct target_nvs nvs_data;
  uint16_t version;

  int rc = config_load(NVS_ID_TARGET_BASE + id, NVS_TYPE_TARGET,
                       &version, &nvs_data, sizeof(nvs_data));
  if (rc < 0) {
    return rc;
  }

  slots[id].active = true;
  slots[id].range_low = nvs_data.range_low;
  slots[id].range_high = nvs_data.range_high;
  slots[id].position_id = nvs_data.position_id;
  return 0;
}

static int save_to_nvs(int id)
{
  struct target_nvs nvs_data = {
      .range_low = slots[id].range_low,
      .range_high = slots[id].range_high,
      .position_id = slots[id].position_id,
  };

  return config_save(NVS_ID_TARGET_BASE + id, NVS_TYPE_TARGET, 1,
                     &nvs_data, sizeof(nvs_data));
}

//////////////////////////////////////////////////////////////
// Overlap Check
//////////////////////////////////////////////////////////////

static bool ranges_overlap(double a_low, double a_high,
                           double b_low, double b_high)
{
  return a_low < b_high && b_low < a_high;
}

//////////////////////////////////////////////////////////////
// Public API
//////////////////////////////////////////////////////////////

int targets_init(void)
{
  for (int i = 0; i < TARGET_MAX_SLOTS; i++) {
    slots[i].active = false;
    if (config_exists(NVS_ID_TARGET_BASE + i) > 0) {
      load_from_nvs(i);
    }
  }

  int count = targets_count();
  if (count > 0) {
    LOG_INF("Loaded %d target(s) from NVS", count);
  }
  return 0;
}

const struct target *targets_get(int id)
{
  if (id < 0 || id >= TARGET_MAX_SLOTS) {
    return NULL;
  }
  if (!slots[id].active) {
    return NULL;
  }
  return &slots[id];
}

int targets_set(int id, double range_low, double range_high, int position_id)
{
  if (id < 0 || id >= TARGET_MAX_SLOTS) {
    return -EINVAL;
  }
  if (range_low >= range_high) {
    return -EINVAL;
  }
  if (!positions_get(position_id)) {
    return -ENOENT;
  }

  for (int i = 0; i < TARGET_MAX_SLOTS; i++) {
    if (i == id || !slots[i].active) {
      continue;
    }
    if (ranges_overlap(range_low, range_high,
                       slots[i].range_low, slots[i].range_high)) {
      return -EEXIST;
    }
  }

  slots[id].active = true;
  slots[id].range_low = range_low;
  slots[id].range_high = range_high;
  slots[id].position_id = position_id;

  int rc = save_to_nvs(id);
  if (rc < 0) {
    LOG_ERR("Failed to save target %d: %d", id, rc);
    return rc;
  }

  LOG_INF("Target %d: [%.1f, %.1f] -> position %d",
          id, range_low, range_high, position_id);
  return 0;
}

int targets_delete(int id)
{
  if (id < 0 || id >= TARGET_MAX_SLOTS) {
    return -EINVAL;
  }
  if (!slots[id].active) {
    return -ENOENT;
  }

  slots[id].active = false;
  config_delete(NVS_ID_TARGET_BASE + id);
  LOG_INF("Target %d deleted", id);
  return 0;
}

int targets_count(void)
{
  int count = 0;
  for (int i = 0; i < TARGET_MAX_SLOTS; i++) {
    if (slots[i].active) {
      count++;
    }
  }
  return count;
}

bool targets_position_referenced(int position_id)
{
  for (int i = 0; i < TARGET_MAX_SLOTS; i++) {
    if (slots[i].active && slots[i].position_id == position_id) {
      return true;
    }
  }
  return false;
}

const struct target *targets_find_by_temp(double temp_c)
{
  for (int i = 0; i < TARGET_MAX_SLOTS; i++) {
    if (!slots[i].active) {
      continue;
    }
    if (temp_c >= slots[i].range_low && temp_c < slots[i].range_high) {
      return &slots[i];
    }
  }
  return NULL;
}
