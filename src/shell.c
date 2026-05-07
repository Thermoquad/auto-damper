// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/damper.h>
#include <auto_damper/zbus.h>

#define PUB_TIMEOUT K_MSEC(100)

//////////////////////////////////////////////////////////////
// damper status
//////////////////////////////////////////////////////////////

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  struct damper_config *cfg = damper_config_get();

  struct temperature_data temp;
  zbus_chan_read(&temperature_data_chan, &temp, PUB_TIMEOUT);

  struct damper_data data;
  zbus_chan_read(&damper_data_chan, &data, PUB_TIMEOUT);

  shell_print(sh, "Damper Status:");
  shell_print(sh, "  Duct temp:     %.1f C", temp.celsius);
  shell_print(sh, "  Route:         %s",
              data.route == DAMPER_ROUTE_INSIDE ? "INSIDE" : "OUTSIDE");
  shell_print(sh, "  Mode:          %s",
              data.mode == DAMPER_MODE_AUTO ? "AUTO" : "OVERRIDE");
  shell_print(sh, "  Servo:         %.1f deg", data.servo_degrees);
  shell_print(sh, "Config:");
  shell_print(sh, "  temp_high:     %.1f C", cfg->temp_high);
  shell_print(sh, "  temp_low:      %.1f C", cfg->temp_low);
  shell_print(sh, "  servo_inside:  %.1f deg", cfg->servo_inside_deg);
  shell_print(sh, "  servo_outside: %.1f deg", cfg->servo_outside_deg);
  shell_print(sh, "  servo_min_us:  %u", cfg->servo_min_us);
  shell_print(sh, "  servo_max_us:  %u", cfg->servo_max_us);

  return 0;
}

//////////////////////////////////////////////////////////////
// damper set <param> <value>
//////////////////////////////////////////////////////////////

static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 3) {
    shell_error(sh, "Usage: damper set <param> <value>");
    return -EINVAL;
  }

  const char *param = argv[1];
  double value = strtod(argv[2], NULL);
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
    shell_error(sh, "Unknown param: %s", param);
    shell_print(sh, "Params: temp_high, temp_low, servo_inside, servo_outside");
    return -EINVAL;
  }

  cmd.value = value;
  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);
  shell_print(sh, "Set %s = %.1f", param, value);

  return 0;
}

//////////////////////////////////////////////////////////////
// damper override <inside|outside|auto>
//////////////////////////////////////////////////////////////

static int cmd_override(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 2) {
    shell_error(sh, "Usage: damper override <inside|outside|auto>");
    return -EINVAL;
  }

  struct damper_command cmd;

  if (strcmp(argv[1], "inside") == 0) {
    cmd.type = DAMPER_CMD_OVERRIDE_INSIDE;
  } else if (strcmp(argv[1], "outside") == 0) {
    cmd.type = DAMPER_CMD_OVERRIDE_OUTSIDE;
  } else if (strcmp(argv[1], "auto") == 0) {
    cmd.type = DAMPER_CMD_SET_AUTO;
  } else {
    shell_error(sh, "Unknown mode: %s (use inside, outside, or auto)", argv[1]);
    return -EINVAL;
  }

  cmd.value = 0.0;
  zbus_chan_pub(&damper_command_chan, &cmd, PUB_TIMEOUT);

  return 0;
}

//////////////////////////////////////////////////////////////
// Shell Registration
//////////////////////////////////////////////////////////////

SHELL_STATIC_SUBCMD_SET_CREATE(
    damper_cmds,
    SHELL_CMD(status, NULL, "Show damper status and config", cmd_status),
    SHELL_CMD(set, NULL, "Set config: damper set <param> <value>", cmd_set),
    SHELL_CMD(override, NULL, "Override: damper override <inside|outside|auto>",
              cmd_override),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(damper, &damper_cmds, "Damper control commands", NULL);
