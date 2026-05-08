// SPDX-License-Identifier: Apache-2.0

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>

#include <auto_damper/damper.h>
#include <auto_damper/zbus.h>
#include <zephyr/bluetooth/addr.h>
#include <auto_damper/heater.h>
#include <auto_damper/wifi.h>
#include <auto_damper/wifi_config.h>
#include <whd_wifi_api.h>
#include <whd_types.h>
#include <whd_types_int.h>
#include <whd_int.h>
#include <whd_thread.h>

extern int heater_ble_init(void);
extern int heater_ble_scan(int timeout_sec);
extern int heater_ble_scan_stop(void);
extern int heater_ble_connect(int index);
extern int heater_ble_disconnect(void);
extern void heater_ble_set_protocol(const struct heater_protocol *proto);
extern const struct heater_protocol *heater_ble_get_protocol(void);
extern void cyw43_sbus_rx_set_paused(bool paused);
extern void cyw43_sbus_dump_whd_diag(void (*pr)(const char *fmt, ...) );
extern whd_interface_t airoc_wifi_get_whd_interface(void);
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
// damper ble init
//////////////////////////////////////////////////////////////

static int cmd_ble_init(const struct shell *sh, size_t argc, char **argv)
{
  int err = heater_ble_init();
  if (err) {
    shell_error(sh, "BT init failed: %d", err);
  } else {
    shell_print(sh, "BT initialized (no scan)");
  }
  return err;
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
  shell_print(sh, "  Mode:         %s",
              heater_run_mode_str(hdata.mode));
  shell_print(sh, "  Target temp:  %d C", hdata.target_temp);
  shell_print(sh, "  Power level:  %d", hdata.power_level);
  shell_print(sh, "  Error:        %d", hdata.error_code);
  return 0;
}

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

//////////////////////////////////////////////////////////////
// damper wifi diag
//////////////////////////////////////////////////////////////

static const struct shell *diag_sh;
static void diag_print(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  shell_vfprintf(diag_sh, SHELL_NORMAL, fmt, ap);
  shell_fprintf(diag_sh, SHELL_NORMAL, "\n");
  va_end(ap);
}

static int cmd_wifi_diag(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  shell_print(sh, "=== WHD Bus Diagnostics ===");
  diag_sh = sh;
  cyw43_sbus_dump_whd_diag(diag_print);
  return 0;
}

//////////////////////////////////////////////////////////////
// damper ble rx_pause <on|off>
//////////////////////////////////////////////////////////////

