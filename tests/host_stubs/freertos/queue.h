#pragma once
#include "FreeRTOS.h"
#include <vector>
#include <deque>
#include <cstring>
struct HostQueue {
  size_t limit, item;
  std::deque<std::vector<unsigned char>> entries;
};
using QueueHandle_t = HostQueue*;
struct HostQueueSet {
  std::vector<QueueHandle_t> queues;
};
using QueueSetHandle_t = HostQueueSet*;
using QueueSetMemberHandle_t = QueueHandle_t;
namespace host {
struct Stop {};
extern bool stop_empty;
}  // namespace host
inline QueueHandle_t xQueueCreate(size_t n, size_t size) { return new HostQueue{n, size, {}}; }
inline int xQueueSend(QueueHandle_t q, const void* item, TickType_t) {
  if (!q || q->entries.size() >= q->limit) return 0;
  auto p = static_cast<const unsigned char*>(item);
  q->entries.emplace_back(p, p + q->item);
  return 1;
}
inline int xQueueReceive(QueueHandle_t q, void* item, TickType_t) {
  if (!q || q->entries.empty()) {
    if (host::stop_empty) throw host::Stop{};
    return 0;
  }
  memcpy(item, q->entries.front().data(), q->item);
  q->entries.pop_front();
  return 1;
}
inline int xQueueOverwrite(QueueHandle_t q, const void* item) {
  q->entries.clear();
  return xQueueSend(q, item, 0);
}
inline size_t uxQueueMessagesWaiting(QueueHandle_t q) { return q->entries.size(); }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline QueueSetHandle_t xQueueCreateSet(size_t) { return new HostQueueSet; }
inline int xQueueAddToSet(QueueHandle_t q, QueueSetHandle_t set) {
  if (!q || !set || !q->entries.empty()) return 0;
  set->queues.push_back(q);
  return 1;
}
inline QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t set, TickType_t) {
  for (auto* q : set->queues)
    if (!q->entries.empty()) return q;
  if (host::stop_empty) throw host::Stop{};
  return nullptr;
}
