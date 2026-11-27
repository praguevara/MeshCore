#include "WifiCLI.h"

#if defined(WIFI_CLI_ENABLED) && defined(ESP32)

#if defined(WITH_ESPNOW_BRIDGE)
  #error "WIFI_CLI_ENABLED cannot be combined with WITH_ESPNOW_BRIDGE (both need the WiFi radio)"
#endif

#include <WiFi.h>
#include <Preferences.h>
#include <target.h>

#define WIFI_CLI_NVS_NAMESPACE   "wificli"
#define WIFI_CLI_RECONNECT_MS    30000UL

WifiCLI wifi_cli;

static WiFiServer server(WIFI_CLI_TCP_PORT);
static WiFiClient client;

// Constant-time compare, so the reply timing doesn't leak how much of the password matched.
static bool secureEquals(const char* a, const char* b) {
  size_t la = strlen(a), lb = strlen(b);
  uint8_t diff = (uint8_t)(la ^ lb);
  for (size_t i = 0; i < la; i++) {
    diff |= (uint8_t)(a[i] ^ b[lb ? i % lb : 0]);
  }
  return diff == 0 && la == lb;
}

static const char* skipSpaces(const char* s) {
  while (*s == ' ') s++;
  return s;
}

void WifiCLI::begin() {
  Preferences nvs;
  if (nvs.begin(WIFI_CLI_NVS_NAMESPACE, true)) {   // read-only
    nvs.getString("ssid", _ssid, sizeof(_ssid));
    nvs.getString("pwd", _pwd, sizeof(_pwd));
    _enabled = nvs.getBool("on", false);
    nvs.end();
  }
  if (_enabled && _ssid[0]) {
    start();
  } else {
    _enabled = false;
  }
}

void WifiCLI::save() {
  Preferences nvs;
  if (nvs.begin(WIFI_CLI_NVS_NAMESPACE, false)) {
    nvs.putString("ssid", _ssid);
    nvs.putString("pwd", _pwd);
    nvs.putBool("on", _enabled);
    nvs.end();
  }
}

void WifiCLI::start() {
  board.setInhibitSleep(true);   // light sleep would drop the WiFi association
  WiFi.persistent(false);        // credentials are kept in our own NVS namespace
  WiFi.mode((WiFi.getMode() & WIFI_AP) ? WIFI_AP_STA : WIFI_STA);   // keep an OTA soft-AP alive, if any
  WiFi.setHostname("meshcore-repeater");
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  WiFi.begin(_ssid, _pwd);
  _last_reconnect = millis();
}

void WifiCLI::stop() {
  dropClient();
  if (_server_started) {
    server.end();
    _server_started = false;
  }
  if (WiFi.getMode() & WIFI_AP) {
    WiFi.disconnect(false);      // OTA soft-AP is up: only leave the station network
    WiFi.mode(WIFI_AP);
  } else {
    WiFi.disconnect(true);       // radio off
    WiFi.mode(WIFI_OFF);
    board.setInhibitSleep(false);
  }
}

void WifiCLI::dropClient() {
  if (client) client.stop();
  _authed = false;
  _iac_skip = 0;
}

bool WifiCLI::handleCommand(const char* command, char* reply) {
  if (memcmp(command, "wifi.", 5) != 0) return false;
  const char* sub = command + 5;

  if (strcmp(sub, "status") == 0) {
    if (!_enabled) {
      sprintf(reply, "WiFi: off (ssid: %s)", _ssid[0] ? _ssid : "-");
    } else if (WiFi.status() == WL_CONNECTED) {
      sprintf(reply, "WiFi: %s %s:%d rssi %d", _ssid, WiFi.localIP().toString().c_str(),
              WIFI_CLI_TCP_PORT, (int)WiFi.RSSI());
    } else {
      sprintf(reply, "WiFi: connecting to %s (status %d)", _ssid, (int)WiFi.status());
    }
  } else if (memcmp(sub, "ssid", 4) == 0 && (sub[4] == ' ' || sub[4] == 0)) {
    const char* val = skipSpaces(sub + 4);
    if (*val == 0) {
      sprintf(reply, "> %s", _ssid[0] ? _ssid : "-");
    } else if (strlen(val) >= sizeof(_ssid)) {
      strcpy(reply, "Err - ssid too long");
    } else {
      strcpy(_ssid, val);
      save();
      strcpy(reply, _enabled ? "OK - 'wifi.on' to apply" : "OK");
    }
  } else if (memcmp(sub, "pwd", 3) == 0 && (sub[3] == ' ' || sub[3] == 0)) {
    const char* val = skipSpaces(sub + 3);   // the passphrase is never echoed back
    if (strlen(val) >= sizeof(_pwd)) {
      strcpy(reply, "Err - pwd too long");
    } else {
      strcpy(_pwd, val);                     // empty = open network
      save();
      strcpy(reply, _enabled ? "OK - 'wifi.on' to apply" : "OK");
    }
  } else if (strcmp(sub, "on") == 0) {
    if (_ssid[0] == 0) {
      strcpy(reply, "Err - set wifi.ssid first");
    } else {
      if (_server_started) { dropClient(); server.end(); _server_started = false; }
      _enabled = true;
      save();
      start();
      strcpy(reply, "OK - connecting, see wifi.status");
    }
  } else if (strcmp(sub, "off") == 0) {
    _enabled = false;
    save();
    stop();
    strcpy(reply, "OK - WiFi off");
  } else {
    strcpy(reply, "Err - wifi.status|ssid|pwd|on|off");
  }
  return true;
}

