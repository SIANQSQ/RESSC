#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp32-hal-rgb-led.h>
#include <initializer_list>

WebServer httpServer(HTTP_PORT);
MotionSnapshot networkSnapshot;
bool snapshotAvailable = false;
struct RequestRecord {
  bool used = false, pending = false;
  CommandResult result = {};
};
constexpr size_t RESULT_HISTORY = 16;
RequestRecord history[RESULT_HISTORY];
uint32_t nextRequestId = 1;
size_t nextHistorySlot = 0;
TaskHandle_t indicatorTaskHandle = nullptr;

String jsonString(const char *value) {
  String out = "\"";
  for (const unsigned char *p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += static_cast<char>(*p); }
    else if (*p < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", *p); out += escaped; }
    else out += static_cast<char>(*p);
  }
  return out + "\"";
}
void sendJson(int code, const String &body) {
  httpServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  httpServer.sendHeader("Pragma", "no-cache");
  httpServer.send(code, "application/json; charset=utf-8", body);
}
void apiError(int code, const char *message) {
  sendJson(code, String("{\"ok\":false,\"message\":") + jsonString(message) + "}");
}
bool validArgs(std::initializer_list<const char*> allowed) {
  for (int i = 0; i < httpServer.args(); ++i) {
    const String name = httpServer.argName(i);
    bool found = false;
    for (const char *key : allowed) if (name == key) found = true;
    for (int j = 0; j < i; ++j) if (httpServer.argName(j) == name) found = false;
    if (!found) { apiError(400, "unknown or duplicate query parameter"); return false; }
  }
  return true;
}
bool numericArg(const char *key, double &out) {
  const String value = httpServer.arg(key);
  if (!httpServer.hasArg(key) || value.length() == 0 || value.length() > 40 ||
      strlen(value.c_str()) != value.length() || !number(value.c_str(), out)) {
    apiError(400, "missing or invalid finite numeric parameter"); return false;
  }
  return true;
}
bool motorArg(unsigned &motor, bool optional = false) {
  if (optional && !httpServer.hasArg("motor")) { motor = 0; return true; }
  double value;
  if (!numericArg("motor", value)) return false;
  if (value < 1 || value > 4 || floor(value) != value) { apiError(400, "motor must be 1..4"); return false; }
  motor = static_cast<unsigned>(value); return true;
}
void collectResults() {
  if (!resultQueue) return;
  CommandResult result;
  while (xQueueReceive(resultQueue, &result, 0) == pdTRUE)
    for (RequestRecord &record : history)
      if (record.used && record.result.id == result.id) { record.result = result; record.pending = false; break; }
}
void submitCommand(const char *text) {
  if (!runtimeReady) { apiError(503, "motor runtime unavailable"); return; }
  collectResults();
  RequestRecord *slot = nullptr;
  for (size_t i = 0; i < RESULT_HISTORY; ++i) {
    const size_t j = (nextHistorySlot + i) % RESULT_HISTORY;
    if (!history[j].pending) { slot = &history[j]; nextHistorySlot = (j + 1) % RESULT_HISTORY; break; }
  }
  if (!slot) { apiError(503, "too many outstanding requests"); return; }
  CommandLine request = {};
  request.id = nextRequestId++;
  if (!nextRequestId) nextRequestId = 1;
  snprintf(request.text, sizeof(request.text), "%s", text);
  if (xQueueSend(commandQueue, &request, 0) != pdTRUE) { apiError(503, "command queue full; request not accepted"); return; }
  *slot = {}; slot->used = true; slot->pending = true; slot->result.id = request.id;
  sendJson(202, String("{\"accepted\":true,\"id\":") + String(request.id) +
    ",\"result_url\":\"/api/result?id=" + String(request.id) + "\"}");
}
void handleMove() {
  if (!validArgs({"motor", "mm", "rpm"})) return;
  unsigned motor; double mm, rpm;
  if (!motorArg(motor) || !numericArg("mm", mm)) return;
  char cmd[192];
  if (httpServer.hasArg("rpm")) {
    if (!numericArg("rpm", rpm)) return;
    snprintf(cmd, sizeof(cmd), "M%u GOTO %.17g %.17g", motor, mm, rpm);
  } else snprintf(cmd, sizeof(cmd), "M%u GOTO %.17g", motor, mm);
  submitCommand(cmd);
}
void handleMoveAll() {
  if (!validArgs({"m1","m2","m3","m4","rpm"})) return;
  double values[4], rpm;
  const char *keys[] = {"m1","m2","m3","m4"};
  for (unsigned i = 0; i < 4; ++i) if (!numericArg(keys[i], values[i])) return;
  char cmd[192];
  int n = snprintf(cmd, sizeof(cmd), "GOTOALL %.17g %.17g %.17g %.17g",
    values[0], values[1], values[2], values[3]);
  if (httpServer.hasArg("rpm")) {
    if (!numericArg("rpm", rpm)) return;
    n += snprintf(cmd + n, sizeof(cmd) - n, " %.17g", rpm);
  }
  if (n < 0 || static_cast<size_t>(n) >= sizeof(cmd)) { apiError(400, "parameters too long"); return; }
  submitCommand(cmd);
}
void handleHome() {
  if (!validArgs({"motor"})) return;
  unsigned motor; if (!motorArg(motor, true)) return;
  char cmd[32];
  if (motor) snprintf(cmd, sizeof(cmd), "M%u HOME", motor); else snprintf(cmd, sizeof(cmd), "HOME");
  submitCommand(cmd);
}
void handleStop() {
  if (!validArgs({"motor"})) return;
  unsigned motor; if (!motorArg(motor, true)) return;
  char cmd[32];
  if (motor) snprintf(cmd, sizeof(cmd), "M%u STOP", motor); else snprintf(cmd, sizeof(cmd), "STOP");
  submitCommand(cmd);
}
void handleSet() {
  if (!validArgs({"motor", "param", "value"})) return;
  unsigned motor; double value;
  if (!motorArg(motor) || !numericArg("value", value)) return;
  const String param = httpServer.arg("param");
  const char *commandName = param == "ppr" ? "PPR" : param == "rpm" ? "RPM" : param == "accel" ? "ACCEL" : nullptr;
  if (!commandName) { apiError(400, "param must be ppr, rpm or accel"); return; }
  char cmd[96]; snprintf(cmd, sizeof(cmd), "M%u %s %.17g", motor, commandName, value); submitCommand(cmd);
}
void handleZero() {
  if (!validArgs({"motor"})) return;
  unsigned motor; if (!motorArg(motor)) return;
  char cmd[32]; snprintf(cmd, sizeof(cmd), "M%u ZERO", motor); submitCommand(cmd);
}
void handleResult() {
  if (!validArgs({"id"})) return;
  double id; if (!numericArg("id", id)) return;
  if (id < 1 || id > UINT32_MAX || floor(id) != id) { apiError(400, "invalid request id"); return; }
  collectResults();
  for (const RequestRecord &record : history) if (record.used && record.result.id == static_cast<uint32_t>(id)) {
    if (record.pending) { sendJson(202, "{\"pending\":true}"); return; }
    sendJson(record.result.code, String("{\"pending\":false,\"ok\":") +
      (record.result.code == 200 ? "true" : "false") + ",\"id\":" + String(record.result.id) +
      ",\"message\":" + jsonString(record.result.message) + "}"); return;
  }
  apiError(404, "request unknown or expired; history resets on reboot");
}
void refreshSnapshot() {
  if (snapshotQueue && xQueuePeek(snapshotQueue, &networkSnapshot, 0) == pdTRUE) snapshotAvailable = true;
}
void handleStatus() {
  if (!validArgs({})) return;
  refreshSnapshot();
  if (!runtimeReady || !snapshotAvailable) { apiError(503, "motor snapshot unavailable"); return; }
  const uint32_t age = millis() - networkSnapshot.sampledMs;
  if (age > 500) { apiError(503, "motor snapshot stale"); return; }
  String json = String("{\"ip\":") + jsonString(WiFi.localIP().toString().c_str()) +
    ",\"sample_age_ms\":" + String(age) + ",\"auto_active\":" + (networkSnapshot.bootActive ? "true" : "false") +
    ",\"self_test\":" + jsonString(selfTestName(networkSnapshot.selfTest)) +
    ",\"origin_mode\":\"manual_on_boot\",\"position_source\":\"commanded_steps\",\"last_event\":" + jsonString(networkSnapshot.lastEvent) + ",\"motors\":[";
  for (unsigned i = 0; i < MOTOR_COUNT; ++i) {
    const Motor &m = networkSnapshot.axes[i]; char axis[768];
    snprintf(axis, sizeof(axis),
      "%s{\"motor\":%u,\"moving\":%s,\"direction\":%d,\"ppr\":%lu,\"default_rpm\":%.6f,\"accel\":%.6f,\"completed\":%lu,\"total\":%lu,\"position\":%lld}",
      i ? "," : "", i+1, m.moving ? "true" : "false", m.direction,
      static_cast<unsigned long>(m.pulsesPerRev), m.defaultRpm, m.acceleration,
      static_cast<unsigned long>(m.completed), static_cast<unsigned long>(m.total), static_cast<long long>(m.position));
    // Reference is assumed from manual placement, without physical sensing.
    const bool homed = m.home == HomeState::Ready;
    String detail = String(",\"homed\":") + (homed ? "true" : "false") +
      ",\"home_state\":" + jsonString(homeName(m.home)) + ",\"fault\":" + jsonString(m.fault) +
      ",\"position_mm\":" + (homed ? String(static_cast<double>(m.position) * RAILS[i].mmPerRev / m.pulsesPerRev, 6) : String("null")) +
      ",\"target_mm\":" + (homed ? String(static_cast<double>(m.target) * RAILS[i].mmPerRev / m.pulsesPerRev, 6) : String("null")) +
      ",\"length_mm\":" + String(RAILS[i].lengthMm, 6) +
      ",\"mm_per_rev\":" + String(RAILS[i].mmPerRev, 6);
    // Remove closing brace and append the mm fields.
    axis[strlen(axis) - 1] = '\0';
    json += String(axis) + detail + "}";
  }
  sendJson(200, json + "]}");
}

