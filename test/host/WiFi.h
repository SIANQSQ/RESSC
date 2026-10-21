#pragma once
#include "WString.h"
constexpr int WIFI_STA = 1, WL_CONNECTED = 3;
struct MockIP { String toString() const { return "192.168.1.100"; } };
struct MockWiFi {
  bool connected = true;
  int status() const { return connected ? WL_CONNECTED : 0; }
  void mode(int) {}
  void setHostname(const char*) {}
  void setAutoReconnect(bool) {}
  void begin(const char*, const char*) {}
  void reconnect() {}
  MockIP localIP() const { return {}; }
};
inline MockWiFi WiFi;
