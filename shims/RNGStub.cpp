// RNGStub.cpp — linker shim for the rweather/Crypto library on Linux.
//
// Crypto's real RNG.cpp wants <Arduino.h> for ESP32 hardware bits, but
// MeshCore never invokes the codepaths that touch RNGClass (only the
// deterministic Ed25519::verify is used). Providing no-op definitions
// satisfies the linker without dragging in Arduino glue.
//
// If/when meshcore-linux needs real entropy for in-process key generation,
// we'll either route this stub to getrandom(2) or pass our own RNG to the
// MeshCore APIs (which already take a mesh::RNG* — see Identity ctor).

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
