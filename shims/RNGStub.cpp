// RNGStub.cpp — linker shim for rweather/Crypto on Linux.
// MeshCore never calls into RNGClass (only deterministic Ed25519::verify);
// no-op definitions keep the linker happy without ESP32 Arduino glue.

#include <RNG.h>

RNGClass::RNGClass() {}
RNGClass::~RNGClass() {}
void RNGClass::begin(const char* /*tag*/) {}
void RNGClass::addNoiseSource(NoiseSource& /*source*/) {}
void RNGClass::setAutoSaveTime(uint16_t /*minutes*/) {}
void RNGClass::rand(uint8_t* data, size_t len) {
  // Best-effort: zero out so callers don't get uninitialised memory.
  // This stub is never actually invoked by MeshCore's code path.
  for (size_t i = 0; i < len; i++) data[i] = 0;
}
bool RNGClass::available(size_t /*len*/) const { return false; }
void RNGClass::stir(const uint8_t* /*data*/, size_t /*len*/, unsigned int /*credit*/) {}
void RNGClass::save() {}
void RNGClass::loop() {}
void RNGClass::destroy() {}

RNGClass RNG;
