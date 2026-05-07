// SPDX-License-Identifier: Apache-2.0

#include <string.h>
#include <zephyr/kernel.h>

#include <auto_damper/heater.h>

//////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////

static uint16_t passkey = 1234;

void heater_byd_set_passkey(uint16_t key)
{
  passkey = key;
}

//////////////////////////////////////////////////////////////
// Match
//////////////////////////////////////////////////////////////

static bool byd_match(const char *name)
{
  return strncmp(name, "BYD-", 4) == 0;
}

//////////////////////////////////////////////////////////////
// XOR Decryption (V2)
//////////////////////////////////////////////////////////////

static void xor_decrypt(uint8_t *buf, size_t len)
{
  static const uint8_t key[] = "password";

  for (size_t i = 0; i < len; i++) {
    buf[i] ^= key[i % 8];
  }
}

//////////////////////////////////////////////////////////////
// Decode
//////////////////////////////////////////////////////////////

static void decode_power(uint8_t val, struct heater_data *data)
{
  switch (val) {
  case 1:
    data->power = HEATER_POWER_STARTING;
    break;
  case 2:
    data->power = HEATER_POWER_RUNNING;
    break;
  case 3:
    data->power = HEATER_POWER_SHUTTING_DOWN;
    break;
  default:
    data->power = HEATER_POWER_OFF;
    break;
  }
}

static void decode_step(uint8_t val, struct heater_data *data)
{
  switch (val) {
  case 1:
    data->step = HEATER_STEP_SELF_CHECK;
    break;
  case 2:
    data->step = HEATER_STEP_PREHEAT;
    break;
  case 3:
    data->step = HEATER_STEP_HEATING;
    break;
  case 4:
    data->step = HEATER_STEP_COOLING;
    break;
  default:
    data->step = HEATER_STEP_IDLE;
    break;
  }
}

static int byd_decode(const uint8_t *buf, size_t len, struct heater_data *data)
{
  if (len < 2) {
    return -EINVAL;
  }

  uint8_t tmp[48];
  bool v2 = false;

  if (buf[0] == 0xAA && (buf[1] == 0x55 || buf[1] == 0x66)) {
    if (len < 18) {
      return -EINVAL;
    }
    memcpy(tmp, buf, MIN(len, sizeof(tmp)));
  } else {
    if (len > sizeof(tmp)) {
      return -EINVAL;
    }
    memcpy(tmp, buf, len);
    xor_decrypt(tmp, len);
    if (tmp[0] != 0xAA || (tmp[1] != 0x55 && tmp[1] != 0x66)) {
      return -EPROTO;
    }
    v2 = true;
  }

  decode_power(tmp[3], data);
  decode_step(tmp[5], data);

  if (!v2) {
    data->voltage = (double)((tmp[12] << 8) | tmp[11]) / 10.0;
    data->exhaust_temp_c = (double)(int16_t)((tmp[14] << 8) | tmp[13]);
    data->ambient_temp_c = (double)(int16_t)((tmp[16] << 8) | tmp[15]);
    data->error_code = (tmp[1] == 0x66) ? tmp[17] : tmp[4];
    data->target_temp = tmp[9];
    data->gear_level = tmp[10];
  } else if (len >= 36) {
    data->voltage = (double)((tmp[11] << 8) | tmp[12]) / 10.0;
    data->exhaust_temp_c = (double)(int16_t)((tmp[13] << 8) | tmp[14]);
    data->ambient_temp_c = (double)(int16_t)((tmp[32] << 8) | tmp[33]) / 10.0;
    data->error_code = (tmp[1] == 0x66) ? tmp[35] : tmp[4];
    data->target_temp = tmp[9];
    data->gear_level = tmp[10];
  }

  data->connected = true;
  return 0;
}

//////////////////////////////////////////////////////////////
// Encode Helpers
//////////////////////////////////////////////////////////////

static void byd_frame(uint8_t *buf, uint8_t cmd, uint8_t data_lo,
                      uint8_t data_hi)
{
  buf[0] = 0xAA;
  buf[1] = 0x55;
  buf[2] = passkey / 100;
  buf[3] = passkey % 100;
  buf[4] = cmd;
  buf[5] = data_lo;
  buf[6] = data_hi;
  buf[7] = (buf[2] + buf[3] + buf[4] + buf[5] + buf[6]) & 0xFF;
}

static int byd_encode_ping(uint8_t *buf, size_t len)
{
  if (len < 8) {
    return -ENOMEM;
  }
  byd_frame(buf, 0x01, 0x00, 0x00);
  return 8;
}

static int byd_encode_power(uint8_t *buf, size_t len, bool on)
{
  if (len < 8) {
    return -ENOMEM;
  }
  byd_frame(buf, 0x03, on ? 0x01 : 0x00, 0x00);
  return 8;
}

static int byd_encode_set_temp(uint8_t *buf, size_t len, int temp_c)
{
  if (len < 8) {
    return -ENOMEM;
  }
  byd_frame(buf, 0x04, temp_c & 0xFF, (temp_c >> 8) & 0xFF);
  return 8;
}

//////////////////////////////////////////////////////////////
// Protocol Definition
//////////////////////////////////////////////////////////////

const struct heater_protocol heater_protocol_byd = {
    .name = "byd",
    .match = byd_match,
    .service_uuid = 0xFFE0,
    .write_char_uuid = 0xFFE1,
    .notify_char_uuid = 0xFFE1,
    .heartbeat_ms = 2500,
    .decode = byd_decode,
    .encode_ping = byd_encode_ping,
    .encode_power = byd_encode_power,
    .encode_set_temp = byd_encode_set_temp,
};
