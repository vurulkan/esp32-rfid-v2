#pragma once
#include <Arduino.h>
struct HostAtomic {
  bool active = true;
  HostAtomic() { ++host::atomic_depth; }
  ~HostAtomic() { --host::atomic_depth; }
  operator bool() const { return active; }
  void next() { active = false; }
};
#define ATOMIC_RESTORESTATE 0
#define ATOMIC_BLOCK(mode) for (HostAtomic atomic_guard; atomic_guard; atomic_guard.next())
