#pragma once
// Host stand-in for Arduino.h: just enough for SecureHttpClient.h.
#include <cstddef>
#include <cstdint>

class IPAddress {};

// Virtual clock: every call advances 1 ms and delay() adds its argument, so
// the client's timeout loops terminate quickly without real sleeping.
inline unsigned long& hostClockMs() {
  static unsigned long now = 1000;
  return now;
}
inline unsigned long millis() { return ++hostClockMs(); }
inline void delay(unsigned long ms) { hostClockMs() += ms; }
