#pragma once

// Vendored from MeshCore/examples/simple_repeater/RateLimiter.h — small enough
// to keep header-only inside meshcore-linux without pulling the whole
// simple_repeater example into the Linux build.
//
// Sliding-window counter: allow up to `_maximum` events per `_secs` seconds.
// The window is reset (not slid) when it expires — simple and matches the
// upstream behaviour exactly.

#include <stdint.h>

class RateLimiter {
  uint32_t _start_timestamp;
  uint32_t _secs;
  uint16_t _maximum;
  uint16_t _count;

public:
  RateLimiter(uint16_t maximum, uint32_t secs)
    : _start_timestamp(0), _secs(secs), _maximum(maximum), _count(0) {}

  bool allow(uint32_t now) {
    if (now < _start_timestamp + _secs) {
      _count++;
      if (_count > _maximum) return false;
    } else {
      _start_timestamp = now;
      _count = 1;
    }
    return true;
  }
};
