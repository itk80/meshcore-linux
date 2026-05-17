#pragma once

// Vendored from MeshCore/examples/simple_repeater/RateLimiter.h.
// Fixed-window counter: allow `_maximum` events per `_secs` seconds.

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
