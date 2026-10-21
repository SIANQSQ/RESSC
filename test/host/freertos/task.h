#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
inline bool failTaskCreation = false;
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, unsigned, void*,
                                        UBaseType_t, TaskHandle_t *out, BaseType_t) {
  if (failTaskCreation) return pdFALSE;
  *out = reinterpret_cast<void*>(1); return pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}
inline void vTaskDelay(TickType_t) {}
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 1; }
inline void xTaskNotifyGive(TaskHandle_t) {}
