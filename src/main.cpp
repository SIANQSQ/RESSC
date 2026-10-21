#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include "device_config.h"
#include "rail_config.h"

// GPIO numbers, not header positions. Verify availability on the actual board.
constexpr size_t MOTOR_COUNT = 4;
constexpr bool FORWARD_LEVEL = HIGH;
constexpr uint32_t PULSE_WIDTH_US = 10;
constexpr uint32_t MIN_LOW_US = 10;
constexpr double MAX_HZ = 2000; // Per motor; software scheduling, target rate only.
constexpr uint32_t MAX_PULSES = 10000000;
enum class HomeState { Unhomed, Ready, Fault };
struct Motor {
  uint8_t pulPin, dirPin;
  uint32_t pulsesPerRev = 6400;
  double defaultRpm = 15, acceleration = 800;
  bool moving = false, pulseHigh = false, donePending = false;
  int direction = 1;
  uint32_t total = 0, completed = 0;
  uint32_t nextPulseUs = 0, pulseStartedUs = 0, cruisePeriodUs = 0;
  HomeState home = HomeState::Unhomed;
  const char *fault = "";
  int64_t target = 0;
  int64_t position = 0; // Commanded pulses, no encoder feedback.
  Motor(uint8_t pul, uint8_t dir) : pulPin(pul), dirPin(dir) {}
};
Motor motors[MOTOR_COUNT] = {{4, 5}, {6, 7}, {9, 10}, {11, 12}};
struct MovePlan {
  uint32_t total = 0, cruisePeriodUs = 0, firstDelayUs = 0;
  int direction = 1;
};
enum class SelfTestState { Pending, Forward, Reverse, Passed, Failed, Cancelled, Disabled };
SelfTestState selfTestState = SELF_TEST_ON_BOOT ? SelfTestState::Pending : SelfTestState::Disabled;
uint32_t selfTestStartedUs = 0;
bool autoActive() {
  return selfTestState == SelfTestState::Pending || selfTestState == SelfTestState::Forward ||
         selfTestState == SelfTestState::Reverse;
}
const char *selfTestName(SelfTestState state) {
  switch (state) {
    case SelfTestState::Pending: return "pending";
    case SelfTestState::Forward: return "forward";
    case SelfTestState::Reverse: return "reverse";
    case SelfTestState::Passed: return "passed";
    case SelfTestState::Failed: return "failed";
    case SelfTestState::Cancelled: return "cancelled";
    case SelfTestState::Disabled: return "disabled";
  }
  return "failed";
}
// The motor task alone owns all live state. Network uses queues of copied values.
constexpr uint32_t MOTOR_TICK_US = 50;
constexpr UBaseType_t COMMAND_QUEUE_LENGTH = 8;
struct CommandLine { uint32_t id; char text[192]; };
struct CommandResult { uint32_t id; int code; char message[192]; };
struct MotionSnapshot {
  Motor axes[MOTOR_COUNT] = {{4,5}, {6,7}, {9,10}, {11,12}};
  SelfTestState selfTest = SelfTestState::Pending;
  uint32_t sampledMs = 0;
  bool bootActive = false;
  char lastEvent[192] = {};
};
QueueHandle_t commandQueue = nullptr, resultQueue = nullptr, snapshotQueue = nullptr;
TaskHandle_t motorTaskHandle = nullptr;
esp_timer_handle_t motorTimer = nullptr;
bool runtimeReady = false; // setup + network task only.

// Capture the command outcome without performing I/O from the motor task.
struct MotionLogger {
  CommandResult response = {};
  bool capturing = false;
  char lastEvent[192] = {};
  void begin(uint32_t id) {
    response = {}; response.id = id; response.code = 200; capturing = true;
  }
  void println(const char *text) {
    snprintf(lastEvent, sizeof(lastEvent), "%s", text);
    if (capturing && !response.message[0]) {
      snprintf(response.message, sizeof(response.message), "%s", text);
      if (!strncmp(text, "ERR:", 4)) {
        response.code = (strstr(text, "busy") || strstr(text, "boot active") ||
                         strstr(text, "requires idle")) ? 409 : 400;
      }
    }
  }
  void printf(const char *format, ...) {
    char line[192];
    va_list ap; va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap); va_end(ap);
    const size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = '\0';
    println(line);
  }
} motionLog;

