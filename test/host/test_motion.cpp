#include "Arduino.h"
#include <cassert>
#include <iostream>
#include <cstring>
uint32_t fakeUs = 0;
std::vector<Edge> edges;
int levels[64] = {};
MockSerial Serial;
#include "../../src/main.cpp"

void send(const char *text) {
  assert(!anyPulseHigh());
  char buffer[256]; std::strcpy(buffer, text);
  motionLog.begin(0); command(buffer); motionLog.capturing = false;
}
void tick(uint32_t duration) {
  for (uint32_t i = 0; i < duration; i += MOTOR_TICK_US) {
    fakeUs += MOTOR_TICK_US; serviceControl();
    if (i % 1000 == 0) loop();
  }
}
void reset(bool homed = true, uint32_t now = 0) {
  if (commandQueue) vQueueDelete(commandQueue);
  if (resultQueue) vQueueDelete(resultQueue);
  if (snapshotQueue) vQueueDelete(snapshotQueue);
  commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandLine));
  resultQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(CommandResult));
  snapshotQueue = xQueueCreate(1, sizeof(MotionSnapshot));
  motionLog = MotionLogger(); runtimeReady = true; selfTestState = SelfTestState::Disabled;
  snapshotAvailable = false; WiFi.connected = true;
  for (RequestRecord &r : history) r = {};
  nextRequestId = 1; nextHistorySlot = 0; httpServer.query.clear(); fakeUs = now;
  std::fill(levels, levels + 64, HIGH);
  std::fill(pinModes, pinModes + 64, -1);
  for (size_t i = 0; i < MOTOR_COUNT; ++i) {
    const uint8_t pul = motors[i].pulPin, dir = motors[i].dirPin;
    motors[i] = Motor(pul, dir); motors[i].pulsesPerRev = RAILS[i].pulsesPerRev;
    if (homed) motors[i].home = HomeState::Ready;
    levels[pul] = LOW;
  }
  edges.clear(); Serial = MockSerial();
}
void untilLow() { while (anyPulseHigh()) { fakeUs += MOTOR_TICK_US; serviceMotors(); } }
void rejected(const char *cmd) { send(cmd); assert(motionLog.response.code >= 400); }

void untilState(SelfTestState desired) {
  for (unsigned n = 0; n < 2400000 && selfTestState != desired; ++n) {
    fakeUs += MOTOR_TICK_US; serviceControl();
    if (n % 20 == 0) loop();
    for (const Motor &m : motors) assert(m.position >= 0 && m.position <= 32000);
    assert(selfTestState != SelfTestState::Failed);
  }
  assert(selfTestState == desired);
}

