#pragma once

// WiFi TCP console for ESP32 repeaters.
//
// Enabled with -D WIFI_CLI_ENABLED=1. WiFi stays completely off until
// credentials are set and enabled from an existing admin channel (USB serial,
// or remote management over LoRa):
//
//   wifi.ssid <network name>
//   wifi.pwd <passphrase>
//   wifi.on            (persists; reconnects on every boot)
//   wifi.off           (persists; radio off, power saving allowed again)
//   wifi.status
//
// Credentials live in NVS (not in the MeshCore prefs file), so the stock
// prefs/identity layout is untouched and stock firmware can be flashed back
// at any time.
//
// Once connected: `nc <ip> 23` (or telnet). The first line sent must be the
// repeater's admin password; after that, lines are regular CLI commands.

#if defined(WIFI_CLI_ENABLED) && defined(ESP32)

#include <Arduino.h>

#ifndef WIFI_CLI_TCP_PORT
  #define WIFI_CLI_TCP_PORT 23
#endif
#ifndef WIFI_CLI_BANNER
  #define WIFI_CLI_BANNER "MeshCore Repeater CLI"
#endif
#ifndef WIFI_CLI_IDLE_TIMEOUT_MS
  #define WIFI_CLI_IDLE_TIMEOUT_MS   (5 * 60 * 1000UL)
#endif
#ifndef WIFI_CLI_AUTH_TIMEOUT_MS
  #define WIFI_CLI_AUTH_TIMEOUT_MS   (30 * 1000UL)
#endif
#ifndef WIFI_CLI_MAX_AUTH_FAILS
  #define WIFI_CLI_MAX_AUTH_FAILS    3
#endif
#ifndef WIFI_CLI_LOCKOUT_MS
  #define WIFI_CLI_LOCKOUT_MS        (60 * 1000UL)
#endif

class WifiCLI {
public:
  // Load stored settings; connects if WiFi was left enabled.
  void begin();

  // Call from loop(): reconnects, accepts clients, enforces timeouts.
  void loop();

  // Handles the wifi.* commands. Returns true if `command` was consumed.
  // Safe to call from any admin channel (serial, LoRa remote CLI, TCP).
  bool handleCommand(const char* command, char* reply);

  // Reads from the TCP client into `buf` (which must persist between calls
  // and start out empty). Returns true when an authenticated, complete
  // command line is ready. The caller processes it, then sets buf[0] = 0.
  bool readLine(char* buf, size_t buf_size, const char* admin_password);

  // Sends a CLI reply to the TCP client.
  void sendReply(const char* reply);

  bool isEnabled() const { return _enabled; }

private:
  void start();
  void stop();
  void dropClient();
  void save();

  char _ssid[33] = {0};
  char _pwd[65] = {0};
  bool _enabled = false;
  bool _server_started = false;
  bool _authed = false;
  uint8_t _auth_fails = 0;
  uint8_t _iac_skip = 0;
  unsigned long _lockout_until = 0;
  unsigned long _last_activity = 0;
  unsigned long _last_reconnect = 0;
};

extern WifiCLI wifi_cli;

#endif