bool number(const char *s, double &v) {
  if (!s) return false;
  char *end;
  v = strtod(s, &end);
  return end != s && *end == '\0' && std::isfinite(v);
}
double pulsesPerMm(size_t i) { return motors[i].pulsesPerRev / RAILS[i].mmPerRev; }
int64_t maxPosition(size_t i) { return static_cast<int64_t>(std::floor(RAILS[i].lengthMm * pulsesPerMm(i))); }
const char *homeName(HomeState h) {
  switch (h) {
    case HomeState::Unhomed: return "unhomed";
    case HomeState::Ready: return "ready";
    case HomeState::Fault: return "fault";
  }
  return "fault";
}
void help() {
  motionLog.println("[M1..M4] GOTO <absolute mm> [rpm] | ZERO | STOP | STATUS");
  motionLog.println("GOTOALL <mm1> <mm2> <mm3> <mm4> [rpm]; ZERO records the manually placed origin.");
  motionLog.println("Boot: manual origin, all axes forward configured mm then return; software limits apply.");
}
bool planMove(const Motor &m, double turns, double rpm, MovePlan &plan) {
  const double count = std::fabs(turns) * m.pulsesPerRev;
  const double hz = rpm * m.pulsesPerRev / 60.0;
  if (!std::isfinite(turns) || !std::isfinite(rpm) || rpm < 1 || rpm > 300 ||
      hz > MAX_HZ || count < 0.5 || count > MAX_PULSES || !std::isfinite(count)) {
    motionLog.println("ERR: <0.5 pulse, >10000000 pulses, RPM outside 1..300, or >2000 Hz.");
    return false;
  }
  plan.total = static_cast<uint32_t>(std::llround(count));
  plan.direction = turns > 0 ? 1 : -1;
  plan.cruisePeriodUs = static_cast<uint32_t>(std::ceil(1000000.0 / hz));
  const double firstHz = std::min(hz, std::sqrt(2.0 * m.acceleration));
  plan.firstDelayUs = 100 + static_cast<uint32_t>(std::ceil(1000000.0 / firstHz));
  return true;
}
void armMove(Motor &m, const MovePlan &plan, uint32_t epochUs) {
  m.total = plan.total;
  m.completed = 0;
  m.direction = plan.direction;
  m.cruisePeriodUs = plan.cruisePeriodUs;
  m.donePending = false;
  const size_t index = &m - motors;
  digitalWrite(m.dirPin, m.direction > 0 ? RAILS[index].positiveDirLevel : !RAILS[index].positiveDirLevel);
  m.nextPulseUs = epochUs + plan.firstDelayUs;
  m.moving = true;
}
bool planTarget(size_t i, double mm, double rpm, MovePlan &plan) {
  const Motor &m = motors[i];
  if (m.moving || autoActive()) { motionLog.println("ERR: axis busy."); return false; }
  if (m.home != HomeState::Ready) { motionLog.println("ERR: axis requires manual origin; place at zero then ZERO."); return false; }
  if (!std::isfinite(mm) || mm < 0 || mm > RAILS[i].lengthMm) {
    motionLog.println("ERR: target outside rail range in mm."); return false;
  }
  if (!std::isfinite(rpm) || rpm < 1 || rpm > 300 || rpm * m.pulsesPerRev / 60 > MAX_HZ) {
    motionLog.println("ERR: invalid RPM or pulse frequency."); return false;
  }
  const int64_t target = std::min(maxPosition(i), static_cast<int64_t>(std::llround(mm * pulsesPerMm(i))));
  const int64_t delta = target - m.position;
  if (!delta) { plan = {}; return true; }
  return planMove(m, static_cast<double>(delta) / m.pulsesPerRev, rpm, plan);
}
void armTarget(size_t i, const MovePlan &plan, uint32_t epoch) {
  Motor &m = motors[i];
  m.target = m.position + static_cast<int64_t>(plan.direction) * plan.total;
  if (plan.total) armMove(m, plan, epoch);
}
void failAxis(size_t i, const char *reason) {
  Motor &m = motors[i];
  m.moving = m.donePending = false;
  m.home = HomeState::Fault; m.fault = reason; m.target = m.position;
  motionLog.printf("ERR: M%u %s; manual ZERO required", static_cast<unsigned>(i + 1), reason);
}
bool railConfigValid(size_t i) {
  const auto &r = RAILS[i];
  const double count = r.lengthMm * motors[i].pulsesPerRev / r.mmPerRev;
  return std::isfinite(r.lengthMm) && std::isfinite(r.mmPerRev) && r.lengthMm > 0 &&
    r.mmPerRev > 0 && motors[i].pulsesPerRev > 0 && std::isfinite(count) &&
    count >= 1 && count <= MAX_PULSES;
}
void manualZero(size_t i) {
  Motor &m = motors[i];
  if (!railConfigValid(i)) { failAxis(i, "invalid rail configuration"); return; }
  m.position = m.target = 0; m.home = HomeState::Ready; m.fault = "";
}
bool anyPulseHigh();
bool anyMoving();
void stopMotor(size_t index);
void failSelfTest(const char *reason) {
  selfTestState = SelfTestState::Failed;
  for (size_t i = 0; i < MOTOR_COUNT; ++i) { stopMotor(i); failAxis(i, reason); }
}
void serviceSelfTest() {
  if (!autoActive() || anyPulseHigh()) return;
  if (selfTestState == SelfTestState::Pending) {
    MovePlan plans[MOTOR_COUNT];
    // Validate every axis before starting any output.
    for (size_t i = 0; i < MOTOR_COUNT; ++i) {
      manualZero(i);
      if (motors[i].home != HomeState::Ready || !std::isfinite(SELF_TEST_DISTANCE_MM) ||
          SELF_TEST_DISTANCE_MM <= 0 || SELF_TEST_DISTANCE_MM > RAILS[i].lengthMm ||
          !planMove(motors[i], SELF_TEST_DISTANCE_MM / RAILS[i].mmPerRev, SELF_TEST_RPM, plans[i])) {
        failSelfTest("invalid self-test distance or speed"); return;
      }
      if (plans[i].total > maxPosition(i)) { failSelfTest("self-test exceeds rail travel"); return; }
    }
    selfTestState = SelfTestState::Forward; selfTestStartedUs = micros();
    for (size_t i = 0; i < MOTOR_COUNT; ++i) {
      armMove(motors[i], plans[i], selfTestStartedUs); motors[i].target = plans[i].total;
    }
    motionLog.printf("SELF TEST: all axes forward %.3f mm", SELF_TEST_DISTANCE_MM); return;
  }
  for (const Motor &m : motors) if (m.home != HomeState::Ready) {
    failSelfTest("self-test motion fault"); return;
  }
  if (static_cast<uint32_t>(micros() - selfTestStartedUs) >= SELF_TEST_TIMEOUT_US) {
    failSelfTest("self-test timed out; manually restore origin"); return;
  }
  if (anyMoving()) return;
  if (selfTestState == SelfTestState::Forward) {
    for (const Motor &m : motors) if (!m.completed) {
      failSelfTest("self-test forward produced no pulses"); return;
    }
    // Wait for all axes to reach the requested distance, then return by pulse count.
    selfTestState = SelfTestState::Reverse; selfTestStartedUs = micros();
    for (Motor &m : motors) {
      MovePlan plan;
      if (!planMove(m, -static_cast<double>(m.completed) / m.pulsesPerRev, SELF_TEST_RPM, plan)) {
        failSelfTest("invalid self-test return plan"); return;
      }
      m.moving = m.donePending = false; m.target = 0;
      if (plan.total) armMove(m, plan, selfTestStartedUs);
    }
    motionLog.println("SELF TEST: all axes return to 0 mm");
  } else {
    for (const Motor &m : motors) if (m.position != 0) {
      failSelfTest("self-test return incomplete; manually restore origin"); return;
    }
    selfTestState = SelfTestState::Passed;
    for (Motor &m : motors) m.moving = m.donePending = false;
    motionLog.println("SELF TEST DONE: commanded positions returned to 0 mm");
  }
}
// Each axis owns its rising/falling deadlines. No delay or wait for DONE.
// Command execution runs only with ALL pulse outputs LOW; HTTP runs in another task.
void serviceMotors() {
  for (Motor &m : motors) {
    const size_t i = &m - motors;
    if (!m.moving) continue;
    const uint32_t now = micros();
    if (m.pulseHigh) {
      if (static_cast<uint32_t>(now - m.pulseStartedUs) < PULSE_WIDTH_US) continue;
      digitalWrite(m.pulPin, LOW);
      m.pulseHigh = false;
      ++m.completed;
      m.position += m.direction;
      if (m.completed == m.total) {
        m.moving = false;
        m.donePending = true;
        continue;
      }
      const double rampHz = std::sqrt(2.0 * m.acceleration * std::min(m.completed + 1, m.total - m.completed));
      const uint32_t rampPeriod = static_cast<uint32_t>(std::ceil(1000000.0 / rampHz));
      const uint32_t period = std::max(m.cruisePeriodUs, rampPeriod);
      // Actual-time scheduling: late wakes never cause catch-up bursts.
      const uint32_t scheduleNow = micros();
      const uint32_t highTime = static_cast<uint32_t>(scheduleNow - m.pulseStartedUs);
      m.nextPulseUs = scheduleNow + (period > highTime ? std::max(MIN_LOW_US, period - highTime) : MIN_LOW_US);
    } else if (static_cast<int32_t>(now - m.nextPulseUs) >= 0) {
      if (m.home == HomeState::Ready) {
        const int64_t next = m.position + m.direction;
        if (next < 0 || next > maxPosition(i)) { failAxis(i, "soft travel limit"); continue; }
      }
      digitalWrite(m.pulPin, HIGH);
      m.pulseStartedUs = micros();
      m.pulseHigh = true;
    }
  }
}
bool anyPulseHigh() {
  for (const Motor &m : motors) if (m.pulseHigh) return true;
  return false;
}
bool anyMoving() {
  for (const Motor &m : motors) if (m.moving) return true;
  return false;
}
void reportDone() {
  for (size_t i = 0; i < MOTOR_COUNT; ++i) if (motors[i].donePending) {
    motors[i].donePending = false;
    motionLog.printf("M%u DONE\n", static_cast<unsigned>(i + 1));
  }
}
void stopMotor(size_t index) {
  Motor &m = motors[index];
  m.moving = false;
  m.donePending = false;
  m.pulseHigh = false;
  digitalWrite(m.pulPin, LOW);
  m.target = m.position;
}
void status(size_t index) {
  const Motor &m = motors[index];
  motionLog.printf("M%u %s PPR=%lu rpm=%.3f accel=%.1f done=%lu/%lu pos=%lld (%.6f rev)\n",
    static_cast<unsigned>(index + 1), m.moving ? "MOVING" : "IDLE",
    static_cast<unsigned long>(m.pulsesPerRev), m.defaultRpm, m.acceleration,
    static_cast<unsigned long>(m.completed), static_cast<unsigned long>(m.total),
    static_cast<long long>(m.position), static_cast<double>(m.position) / m.pulsesPerRev);
}
void command(char *line) {
  char *args[12];
  size_t argc = 0;
  char *save = nullptr;
  for (char *token = strtok_r(line, " \t", &save); token; token = strtok_r(nullptr, " \t", &save)) {
    if (argc == 12) { motionLog.println("ERR: too many arguments."); return; }
    args[argc++] = token;
  }
  if (!argc) return;
  auto upper = [](char *s) { for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s -= 'a' - 'A'; };
  upper(args[0]);
  bool prefixed = false;
  size_t index = 0, offset = 0;
  if (args[0][0] == 'M' && args[0][1] >= '1' && args[0][1] <= '4' && args[0][2] == '\0') {
    prefixed = true;
    index = args[0][1] - '1';
    offset = 1;
    if (argc == 1) { motionLog.println("ERR: missing command after M1..M4."); return; }
    upper(args[1]);
  }
  char *cmd = args[offset];
  const size_t n = argc - offset - 1;
  char **values = args + offset + 1;
  Motor &m = motors[index];
  if (autoActive() && strcmp(cmd, "STOP") && strcmp(cmd, "STATUS") && strcmp(cmd, "HELP")) {
    motionLog.println("ERR: boot active; self-test busy."); return;
  }
  if (!strcmp(cmd, "HOME")) {
    motionLog.println("ERR: no home switches; manually place at origin then use ZERO."); return;
  }
  if (!strcmp(cmd, "GOTO") || !strcmp(cmd, "GOTOALL")) {
    const bool group = !strcmp(cmd, "GOTOALL");
    const size_t count = group ? MOTOR_COUNT : 1;
    if ((group && prefixed) || (n != count && n != count + 1)) {
      motionLog.println("ERR: GOTO mm [rpm] or GOTOALL mm1 mm2 mm3 mm4 [rpm]"); return;
    }
    MovePlan plans[MOTOR_COUNT];
    double rpmOverride = 0;
    if (n == count + 1 && !number(values[count], rpmOverride)) { motionLog.println("ERR: invalid RPM."); return; }
    for (size_t j = 0; j < count; ++j) {
      const size_t i = group ? j : index; double mm;
      if (!number(values[j], mm)) { motionLog.println("ERR: invalid mm."); return; }
      if (!planTarget(i, mm, n == count + 1 ? rpmOverride : motors[i].defaultRpm, plans[j])) return;
    }
    const uint32_t epoch = micros();
    for (size_t j = 0; j < count; ++j) armTarget(group ? j : index, plans[j], epoch);
    motionLog.println("POSITION COMMAND STARTED"); return;
  }
  if (!strcmp(cmd, "MOVEALL")) {
    if (prefixed || n != 8) {
      motionLog.println("ERR: MOVEALL <turns1> <rpm1> <turns2> <rpm2> <turns3> <rpm3> <turns4> <rpm4>"); return;
    }
    MovePlan plans[MOTOR_COUNT];
    bool selected[MOTOR_COUNT] = {};
    // Validate the entire group before changing any output or state.
    for (size_t i = 0; i < MOTOR_COUNT; ++i) {
      double turns, rpm;
      if (!number(values[2*i], turns) || !number(values[2*i+1], rpm) || rpm < 1 || rpm > 300) {
        motionLog.println("ERR: finite turns and RPM 1..300 required for every axis."); return;
      }
      selected[i] = turns != 0;
      if (!selected[i]) continue;
      if (motors[i].moving || autoActive()) {
        motionLog.printf("ERR: M%u busy (motion or boot sequence); no axes started.\n", static_cast<unsigned>(i + 1)); return;
      }
      if (!planTarget(i, motors[i].position / pulsesPerMm(i) + turns * RAILS[i].mmPerRev, rpm, plans[i])) return;
    }
    motionLog.println("GROUP START (zero-turn axes skipped)");
    const uint32_t epoch = micros();
    for (size_t i = 0; i < MOTOR_COUNT; ++i) if (selected[i]) armTarget(i, plans[i], epoch);
    return;
  }
  if (!strcmp(cmd, "STOP")) {
    if (n) { motionLog.println("ERR: unexpected argument."); return; }
    if (autoActive()) {
      selfTestState = SelfTestState::Cancelled;
      for (size_t i = 0; i < MOTOR_COUNT; ++i) stopMotor(i);
    }
    if (prefixed) stopMotor(index);
    else for (size_t i = 0; i < MOTOR_COUNT; ++i) stopMotor(i);
    motionLog.println("STOPPED; position is commanded pulses only."); return;
  }
  if (!strcmp(cmd, "STATUS")) {
    if (n) { motionLog.println("ERR: unexpected argument."); return; }
    motionLog.printf("self_test_active=%d\n", autoActive());
    if (prefixed) status(index);
    else for (size_t i = 0; i < MOTOR_COUNT; ++i) status(i);
    return;
  }
  if (!strcmp(cmd, "HELP")) {
    if (prefixed || n) motionLog.println("ERR: use HELP without arguments.");
    else if (anyMoving()) motionLog.println("ERR: HELP requires all axes idle.");
    else help();
    return;
  }
  const bool move = !strcmp(cmd, "MOVE");
  const bool ppr = !strcmp(cmd, "PPR"), rpm = !strcmp(cmd, "RPM"), accel = !strcmp(cmd, "ACCEL");
  const bool zero = !strcmp(cmd, "ZERO");
  if (!move && !ppr && !rpm && !accel && !zero) { motionLog.println("ERR: unknown command; send HELP."); return; }
  if (m.moving) { motionLog.println("ERR: selected motor busy; STOP it or wait for its DONE."); return; }
  double x = 0, y = m.defaultRpm;
  if (move) {
    if (n < 1 || n > 2 || !number(values[0], x) || (n == 2 && !number(values[1], y))) {
      motionLog.println("ERR: [M1..M4] MOVE <signed turns> [rpm]"); return;
    }
    MovePlan plan;
    if (planTarget(index, m.position / pulsesPerMm(index) + x * RAILS[index].mmPerRev, y, plan))
      armTarget(index, plan, micros());
  } else if (zero) {
    if (n) { motionLog.println("ERR: ZERO takes no arguments."); return; }
    manualZero(index);
    if (m.home == HomeState::Ready) motionLog.println("OK: manually placed position recorded as 0 mm.");
  } else {
    if (n != 1 || !number(values[0], x)) { motionLog.println("ERR: one finite numeric argument required."); return; }
    if (ppr && x >= 1 && x <= 100000 && std::floor(x) == x) {
      m.pulsesPerRev = static_cast<uint32_t>(x);
      m.home = HomeState::Unhomed; m.fault = "PPR changed; manual ZERO required";
    } else if (rpm && x >= 1 && x <= 300) m.defaultRpm = x;
    else if (accel && x >= 10 && x <= 100000) m.acceleration = x;
    else { motionLog.println("ERR: setting outside allowed range."); return; }
    motionLog.println("OK (RAM only)");
  }
}
// One bounded iteration, called only by motorTask (also used by host tests).
void publishSnapshot() {
  MotionSnapshot snapshot;
  for (size_t i = 0; i < MOTOR_COUNT; ++i) {
    snapshot.axes[i] = motors[i];
  }
  snapshot.sampledMs = millis(); snapshot.bootActive = autoActive(); snapshot.selfTest = selfTestState;
  snprintf(snapshot.lastEvent, sizeof(snapshot.lastEvent), "%s", motionLog.lastEvent);
  xQueueOverwrite(snapshotQueue, &snapshot);
}
void serviceControl() {
  serviceSelfTest();
  serviceMotors();
  if (anyPulseHigh()) return;
  reportDone();
  CommandLine request;
  // Reserve a result slot before removing a request: results cannot be lost.
  if (uxQueueSpacesAvailable(resultQueue) && xQueueReceive(commandQueue, &request, 0) == pdTRUE) {
    motionLog.begin(request.id);
    command(request.text);
    motionLog.capturing = false;
    xQueueSend(resultQueue, &motionLog.response, 0); // Single producer, reserved slot.
  }
  // Publish a consistent copy, not a pointer into the live motor array.
  static uint32_t lastPublishedMs = 0;
  if (static_cast<uint32_t>(millis() - lastPublishedMs) >= 20) {
    lastPublishedMs = millis(); publishSnapshot();
  }
}
void motorTask(void *) {
  for (;;) {
    // Block between timer notifications so idle/system tasks can run.
    // Clear accumulated notifications: never replay missed ticks as pulse bursts.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    serviceControl();
  }
}
void motorTimerCallback(void *) {
  // ESP_TIMER_TASK dispatch: this is task context, NOT an ISR.
  xTaskNotifyGive(motorTaskHandle);
}
#include "network_control.h"

