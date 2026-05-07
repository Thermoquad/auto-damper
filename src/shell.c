// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/damper.h>
#include <auto_damper/zbus.h>
#include <zephyr/bluetooth/addr.h>
#include <auto_damper/heater.h>

extern int heater_ble_init(void);
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
// damper ble scan [timeout_sec]
//////////////////////////////////////////////////////////////

static int cmd_ble_scan(const struct shell *sh, size_t argc, char **argv)
{
  int timeout = 5;

  if (argc >= 2) {
    timeout = atoi(argv[1]);
    if (timeout <= 0 || timeout > 30) {
      timeout = 5;
    }
  }

  int err = heater_ble_scan(timeout);

  if (err == -EALREADY) {
    shell_warn(sh, "Already scanning");
  } else if (err) {
    shell_error(sh, "Scan failed: %d", err);
  } else {
    shell_print(sh, "Scanning for %d seconds...", timeout);
  }
  return err;
}

//////////////////////////////////////////////////////////////
// damper ble stop
//////////////////////////////////////////////////////////////

static int cmd_ble_stop(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  heater_ble_scan_stop();
  shell_print(sh, "Scan stopped, %d device(s) found",
              heater_ble_get_scan_count());

  for (int i = 0; i < heater_ble_get_scan_count(); i++) {
    const struct ble_scan_result *r = heater_ble_get_scan_result(i);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&r->addr, addr_str, sizeof(addr_str));
    shell_print(sh, "  [%d] %s \"%s\" RSSI %d (%s)", i, addr_str, r->name,
                r->rssi, r->protocol ? r->protocol->name : "unknown");
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// damper ble connect <index>
//////////////////////////////////////////////////////////////

static int cmd_ble_connect(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 2) {
    shell_error(sh, "Usage: damper ble connect <index>");
    return -EINVAL;
  }

  int index = atoi(argv[1]);
  int err = heater_ble_connect(index);

  if (err == -EALREADY) {
    shell_warn(sh, "Already connected");
  } else if (err == -EINVAL) {
    shell_error(sh, "Invalid index (0-%d)", heater_ble_get_scan_count() - 1);
  } else if (err) {
    shell_error(sh, "Connect failed: %d", err);
  } else {
    shell_print(sh, "Connecting...");
  }
  return err;
}

//////////////////////////////////////////////////////////////
// damper ble disconnect
//////////////////////////////////////////////////////////////

static int cmd_ble_disconnect(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  int err = heater_ble_disconnect();

  if (err == -ENOTCONN) {
    shell_warn(sh, "Not connected");
  } else if (err) {
    shell_error(sh, "Disconnect failed: %d", err);
  } else {
    shell_print(sh, "Disconnecting...");
  }
  return err;
}

//////////////////////////////////////////////////////////////
// damper ble protocol <byd|cc|auto>
//////////////////////////////////////////////////////////////

static int cmd_ble_protocol(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 2) {
    const struct heater_protocol *p = heater_ble_get_protocol();
    shell_print(sh, "Protocol: %s", p ? p->name : "none");
    shell_print(sh, "Usage: damper ble protocol <byd|cc|auto>");
    return 0;
  }

  if (strcmp(argv[1], "byd") == 0) {
    heater_ble_set_protocol(&heater_protocol_byd);
    shell_print(sh, "Protocol forced: byd");
  } else if (strcmp(argv[1], "cc") == 0) {
    heater_ble_set_protocol(&heater_protocol_cc);
    shell_print(sh, "Protocol forced: cc");
  } else if (strcmp(argv[1], "auto") == 0) {
    heater_ble_set_protocol(NULL);
    shell_print(sh, "Protocol: auto-detect");
  } else {
    shell_error(sh, "Unknown protocol: %s (use byd, cc, or auto)", argv[1]);
    return -EINVAL;
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// damper ble status
//////////////////////////////////////////////////////////////

static int cmd_ble_status(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  const struct heater_protocol *p = heater_ble_get_protocol();

  shell_print(sh, "BLE Status:");
  shell_print(sh, "  Connected:    %s",
              heater_ble_is_connected() ? "yes" : "no");
  shell_print(sh, "  Protocol:     %s", p ? p->name : "none");
  shell_print(sh, "  Scanning:     %s",
              heater_ble_is_scanning() ? "yes" : "no");

  if (!heater_ble_is_connected()) {
    return 0;
  }

  struct heater_data hdata;
  int ret = zbus_chan_read(&heater_data_chan, &hdata, K_MSEC(100));

  if (ret) {
    shell_warn(sh, "  No telemetry data");
    return 0;
  }

  shell_print(sh, "Heater Telemetry:");
  shell_print(sh, "  Power:        %s",
              heater_power_state_str(hdata.power));
  shell_print(sh, "  Step:         %s",
              heater_run_step_str(hdata.step));
  shell_print(sh, "  Exhaust:      %.1f C", hdata.exhaust_temp_c);
  shell_print(sh, "  Ambient:      %.1f C", hdata.ambient_temp_c);
  shell_print(sh, "  Voltage:      %.1f V", hdata.voltage);
  shell_print(sh, "  Target temp:  %d C", hdata.target_temp);
  shell_print(sh, "  Gear:         %d", hdata.gear_level);
  shell_print(sh, "  Error:        %d", hdata.error_code);
  return 0;
}

//////////////////////////////////////////////////////////////
// Shell Registration
//////////////////////////////////////////////////////////////

SHELL_STATIC_SUBCMD_SET_CREATE(
    ble_cmds,
    SHELL_CMD_ARG(scan, NULL, "Scan for heaters [timeout_sec]", cmd_ble_scan,
                  1, 1),
    SHELL_CMD(stop, NULL, "Stop scanning", cmd_ble_stop),
    SHELL_CMD_ARG(connect, NULL, "Connect: damper ble connect <index>",
                  cmd_ble_connect, 2, 0),
    SHELL_CMD(disconnect, NULL, "Disconnect from heater", cmd_ble_disconnect),
    SHELL_CMD_ARG(protocol, NULL, "Set protocol: byd, cc, or auto",
                  cmd_ble_protocol, 1, 1),
    SHELL_CMD(status, NULL, "Show heater BLE status and telemetry",
              cmd_ble_status),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    damper_cmds,
    SHELL_CMD(status, NULL, "Show damper status and config", cmd_status),
    SHELL_CMD(set, NULL, "Set config: damper set <param> <value>", cmd_set),
    SHELL_CMD(override, NULL, "Override: damper override <inside|outside|auto>",
              cmd_override),
    SHELL_CMD(ble, &ble_cmds, "BLE heater commands", NULL),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(damper, &damper_cmds, "Damper control commands", NULL);
