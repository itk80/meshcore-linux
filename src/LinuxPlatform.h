#pragma once

// LinuxPlatform.h — header-only implementations of MeshCore's host
// abstractions (MainBoard / MillisecondClock / RTCClock / RNG) for Linux.

#include <Mesh.h>            // mesh::MainBoard, MillisecondClock
#include <Utils.h>           // mesh::RNG
#include <MeshCore.h>        // mesh::RTCClock

#include <chrono>
#include <cstdlib>           // exit
#include <ctime>
#if defined(__linux__)
  #include <sys/random.h>
#endif

class LinuxMainBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 0; }   // no battery on server
  const char* getManufacturerName() const override { return "MeshCore-Linux"; }
  void reboot() override {
    // systemd will restart on Restart=on-failure / always.
    fprintf(stderr, "[LinuxMainBoard] reboot() — exiting for systemd restart\n");
    std::exit(0);
  }
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

class LinuxMillisClock : public mesh::MillisecondClock {
public:
  unsigned long getMillis() override {
    using namespace std::chrono;
    return (unsigned long)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
  }
};

class LinuxRTCClock : public mesh::RTCClock {
public:
  void begin() {
    // Host clock is already set; nothing to initialise.
  }
  uint32_t getCurrentTime() override {
    return (uint32_t)std::time(nullptr);
  }
  void setCurrentTime(uint32_t t) override {
    // Setting the system clock requires CAP_SYS_TIME. Silently ignore in
    // the unprivileged service path; the network packets carry timestamps
    // for their own purposes, MeshCore just uses the local clock.
    (void)t;
  }
};

class LinuxRNG : public mesh::RNG {
public:
  void random(uint8_t* dest, size_t sz) override {
#if defined(__linux__)
    ssize_t got = 0;
    while ((size_t)got < sz) {
      ssize_t n = getrandom(dest + got, sz - (size_t)got, 0);
      if (n < 0) { fillFallback(dest + got, sz - (size_t)got); return; }
      got += n;
    }
#else
    fillFallback(dest, sz);
#endif
  }
private:
  static void fillFallback(uint8_t* dest, size_t sz) {
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f) {
      size_t got = std::fread(dest, 1, sz, f);
      std::fclose(f);
      if (got == sz) return;
    }
    // Last-ditch: time-based. Never expected in practice.
    for (size_t i = 0; i < sz; i++) dest[i] = (uint8_t)(std::time(nullptr) ^ i);
  }
};