struct LedColor { uint8_t r, g, b; };
LedColor indicatorColor(bool ready, bool connected, bool haveSnapshot, const MotionSnapshot &s, uint32_t now) {
  if (!ready) return {RGB_LEVEL,0,0};
  if (!haveSnapshot || static_cast<uint32_t>(now - s.sampledMs) > 500) return {RGB_LEVEL,0,0};
  if (!connected) return (now / 500) % 2 ? LedColor{RGB_LEVEL,RGB_LEVEL,0} : LedColor{0,0,0};
  bool moving = false;
  for (const Motor &m : s.axes) moving |= m.moving;
  for (const Motor &m : s.axes) if (m.home == HomeState::Fault) return {RGB_LEVEL,0,0};
  if (!moving) return s.bootActive ? LedColor{RGB_LEVEL,RGB_LEVEL/2,0} : LedColor{0,RGB_LEVEL,0};
  // Four 1600ms slots. White marker count identifies axis; then show its state.
  const unsigned axis = (now / 1600) % MOTOR_COUNT;
  const unsigned phase = now % 1600;
  if (phase < 700) {
    const bool on = phase / 160 < axis + 1 && phase % 160 < 80;
    return on ? LedColor{RGB_LEVEL,RGB_LEVEL,RGB_LEVEL} : LedColor{0,0,0};
  }
  if (phase >= 1500) return {0,0,0};
  const Motor &m = s.axes[axis];
  if (!m.moving) return {0,RGB_LEVEL,0};
  return m.direction > 0 ? LedColor{0,0,RGB_LEVEL} : LedColor{RGB_LEVEL,0,RGB_LEVEL};
}
void serviceIndicator() {
  if (STEPPER_RGB_PIN < 0) return;
  MotionSnapshot snapshot;
  const bool have = snapshotQueue && xQueuePeek(snapshotQueue, &snapshot, 0) == pdTRUE;
  const LedColor color = indicatorColor(runtimeReady, WiFi.status() == WL_CONNECTED, have, snapshot, millis());
  static LedColor last = {255,255,255};
  if (color.r != last.r || color.g != last.g || color.b != last.b) {
    neopixelWrite(STEPPER_RGB_PIN, color.r, color.g, color.b); last = color;
  }
}
void indicatorTask(void *) {
  for (;;) { serviceIndicator(); vTaskDelay(pdMS_TO_TICKS(20)); }
}
void beginNetwork() {
  httpServer.on("/", HTTP_GET, [] {
    sendJson(200, "{\"service\":\"esp32-stepper\",\"endpoints\":[\"/api/status\",\"/api/move\",\"/api/move_all\",\"/api/stop\",\"/api/set\",\"/api/zero\",\"/api/home\",\"/api/result\"]}");
  });
  httpServer.on("/api/status", HTTP_GET, handleStatus);
  httpServer.on("/api/move", HTTP_GET, handleMove);
  httpServer.on("/api/move_all", HTTP_GET, handleMoveAll);
  httpServer.on("/api/stop", HTTP_GET, handleStop);
  httpServer.on("/api/set", HTTP_GET, handleSet);
  httpServer.on("/api/zero", HTTP_GET, handleZero);
  httpServer.on("/api/home", HTTP_GET, handleHome);
  httpServer.on("/api/result", HTTP_GET, handleResult);
  httpServer.onNotFound([] { apiError(404, "unknown path or unsupported method; use GET"); });
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(STEPPER_HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (strlen(STEPPER_WIFI_SSID)) WiFi.begin(STEPPER_WIFI_SSID, STEPPER_WIFI_PASSWORD);
  // Indicator task stays responsive even if a slow HTTP client blocks handleClient.
  if (STEPPER_RGB_PIN >= 0)
    xTaskCreatePinnedToCore(indicatorTask, "rgb", 3072, nullptr, 1, &indicatorTaskHandle, 1);
}
void serviceNetwork() {
  static bool wasConnected = false;
  static uint32_t lastReconnectMs = 0;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    if (MDNS.begin(STEPPER_HOSTNAME)) MDNS.addService("http", "tcp", HTTP_PORT);
    httpServer.begin();
    Serial.print("HTTP IP: "); Serial.println(WiFi.localIP()); // Optional diagnostics only.
  } else if (!connected && wasConnected) { httpServer.stop(); MDNS.end(); }
  wasConnected = connected;
  if (!connected && strlen(STEPPER_WIFI_SSID) && static_cast<uint32_t>(millis() - lastReconnectMs) >= 15000) {
    lastReconnectMs = millis(); WiFi.reconnect();
  }
  if (runtimeReady) { collectResults(); refreshSnapshot(); }
  if (connected) httpServer.handleClient();
  if (!indicatorTaskHandle) serviceIndicator();
}
