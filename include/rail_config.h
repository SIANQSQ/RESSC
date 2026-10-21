#pragma once

// 2 mm lead screw, 100 mm travel, driver set to 6400 pulses/revolution.
// Place every slider at its physical zero BEFORE powering on. No limit inputs.
// mmPerRev is slider travel per MOTOR revolution (include any gearbox).
// ===== Driver subdivision settings: edit these values when changing DIP switches. =====
// Enter the driver's pulses/revolution (e.g. 1600/3200/6400/12800), NOT the
// microstep multiplier (e.g. 8/16/32). Software cannot change the driver's switches.
// For a 1.8-degree motor: pulses/rev = 200 * microstep multiplier.
constexpr uint32_t MOTOR1_PULSES_PER_REV = 1600;
constexpr uint32_t MOTOR2_PULSES_PER_REV = 1600;
constexpr uint32_t MOTOR3_PULSES_PER_REV = 1600;
constexpr uint32_t MOTOR4_PULSES_PER_REV = 1600;

// Distance conversion always uses these settings: pulses = mm / mmPerRev * PPR.
// With a 2 mm lead: 1 cm = 5 revolutions = 5 * PPR pulses.
struct RailConfig {
  double lengthMm, mmPerRev;
  uint32_t pulsesPerRev;
  bool positiveDirLevel;
};
constexpr RailConfig RAILS[4] = {
  {100, 2, MOTOR1_PULSES_PER_REV, HIGH},
  {100, 2, MOTOR2_PULSES_PER_REV, HIGH},
  {100, 2, MOTOR3_PULSES_PER_REV, HIGH},
  {100, 2, MOTOR4_PULSES_PER_REV, HIGH},
};
constexpr bool SELF_TEST_ON_BOOT = true;
constexpr double SELF_TEST_RPM = 50;
constexpr double SELF_TEST_DISTANCE_MM = 10; // Forward 10 mm, then return to zero.
constexpr uint32_t SELF_TEST_TIMEOUT_US = 120000000; // Per-leg timeout, not motion duration.
