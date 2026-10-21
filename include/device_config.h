#pragma once

// Copy device_config.local.h.example to device_config.local.h for private values.
#if __has_include("device_config.local.h")
#include "device_config.local.h"
#endif
#ifndef STEPPER_WIFI_SSID
#define STEPPER_WIFI_SSID ""
#endif
#ifndef STEPPER_WIFI_PASSWORD
#define STEPPER_WIFI_PASSWORD ""
#endif
#ifndef STEPPER_HOSTNAME
#define STEPPER_HOSTNAME "truss-ctrl"
#endif
// Physical GPIO for a single WS2812-compatible GRB LED. Some S3 boards use 38.
// Set -1 to disable. Verify against the actual board schematic.
#ifndef STEPPER_RGB_PIN
#define STEPPER_RGB_PIN 48
#endif
// Motion and boot self-test settings are in rail_config.h.
constexpr uint16_t HTTP_PORT = 80;
constexpr uint8_t RGB_LEVEL = 16;