int main() {
  reset(); send("GOTOALL 0.4 0.8 0.2 0.6 15"); tick(5000000);
  const int64_t expected[] = {1280,2560,640,1920};
  for (size_t i = 0; i < MOTOR_COUNT; ++i) {
    const Motor &m = motors[i];
    assert(!m.moving && !m.pulseHigh && m.position == expected[i] && m.target == expected[i]);
    uint32_t started = 0, ended = 0; bool high = false, seen = false; unsigned count = 0;
    for (const Edge &e : edges) if (e.pin == m.pulPin) {
      if (e.level == HIGH) {
        assert(!high); if (seen) assert(e.at - ended >= MIN_LOW_US);
        high = true; started = e.at; ++count;
      } else {
        assert(high && e.at - started >= PULSE_WIDTH_US);
        high = false; seen = true; ended = e.at;
      }
    }
    assert(!high && count == m.total);
  }
  const auto edgeCount = edges.size(); send("GOTOALL 0.4 0.8 0.2 0.6 15");
  tick(100000); assert(edges.size() == edgeCount);
  send("GOTOALL 0 0 0 0 15"); tick(5000000);
  for (const Motor &m : motors) assert(m.position == 0 && !m.moving);
  reset(); rejected("GOTOALL 1 1 1 100.00001"); assert(!anyMoving());
  rejected("GOTO -0.00001"); rejected("GOTO nan"); rejected("GOTO 1 300"); rejected("GOTO 0 300");
  send("M3 GOTO 2"); rejected("GOTOALL 1 1 1 1");
  assert(!motors[0].moving && !motors[1].moving && motors[2].moving && !motors[3].moving);
  send("STOP");
  for (size_t i = 0; i < MOTOR_COUNT; ++i) motors[i].position = maxPosition(i) - 1;
  send("GOTOALL 100 100 100 100"); tick(1000000);
  for (size_t i = 0; i < MOTOR_COUNT; ++i) assert(motors[i].position == maxPosition(i));
  reset(); send("GOTO 0.00016"); tick(100000); assert(motors[0].position == 1);
  send("GOTO 0.00001"); tick(100000); assert(motors[0].position == 0);
  rejected("MOVE -1"); rejected("MOVEALL 1 30 1 30 1 30 100 30"); assert(!anyMoving());
  reset(); send("GOTOALL 10 10 10 10"); tick(100000); untilLow();
  send("M2 STOP"); auto stopped = motors[1].position;
  tick(100000); untilLow(); assert(motors[1].position == stopped && motors[0].position > stopped);
  send("M2 PPR 3200"); assert(motors[1].home == HomeState::Unhomed);
  rejected("M2 GOTO 1"); send("M2 ZERO"); assert(motors[1].home == HomeState::Ready); send("STOP");
  reset(true, 0xfffff000U); send("GOTOALL 0.01 0.01 0.01 0.01"); tick(1000000);
  for (const Motor &m : motors) assert(!m.moving && m.position == 32);

  // Changing driver pulses/rev must keep the same 1 cm target and feedback.
  for (const unsigned ppr : {3200u, 6400u, 12800u}) {
    reset();
    httpServer.query = {{"motor","2"},{"param","ppr"},{"value",String(ppr)}};
    handleSet(); serviceControl();
    assert(motors[1].pulsesPerRev == ppr && motors[1].home == HomeState::Unhomed);
    rejected("M2 GOTO 10 5");
    send("M2 ZERO"); send("M2 GOTO 10 5");
    assert(motors[1].total == 5 * ppr && motors[1].target == 5 * ppr);
    tick(70000000);
    assert(!motors[1].moving && motors[1].position == 5 * ppr);
    httpServer.query.clear(); handleStatus(); assert(httpServer.code == 200);
    assert(std::string(httpServer.body.c_str()).find("\"position_mm\":10.000000") != std::string::npos);
    assert(motors[0].pulsesPerRev == RAILS[0].pulsesPerRev && motors[0].position == 0);
  }

  // Startup is based on manual placement, with no switch inputs.
  assert(motors[2].pulPin == 9 && motors[2].dirPin == 10);
  assert(motors[3].pulPin == 11 && motors[3].dirPin == 12);
  reset(false); selfTestState = SelfTestState::Pending; serviceControl();
  assert(selfTestState == SelfTestState::Forward);
  rejected("GOTO 1"); rejected("M2 ZERO"); rejected("M2 PPR 3200");
  untilState(SelfTestState::Reverse);
  const uint32_t reverseEpoch = selfTestStartedUs;
  for (const Motor &m : motors) assert(m.position == 32000 && m.direction == -1 && m.target == 0);
  publishSnapshot(); httpServer.query.clear(); handleStatus();
  assert(std::string(httpServer.body.c_str()).find("\"self_test\":\"reverse\"") != std::string::npos);
  untilState(SelfTestState::Passed); assert(!autoActive());
  for (size_t i = 0; i < MOTOR_COUNT; ++i) {
    const Motor &m = motors[i];
    assert(m.home == HomeState::Ready && m.position == 0 && !m.moving);
    unsigned forward = 0, reverse = 0; uint32_t highAt = 0; bool high = false;
    for (const Edge &e : edges) if (e.pin == m.pulPin) {
      if (e.level == HIGH) {
        assert(!high); high = true; highAt = e.at;
        if (e.at < reverseEpoch) ++forward; else ++reverse;
      } else { assert(high && e.at - highAt >= PULSE_WIDTH_US); high = false; }
    }
    assert(!high && forward == 32000 && reverse == forward);
  }
  send("GOTOALL 1 1 1 1"); tick(5000000);
  for (const Motor &m : motors) assert(m.position == 3200);
  // Self-test STOP cancels all axes even when addressed to only one motor.
  reset(false); selfTestState = SelfTestState::Pending; tick(400000); untilLow();
  send("M2 STOP"); assert(selfTestState == SelfTestState::Cancelled && !anyMoving());
  const auto cancelledAt = motors[0].position;
  tick(3000000); assert(motors[0].position == cancelledAt && cancelledAt > 0);
  // A delayed reverse cannot silently declare the slider back at zero.
  reset(false); selfTestState = SelfTestState::Pending; untilState(SelfTestState::Reverse); tick(100000); untilLow();
  fakeUs += SELF_TEST_TIMEOUT_US; serviceControl();
  assert(selfTestState == SelfTestState::Failed && !anyMoving());
  for (const Motor &m : motors) assert(m.home == HomeState::Fault && m.position > 0);
  rejected("GOTO 1"); send("M1 ZERO"); assert(motors[0].home == HomeState::Ready && motors[0].position == 0);
  rejected("HOME");
  // The self-test clock and pulse deadlines tolerate micros() wraparound.
  reset(false, 0xffff0000U); selfTestState = SelfTestState::Pending; untilState(SelfTestState::Passed);
  assert(selfTestState == SelfTestState::Passed);
  for (const Motor &m : motors) assert(m.position == 0);
  reset(); send("GOTO 1"); motors[0].position = maxPosition(0); tick(100000);
  assert(motors[0].home == HomeState::Fault && motors[0].position == maxPosition(0));

  reset(); httpServer.query = {{"motor","2"},{"mm","0.8"},{"rpm","10"}};
  handleMove(); assert(httpServer.code == 202 && !motors[1].moving);
  assert(httpServer.cacheHeader == "no-store, no-cache, must-revalidate");
  httpServer.query = {{"id","1"}}; handleResult(); assert(httpServer.code == 202);
  serviceControl(); handleResult(); assert(httpServer.code == 200 && motors[1].target == 2560);
  httpServer.query = {{"motor","2"},{"mm","1"}}; handleMove(); serviceControl();
  httpServer.query = {{"id","2"}}; handleResult(); assert(httpServer.code == 409);
  const size_t queued = commandQueue->items.size();
  for (const char *bad : {"nan", "inf", "1 STOP"}) {
    httpServer.query = {{"motor","2"},{"mm",bad}}; handleMove(); assert(httpServer.code == 400);
  }
  httpServer.query = {{"motor","2"},{"mm",String(std::string("1\0STOP",6))}}; handleMove(); assert(httpServer.code == 400);
  httpServer.query = {{"motor","2"},{"mm","1"},{"motor","3"}}; handleMove(); assert(httpServer.code == 400);
  httpServer.query = {{"motor","2"}}; handleMove(); assert(httpServer.code == 400);
  httpServer.query = {{"motor","2"},{"turns","1"}}; handleMove(); assert(httpServer.code == 400);
  assert(commandQueue->items.size() == queued);
  reset(); httpServer.query = {{"m1","0.4"},{"m2","0.8"},{"m3","0.2"},{"m4","0.6"}};
  handleMoveAll(); assert(httpServer.code == 202); serviceControl();
  for (unsigned i = 0; i < 100000; ++i) { fakeUs += MOTOR_TICK_US; serviceControl(); }
  for (size_t i = 0; i < MOTOR_COUNT; ++i) assert(motors[i].position == expected[i]);
  httpServer.query.clear(); handleStatus(); assert(httpServer.code == 200);
  const std::string body = httpServer.body.c_str();
  assert(body.find("\"position_mm\":0.800000") != std::string::npos);
  assert(body.find("\"length_mm\":100.000000") != std::string::npos);
  assert(body.find("\"homed\":true") != std::string::npos);
  fakeUs += 600000; handleStatus(); assert(httpServer.code == 503);
  reset(false); publishSnapshot(); httpServer.query.clear(); handleStatus();
  assert(std::string(httpServer.body.c_str()).find("\"position_mm\":null") != std::string::npos);
  httpServer.query = {{"motor","2"}}; handleHome(); serviceControl(); assert(!anyMoving() && motionLog.response.code == 400);
  httpServer.query.clear(); handleStop(); serviceControl(); assert(!anyMoving());
  reset(); httpServer.query = {{"m1","1"},{"m2","1"},{"m3","1"},{"m4","101"}};
  handleMoveAll(); serviceControl(); httpServer.query = {{"id","1"}}; handleResult(); assert(httpServer.code == 400 && !anyMoving());
  reset(); httpServer.query = {{"motor","2"},{"mm","1"}};
  for (unsigned i = 0; i < COMMAND_QUEUE_LENGTH; ++i) handleMove();
  handleMove(); assert(httpServer.code == 503 && commandQueue->items.size() == COMMAND_QUEUE_LENGTH);
  for (unsigned i = 0; i < COMMAND_QUEUE_LENGTH; ++i) serviceControl();
  assert(resultQueue->items.size() == COMMAND_QUEUE_LENGTH);
  CommandLine request = {}; request.id = 100; strcpy(request.text,"STOP");
  xQueueSend(commandQueue,&request,0); serviceControl(); assert(commandQueue->items.size() == 1);
  collectResults(); serviceControl(); assert(!anyMoving());
  httpServer.query = {{"id","999"}}; handleResult(); assert(httpServer.code == 404);
  runtimeReady = false; httpServer.query.clear(); handleStop(); assert(httpServer.code == 503);
  // Setup configures only PUL/DIR outputs; no input ownership on GPIO12..15.
  reset(); vQueueDelete(commandQueue); vQueueDelete(resultQueue); vQueueDelete(snapshotQueue);
  commandQueue = resultQueue = snapshotQueue = nullptr;
  setup(); assert(runtimeReady && selfTestState == SelfTestState::Pending);
  assert(pinModes[12] == OUTPUT && pinModes[13] == -1 && pinModes[14] == -1 && pinModes[15] == -1);
  for (const Motor &m : motors) assert(m.home == HomeState::Ready && m.position == 0);
  for (unsigned failure = 0; failure < 4; ++failure) {
    reset(); vQueueDelete(commandQueue); vQueueDelete(resultQueue); vQueueDelete(snapshotQueue);
    commandQueue = nullptr; resultQueue = nullptr; snapshotQueue = nullptr;
    motorTaskHandle = nullptr; motorTimer = nullptr; runtimeReady = false;
    failQueueCreation = failure == 0; failTimerCreation = failure == 1;
    failTaskCreation = failure == 2; failTimerStart = failure == 3;
    setup();
    assert(!runtimeReady && !commandQueue && !resultQueue && !snapshotQueue && !motorTaskHandle && !motorTimer);
    for (const Motor &m : motors) assert(!m.moving && levels[m.pulPin] == LOW);
    failQueueCreation = failTimerCreation = failTaskCreation = failTimerStart = false;
  }
  std::cout << "PASS: mm positioning, manual origin + 10mm self-test, limits, GET/results/queues, snapshots, init failures\n";
}