void WifiCLI::loop() {
  if (!_enabled) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (client) dropClient();
    // auto-reconnect normally handles this; kick it if it has given up
    if (millis() - _last_reconnect >= WIFI_CLI_RECONNECT_MS) {
      _last_reconnect = millis();
      WiFi.disconnect();
      WiFi.begin(_ssid, _pwd);
    }
    return;
  }

  if (!_server_started) {
    server.begin();
    server.setNoDelay(true);
    _server_started = true;
    Serial.printf("WiFi CLI: listening on %s:%d\n", WiFi.localIP().toString().c_str(), WIFI_CLI_TCP_PORT);
  }

  WiFiClient incoming = server.available();   // on ESP32 this only returns newly-accepted sockets
  if (incoming) {
    if (_lockout_until && (long)(millis() - _lockout_until) < 0) {
      incoming.println("Locked out, try later");
      incoming.stop();
    } else {
      _lockout_until = 0;
      dropClient();                           // single session: newest connection wins
      client = incoming;
      _last_activity = millis();
      client.println(WIFI_CLI_BANNER);
      client.print("Password: ");
    }
  }

  if (client) {
    unsigned long limit = _authed ? WIFI_CLI_IDLE_TIMEOUT_MS : WIFI_CLI_AUTH_TIMEOUT_MS;
    if (!client.connected()) {
      dropClient();
    } else if (millis() - _last_activity >= limit) {
      client.println("Timeout");
      dropClient();
    }
  }
}

bool WifiCLI::readLine(char* buf, size_t buf_size, const char* admin_password) {
  if (!_enabled || !client || !client.connected()) {
    buf[0] = 0;   // discard any partial line from a dead session
    return false;
  }

  int len = strlen(buf);
  bool complete = false;
  while (client.available() && len < (int)buf_size - 1) {
    uint8_t c = client.read();
    if (_iac_skip) { _iac_skip--; continue; }       // rest of a telnet negotiation sequence
    if (c == 0xFF) { _iac_skip = 2; continue; }     // telnet IAC: ignore option negotiation
    if (c == '\n' && len == 0) continue;            // LF of a CR+LF pair, or an empty line
    if (c == '\r' || c == '\n') { complete = true; break; }
    if (c < ' ') continue;                          // drop other control chars
    buf[len++] = (char)c;
    buf[len] = 0;
  }
  if (len == (int)buf_size - 1) complete = true;    // buffer full: treat as a line
  if (!complete) return false;

  _last_activity = millis();

  if (!_authed) {
    // A blank admin password never grants access over the network.
    bool ok = admin_password[0] != 0 && secureEquals(buf, admin_password);
    memset(buf, 0, buf_size);                       // don't leave the attempt lying around
    if (ok) {
      _authed = true;
      _auth_fails = 0;
      client.println("OK");
    } else {
      delay(250);
      if (++_auth_fails >= WIFI_CLI_MAX_AUTH_FAILS) {
        _auth_fails = 0;
        _lockout_until = millis() + WIFI_CLI_LOCKOUT_MS;
        if (_lockout_until == 0) _lockout_until = 1;
        client.println("Denied - locked out");
        dropClient();
      } else {
        client.println("Denied");
        client.print("Password: ");
      }
    }
    return false;
  }

  if (strcmp(buf, "exit") == 0 || strcmp(buf, "quit") == 0) {
    client.println("Bye");
    dropClient();
    buf[0] = 0;
    return false;
  }
  return true;
}

void WifiCLI::sendReply(const char* reply) {
  if (!client || !_authed) return;
  if (reply[0]) {
    client.print("  -> ");
    client.println(reply);
  }
}

#endif