static int cmd_ble_rx_pause(const struct shell *sh, size_t argc, char **argv)
{
  if (argc != 2) {
    shell_error(sh, "Usage: damper ble rx_pause <on|off>");
    return -EINVAL;
  }

  if (strcmp(argv[1], "on") == 0) {
    cyw43_sbus_rx_set_paused(true);
    shell_print(sh, "BT RX thread paused (no bus access)");
  } else if (strcmp(argv[1], "off") == 0) {
    cyw43_sbus_rx_set_paused(false);
    shell_print(sh, "BT RX thread resumed");
  } else {
    shell_error(sh, "Usage: damper ble rx_pause <on|off>");
    return -EINVAL;
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// damper test wifi_ble
//////////////////////////////////////////////////////////////

static int cmd_test_wifi_ble(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  shell_print(sh, "=== WiFi+BLE Coexistence Test ===");

  /* Step 1: Check WiFi */
  shell_print(sh, "\n[1] WiFi status:");
  if (!wifi_is_connected()) {
    shell_error(sh, "  WiFi not connected — abort");
    return -ENODEV;
  }
  char addr[16];
  wifi_get_ip_address(addr, sizeof(addr));
  shell_print(sh, "  Connected, IP: %s", addr);

  /* Step 2: WHD state before BLE */
  shell_print(sh, "\n[2] WHD state BEFORE BLE:");
  diag_sh = sh;
  cyw43_sbus_dump_whd_diag(diag_print);

  /* Step 3: Ping gateway before BLE */
  shell_print(sh, "\n[3] Ping gateway BEFORE BLE:");
  shell_execute_cmd(sh, "net ping 192.168.8.1");
  k_sleep(K_SECONDS(5));

  /* Step 3.5: Disable power save before BLE */
  shell_print(sh, "\n[3.5] Disabling WiFi power save...");
  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (ifp) {
    whd_result_t pm_rc = whd_wifi_disable_powersave(ifp);
    shell_print(sh, "  PM disable: %s", pm_rc == WHD_SUCCESS ? "OK" : "FAIL");
    uint32_t pm_val = 0;
    whd_wifi_get_powersave_mode(ifp, &pm_val);
    shell_print(sh, "  PM mode now: %u", pm_val);
  }

  /* Step 4: BLE scan */
  shell_print(sh, "\n[4] Starting BLE scan (5s)...");
  int rc = heater_ble_scan(5);
  if (rc) {
    shell_error(sh, "  BLE scan failed: %d", rc);
  }
  k_sleep(K_SECONDS(6));

  /* Step 5: WHD state after BLE */
  shell_print(sh, "\n[5] WHD state AFTER BLE:");
  cyw43_sbus_dump_whd_diag(diag_print);

  /* Step 6: Ping gateway after BLE */
  shell_print(sh, "\n[6] Ping gateway AFTER BLE:");
  shell_execute_cmd(sh, "net ping 192.168.8.1");
  k_sleep(K_SECONDS(5));

  /* Step 7: Summary */
  shell_print(sh, "\n[7] WiFi status after BLE:");
  shell_print(sh, "  Connected: %s", wifi_is_connected() ? "yes" : "no");
  wifi_get_ip_address(addr, sizeof(addr));
  shell_print(sh, "  IP: %s", addr);

  /* Step 8: WHD state after failed ping */
  shell_print(sh, "\n[8] WHD state AFTER ping attempt:");
  cyw43_sbus_dump_whd_diag(diag_print);

  shell_print(sh, "\n=== Test complete ===");
  return 0;
}

//////////////////////////////////////////////////////////////
// damper wifi pm <off|pm1|pm2|get>
//////////////////////////////////////////////////////////////

static int cmd_wifi_pm(const struct shell *sh, size_t argc, char **argv)
{
  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (!ifp) {
    shell_error(sh, "WiFi interface not available");
    return -ENODEV;
  }

  if (argc < 2 || strcmp(argv[1], "get") == 0) {
    uint32_t pm_mode = 0;
    whd_result_t rc = whd_wifi_get_powersave_mode(ifp, &pm_mode);
    if (rc != WHD_SUCCESS) {
      shell_error(sh, "Failed to get PM mode: %d", (int)rc);
      return -EIO;
    }
    const char *name = (pm_mode == 0) ? "OFF" : (pm_mode == 1) ? "PM1" : "PM2";
    shell_print(sh, "Power save mode: %s (%u)", name, pm_mode);
    return 0;
  }

  whd_result_t rc;
  if (strcmp(argv[1], "off") == 0) {
    rc = whd_wifi_disable_powersave(ifp);
    shell_print(sh, "Disable powersave: %s", rc == WHD_SUCCESS ? "OK" : "FAIL");
  } else if (strcmp(argv[1], "pm1") == 0) {
    rc = whd_wifi_enable_powersave(ifp);
    shell_print(sh, "Enable PM1: %s", rc == WHD_SUCCESS ? "OK" : "FAIL");
  } else if (strcmp(argv[1], "pm2") == 0) {
    rc = whd_wifi_enable_powersave_with_throughput(ifp, 200);
    shell_print(sh, "Enable PM2 (200ms): %s", rc == WHD_SUCCESS ? "OK" : "FAIL");
  } else {
    shell_error(sh, "Usage: damper wifi pm <off|pm1|pm2|get>");
    return -EINVAL;
  }
  return 0;
}

//////////////////////////////////////////////////////////////
// damper wifi wake - manually wake WHD thread
//////////////////////////////////////////////////////////////

static int cmd_wifi_wake(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (!ifp || !ifp->whd_driver) {
    shell_error(sh, "WHD driver not available");
    return -ENODEV;
  }

  whd_driver_t drv = ifp->whd_driver;
  shell_print(sh, "WHD bus_interrupt was: %s",
              drv->thread_info.bus_interrupt ? "true" : "false");
  shell_print(sh, "Posting whd_thread_notify_irq...");
  whd_thread_notify_irq(drv);
  shell_print(sh, "Done. Check diag in ~100ms to see if thread processed.");
  return 0;
}

//////////////////////////////////////////////////////////////
// damper wifi recover - force WiFi leave and rejoin
//////////////////////////////////////////////////////////////

static int cmd_wifi_recover(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (!ifp) {
    shell_error(sh, "WHD interface not available");
    return -ENODEV;
  }

  whd_driver_t drv = ifp->whd_driver;

  /* Wake the WHD thread first so it can process the leave/join */
  shell_print(sh, "Waking WHD thread...");
  whd_thread_notify_irq(drv);
  k_sleep(K_MSEC(100));

  shell_print(sh, "Forcing WiFi leave...");
  whd_result_t rc = whd_wifi_leave(ifp);
  shell_print(sh, "  whd_wifi_leave: %s (rc=%u)",
              rc == WHD_SUCCESS ? "OK" : "FAIL", (unsigned)rc);

  /* Let the connection manager handle the rejoin */
  shell_print(sh, "Connection manager should detect link loss and reconnect.");
  shell_print(sh, "Check status in ~10 seconds.");
  return 0;
}

//////////////////////////////////////////////////////////////
// damper wifi poll - force WHD to polling mode
//////////////////////////////////////////////////////////////

static int cmd_wifi_poll(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (!ifp || !ifp->whd_driver) {
    shell_error(sh, "WHD driver not available");
    return -ENODEV;
  }

  whd_driver_t drv = ifp->whd_driver;
  whd_bt_dev_t btdev = drv->bt_dev;

  if (!btdev) {
    shell_print(sh, "No BT device attached, nothing to change");
    return 0;
  }

  shell_print(sh, "BT intr was: %s", btdev->intr ? "true" : "false");
  shell_print(sh, "Clearing BT intr flag to force WHD polling mode...");
  btdev->intr = false;
  shell_print(sh, "Done. WHD thread will now poll at BT_POLLING_TIME interval.");
  whd_thread_notify_irq(drv);
  return 0;
}

static int cmd_wifi_fwlog(const struct shell *sh, size_t argc, char **argv)
{
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (!ifp || !ifp->whd_driver) {
    shell_error(sh, "WHD driver not available");
    return -ENODEV;
  }

  char *buf = k_malloc(1024);
  if (!buf) {
    shell_error(sh, "Failed to allocate log buffer");
    return -ENOMEM;
  }

  whd_result_t ret = whd_wifi_read_wlan_log(ifp->whd_driver, buf, 1024);
  if (ret != WHD_SUCCESS) {
    shell_error(sh, "whd_wifi_read_wlan_log failed: %d", ret);
    k_free(buf);
    return -EIO;
  }

  buf[1023] = '\0';
  if (buf[0] == '\0') {
    shell_print(sh, "(firmware log empty)");
  } else {
    shell_print(sh, "=== FW Console ===");
    shell_print(sh, "%s", buf);
    shell_print(sh, "=== End ===");
  }

  k_free(buf);
  return 0;
}

static int cmd_wifi_btcmode(const struct shell *sh, size_t argc, char **argv)
{
  whd_interface_t ifp = airoc_wifi_get_whd_interface();
  if (!ifp || !ifp->whd_driver) {
    shell_error(sh, "WHD driver not available");
    return -ENODEV;
  }

  if (argc >= 2) {
    uint32_t mode = (uint32_t)atoi(argv[1]);
    whd_result_t ret = whd_wifi_set_iovar_value(ifp, "btc_mode", mode);
    if (ret != WHD_SUCCESS) {
      shell_error(sh, "Set btc_mode=%u failed: %d", mode, ret);
      return -EIO;
    }
    shell_print(sh, "btc_mode set to %u", mode);
  }

  uint32_t cur = 0xFFFFFFFF;
  whd_result_t ret = whd_wifi_get_iovar_value(ifp, "btc_mode", &cur);
  if (ret != WHD_SUCCESS) {
    shell_error(sh, "Get btc_mode failed: %d", ret);
    return -EIO;
  }
  shell_print(sh, "btc_mode = %u (0=off, 1=enable, 4=hybrid, 8=default)", cur);

  uint32_t flags = 0xFFFFFFFF;
  ret = whd_wifi_get_iovar_value(ifp, "btc_flags", &flags);
  if (ret == WHD_SUCCESS) {
    shell_print(sh, "btc_flags = 0x%08x", flags);
  }

  return 0;
}

//////////////////////////////////////////////////////////////
// Shell Registration
//////////////////////////////////////////////////////////////

SHELL_STATIC_SUBCMD_SET_CREATE(
    wifi_cmds,
    SHELL_CMD_ARG(save, NULL, "Save credentials: damper wifi save <ssid> <pw>",
                  cmd_wifi_save, 3, 0),
    SHELL_CMD(status, NULL, "Show WiFi status", cmd_wifi_status),
    SHELL_CMD(diag, NULL, "Dump WHD bus diagnostics", cmd_wifi_diag),
    SHELL_CMD_ARG(pm, NULL, "Power save: damper wifi pm <off|pm1|pm2|get>",
                  cmd_wifi_pm, 1, 1),
    SHELL_CMD(wake, NULL, "Manually wake WHD thread (debug)", cmd_wifi_wake),
    SHELL_CMD(recover, NULL, "Force WiFi leave + auto-rejoin", cmd_wifi_recover),
    SHELL_CMD(poll, NULL, "Force WHD polling mode (disable BT intr)", cmd_wifi_poll),
    SHELL_CMD(fwlog, NULL, "Read CYW43439 firmware console log", cmd_wifi_fwlog),
    SHELL_CMD_ARG(btcmode, NULL, "Get/set btc_mode: damper wifi btcmode [value]",
                  cmd_wifi_btcmode, 1, 1),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    ble_cmds,
    SHELL_CMD(init, NULL, "Init BT only (no scan)", cmd_ble_init),
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
    SHELL_CMD_ARG(rx_pause, NULL, "Pause BT RX: damper ble rx_pause <on|off>",
                  cmd_ble_rx_pause, 2, 0),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    damper_cmds,
    SHELL_CMD(status, NULL, "Show damper status and config", cmd_status),
    SHELL_CMD(set, NULL, "Set config: damper set <param> <value>", cmd_set),
    SHELL_CMD(override, NULL, "Override: damper override <inside|outside|auto>",
              cmd_override),
    SHELL_CMD(ble, &ble_cmds, "BLE heater commands", NULL),
    SHELL_CMD(wifi, &wifi_cmds, "WiFi commands", NULL),
    SHELL_CMD(test, NULL, "Run WiFi+BLE coex test", cmd_test_wifi_ble),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(damper, &damper_cmds, "Damper control commands", NULL);