void setup() {
  for (size_t i = 0; i < MOTOR_COUNT; ++i) {
    Motor &m = motors[i];
    m.pulsesPerRev = RAILS[i].pulsesPerRev;
    manualZero(i);
    pinMode(m.pulPin, OUTPUT);
    digitalWrite(m.pulPin, LOW);
    pinMode(m.dirPin, OUTPUT);
    digitalWrite(m.dirPin, FORWARD_LEVEL);
  }
  Serial.begin(115200);
  commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandLine));
  resultQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandResult));
  snapshotQueue = xQueueCreate(1, sizeof(MotionSnapshot));
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = motorTimerCallback;
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "motor_tick";
  timerArgs.skip_unhandled_events = true;
  selfTestState = SELF_TEST_ON_BOOT ? SelfTestState::Pending : SelfTestState::Disabled;
  // Motor task on core 0; Arduino HTTP loop on its configured core (S3: core 1).
  // Priority 3 is above Arduino loop/idle and below system timer tasks.
  if (commandQueue && resultQueue && snapshotQueue && esp_timer_create(&timerArgs, &motorTimer) == ESP_OK &&
      xTaskCreatePinnedToCore(motorTask, "motors", 6144, nullptr, 3, &motorTaskHandle, 0) == pdPASS &&
      esp_timer_start_periodic(motorTimer, MOTOR_TICK_US) == ESP_OK) {
    runtimeReady = true;
    Serial.println("4 STEPPERS HTTP READY: configure Wi-Fi, use /api/status. Manual origin on boot; all-axis forward/reverse self-test; no limit switches.");
  } else {
    // No timer has successfully started; the task is blocked, all PUL remain LOW.
    if (motorTaskHandle) { vTaskDelete(motorTaskHandle); motorTaskHandle = nullptr; }
    if (motorTimer) { esp_timer_delete(motorTimer); motorTimer = nullptr; }
    if (commandQueue) { vQueueDelete(commandQueue); commandQueue = nullptr; }
    if (resultQueue) { vQueueDelete(resultQueue); resultQueue = nullptr; }
    if (snapshotQueue) { vQueueDelete(snapshotQueue); snapshotQueue = nullptr; }
    Serial.println("ERR: FreeRTOS initialization failed; motion disabled; reset to retry.");
  }
  beginNetwork();
}
void loop() {
  serviceNetwork();
  vTaskDelay(1); // Network task yields; motion continues independently.
}
