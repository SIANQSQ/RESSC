#pragma once
#include "FreeRTOS.h"
#include <deque>
#include <vector>
#include <cstring>
struct MockQueue {
  UBaseType_t length;
  unsigned itemSize;
  std::deque<std::vector<unsigned char>> items;
};
using QueueHandle_t = MockQueue*;
inline bool failQueueCreation = false;
inline QueueHandle_t xQueueCreate(UBaseType_t count, unsigned size) {
  return failQueueCreation ? nullptr : new MockQueue{count, size, {}};
}
inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
  // Production motion path must use nonblocking queue operations.
  if (!q || wait != 0 || q->items.size() == q->length) return pdFALSE;
  const auto *p = static_cast<const unsigned char*>(item);
  q->items.emplace_back(p, p + q->itemSize);
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
  if (!q || wait != 0 || q->items.empty()) return pdFALSE;
  memcpy(item, q->items.front().data(), q->itemSize);
  q->items.pop_front(); return pdTRUE;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }

inline BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t wait) {
  if (!q || wait != 0 || q->items.empty()) return pdFALSE;
  memcpy(item, q->items.front().data(), q->itemSize); return pdTRUE;
}
inline BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item) {
  if (!q || q->length != 1) return pdFALSE;
  q->items.clear(); return xQueueSend(q, item, 0);
}
inline UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) {
  return q ? q->length - static_cast<UBaseType_t>(q->items.size()) : 0;
}
