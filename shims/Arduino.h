#pragma once

// Arduino.h — umbrella shim for meshcore-linux. Pulled in by every MeshCore
// helper that does `#include <Arduino.h>`. Re-exports our existing
// transport / clock / serial shims and adds the loose Arduino-isms that
// helper code expects (PROGMEM, F(), min/max, pinMode nops, etc.).

#include "PosixArduinoCompat.h"   // WiFiClient, millis, delay, Serial, esp_random
#include "Stream.h"               // Print / Stream

#include <algorithm>              // std::min / std::max
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// ── Flash-string macros (no-ops on hosts with unified memory) ─────────
#ifndef PROGMEM
  #define PROGMEM
#endif
#ifndef PGM_P
  #define PGM_P const char*
#endif
#ifndef F
  #define F(x) (x)
#endif
#ifndef FPSTR
  #define FPSTR(x) (x)
#endif
#define pgm_read_byte(addr)  (*(const uint8_t*)(addr))
#define pgm_read_word(addr)  (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))

// ── Common Arduino GPIO/level/pinmode constants — all nops on Linux ──
#ifndef HIGH
  #define HIGH 0x1
  #define LOW  0x0
#endif
#ifndef INPUT
  #define INPUT        0x0
  #define OUTPUT       0x1
  #define INPUT_PULLUP 0x2
  #define INPUT_PULLDOWN 0x3
#endif
#ifndef LSBFIRST
  #define LSBFIRST 0
  #define MSBFIRST 1
#endif

static inline void pinMode(int /*pin*/, int /*mode*/) {}
static inline int  digitalRead(int /*pin*/) { return 0; }
static inline void digitalWrite(int /*pin*/, int /*val*/) {}
static inline int  analogRead(int /*pin*/) { return 0; }
static inline void analogWrite(int /*pin*/, int /*val*/) {}
static inline void analogReadResolution(int /*bits*/) {}
static inline int  analogReadMilliVolts(int /*pin*/) { return 0; }
static inline void adcAttachPin(int /*pin*/) {}
static inline void noInterrupts() {}
static inline void interrupts() {}
static inline void yield() {}

// ── min/max/abs/constrain — templates (NOT macros), so libc++ stays sane
// while MeshCore's `min(a,b)` / `max(a,b)` usages still compile. ────────
template <typename A, typename B>
inline auto min(A a, B b) -> typename std::common_type<A,B>::type { return a < b ? a : b; }
template <typename A, typename B>
inline auto max(A a, B b) -> typename std::common_type<A,B>::type { return a > b ? a : b; }
template <typename T> inline T abs(T x) { return x < 0 ? -x : x; }
template <typename T, typename L, typename H>
inline T constrain(T x, L lo, H hi) { return x < lo ? (T)lo : (x > hi ? (T)hi : x); }

static inline long random(long max) {
  return (long)(esp_random() % (uint32_t)max);
}
static inline long random(long minv, long maxv) {
  return minv + random(maxv - minv);
}
static inline void randomSeed(unsigned long /*seed*/) {}

// ── String — minimal wrapper around std::string with the methods that
//   MeshCore helper code actually calls (constructors, concat, length,
//   c_str, indexing, substring, indexOf, startsWith, toInt). ───────────

class String {
public:
  String() = default;
  String(const char* s)        : _s(s ? s : "") {}
  String(const std::string& s) : _s(s) {}
  String(int v)                { char b[16]; snprintf(b, sizeof b, "%d",  v); _s = b; }
  String(unsigned v)           { char b[16]; snprintf(b, sizeof b, "%u",  v); _s = b; }
  String(long v)               { char b[24]; snprintf(b, sizeof b, "%ld", v); _s = b; }
  String(unsigned long v)      { char b[24]; snprintf(b, sizeof b, "%lu", v); _s = b; }
  String(float v)              { char b[24]; snprintf(b, sizeof b, "%g", (double)v); _s = b; }
  String(double v)             { char b[32]; snprintf(b, sizeof b, "%g", v); _s = b; }

  const char* c_str() const { return _s.c_str(); }
  size_t      length() const { return _s.size(); }
  bool        isEmpty() const { return _s.empty(); }
  char        charAt(size_t i) const { return i < _s.size() ? _s[i] : 0; }
  char        operator[](size_t i) const { return charAt(i); }

  String& operator+=(const char* s)         { _s += (s ? s : ""); return *this; }
  String& operator+=(const String& other)   { _s += other._s; return *this; }
  String& operator+=(char c)                { _s += c; return *this; }
  String  operator+ (const char* s) const   { return String(_s + (s ? s : "")); }
  String  operator+ (const String& o) const { return String(_s + o._s); }

  bool operator==(const String& o) const { return _s == o._s; }
  bool operator==(const char* s) const   { return s && _s == s; }
  bool operator!=(const String& o) const { return _s != o._s; }
  bool operator!=(const char* s) const   { return !(*this == s); }

  int indexOf(char c) const { auto p = _s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char* s) const { auto p = _s.find(s ? s : ""); return p == std::string::npos ? -1 : (int)p; }
  bool startsWith(const char* s) const   { return s && _s.rfind(s, 0) == 0; }
  bool startsWith(const String& s) const { return _s.rfind(s._s, 0) == 0; }
  bool endsWith(const char* s) const {
    if (!s) return false;
    size_t l = strlen(s);
    return _s.size() >= l && _s.compare(_s.size() - l, l, s) == 0;
  }
  String substring(size_t from) const                 { return String(_s.substr(from)); }
  String substring(size_t from, size_t to) const      { return String(_s.substr(from, to - from)); }
  long   toInt()  const { return strtol(_s.c_str(), nullptr, 10); }
  double toFloat() const { return strtod(_s.c_str(), nullptr); }
  void   trim() {
    size_t b = _s.find_first_not_of(" \t\r\n");
    size_t e = _s.find_last_not_of (" \t\r\n");
    _s = (b == std::string::npos) ? "" : _s.substr(b, e - b + 1);
  }
  void toUpperCase() { for (auto& c : _s) c = (char)std::toupper((unsigned char)c); }
  void toLowerCase() { for (auto& c : _s) c = (char)std::tolower((unsigned char)c); }

  const std::string& std() const { return _s; }
private:
  std::string _s;
};

inline String operator+(const char* lhs, const String& rhs) {
  return String(std::string(lhs ? lhs : "") + rhs.std());
}

// ── ltoa / itoa — POSIX doesn't have these by default ────────────────
static inline char* ltoa(long value, char* buf, int radix) {
  if (radix == 16) std::snprintf(buf, 32, "%lx", value);
  else if (radix == 8) std::snprintf(buf, 32, "%lo", value);
  else std::snprintf(buf, 32, "%ld", value);
  return buf;
}
static inline char* itoa(int value, char* buf, int radix) {
  return ltoa((long)value, buf, radix);
}
static inline char* ultoa(unsigned long value, char* buf, int radix) {
  if (radix == 16) std::snprintf(buf, 32, "%lx", value);
  else if (radix == 8) std::snprintf(buf, 32, "%lo", value);
  else std::snprintf(buf, 32, "%lu", value);
  return buf;
}
static inline char* utoa(unsigned int value, char* buf, int radix) {
  return ultoa((unsigned long)value, buf, radix);
}

// ── Abort helper used by some MeshCore paths ──────────────────────────
static inline void __attribute__((noreturn)) panic(const char* msg) {
  fprintf(stderr, "panic: %s\n", msg ? msg : "(no msg)");
  std::abort();
}
