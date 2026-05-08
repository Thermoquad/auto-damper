// SPDX-License-Identifier: Apache-2.0

#include <string.h>
#include <zephyr/kernel.h>

#include <auto_damper/heater.h>

//////////////////////////////////////////////////////////////
// Match
//////////////////////////////////////////////////////////////

static bool cc_match(const char *name)
{
  return strstr(name, "Heater") != NULL;
}

//////////////////////////////////////////////////////////////
// Checksum
//////////////////////////////////////////////////////////////

static uint8_t cc_checksum(const uint8_t *buf, size_t len)
{
  uint8_t sum = 0;

  for (size_t i = 0; i < len; i++) {
    sum += buf[i];
  }
  return sum;
}

//////////////////////////////////////////////////////////////
// Decode
//////////////////////////////////////////////////////////////

static int cc_decode(const uint8_t *buf, size_t len, struct heater_data *data)
{
  if (len < 19) {
    return -EINVAL;
  }
  if (buf[0] != 0xAB || buf[1] != 0xBA) {
    return -EPROTO;
  }
  if (buf[3] != 0xCC) {
    return -EPROTO;
  }

  switch (buf[4]) {
  case 0x01:
    data->power = HEATER_POWER_RUNNING;
    data->step = HEATER_STEP_HEATING;
    break;
  case 0x02:
    data->power = HEATER_POWER_SHUTTING_DOWN;
    data->step = HEATER_STEP_COOLING;
    break;
  case 0x04:
    data->power = HEATER_POWER_RUNNING;
    data->step = HEATER_STEP_IDLE;
    break;
  case 0x06:
    data->power = HEATER_POWER_RUNNING;
    data->step = HEATER_STEP_IDLE;
    break;
  default:
    data->power = HEATER_POWER_OFF;
    data->step = HEATER_STEP_IDLE;
    break;
  }

  if (buf[5] == 0xFF) {
    data->error_code = buf[6];
    data->power_level = 0;
  } else {
    data->error_code = 0;
    data->power_level = buf[6];
  }

  data->target_temp = (buf[5] != 0xFF) ? buf[6] : 0;
  data->voltage = (double)buf[9];
  data->ambient_temp_c = (double)buf[11] - 30.0;
  data->exhaust_temp_c = (double)((buf[12] << 8) | buf[13]);
  data->connected = true;

  return 0;
}

//////////////////////////////////////////////////////////////
// Encode
//////////////////////////////////////////////////////////////

static int cc_encode_ping(uint8_t *buf, size_t len)
{
  if (len < 8) {
    return -ENOMEM;
  }
  uint8_t cmd[] = {0xBA, 0xAB, 0x04, 0xCC, 0x00, 0x00, 0x00};

  memcpy(buf, cmd, 7);
  buf[7] = cc_checksum(buf, 7);
  return 8;
}

static void cc_cmd(uint8_t *buf, uint8_t b3, uint8_t b4)
{
  buf[0] = 0xBA;
  buf[1] = 0xAB;
  buf[2] = 0x04;
  buf[3] = b3;
  buf[4] = b4;
  buf[5] = 0x00;
  buf[6] = 0x00;
  buf[7] = cc_checksum(buf, 7);
}

static int cc_encode_power(uint8_t *buf, size_t len, bool on)
{
  if (len < 8) {
    return -ENOMEM;
  }
  if (!on) {
    return -ENOTSUP;
  }

  cc_cmd(buf, 0xBB, 0xA1);
  return 8;
}

static int cc_encode_set_temp(uint8_t *buf, size_t len, int temp_c)
{
  (void)buf;
  (void)len;
  (void)temp_c;
  return -ENOTSUP;
}

static int cc_encode_set_mode(uint8_t *buf, size_t len,
                              enum heater_run_mode mode)
{
  if (len < 8) {
    return -ENOMEM;
  }

  switch (mode) {
  case HEATER_MODE_MANUAL:
    cc_cmd(buf, 0xBB, 0xA1);
    break;
  case HEATER_MODE_AUTOMATIC:
    cc_cmd(buf, 0xBB, 0xA5);
    break;
  case HEATER_MODE_FAN:
    cc_cmd(buf, 0xBB, 0xA4);
    break;
  default:
    return -EINVAL;
  }
  return 8;
}

//////////////////////////////////////////////////////////////
// Protocol Definition
//////////////////////////////////////////////////////////////

const struct heater_protocol heater_protocol_cc = {
    .name = "cc",
    .match = cc_match,
    .service_uuid = 0xFFF0,
    .write_char_uuid = 0xFFF2,
    .notify_char_uuid = 0,
    .heartbeat_ms = 2000,
    .decode = cc_decode,
    .encode_ping = cc_encode_ping,
    .encode_power = cc_encode_power,
    .encode_set_temp = cc_encode_set_temp,
    .encode_set_mode = cc_encode_set_mode,
};
