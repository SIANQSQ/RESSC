#pragma once
#include <cstdint>
constexpr int ESP_OK = 0, ESP_TIMER_TASK = 0;
using esp_timer_handle_t = void*;
struct esp_timer_create_args_t {
  void (*callback)(void*);
  void *arg;
  int dispatch_method;
  const char *name;
  bool skip_unhandled_events;
};
inline bool failTimerCreation = false, failTimerStart = false;
inline int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t *out) {
  if (failTimerCreation) return -1;
  *out = reinterpret_cast<void*>(1); return ESP_OK;
}
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return failTimerStart ? -1 : ESP_OK; }
inline int esp_timer_delete(esp_timer_handle_t) { return ESP_OK; }
