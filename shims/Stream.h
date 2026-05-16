#pragma once

// Stream.h — minimal Arduino Stream/Print shim. MeshCore core (Utils.h) takes
// a `Stream&` for hex dumping. We provide just enough: the print(char/int/
// string) overloads, plus virtual write(uint8_t) so concrete subclasses can
// route bytes anywhere they like (stderr, file, socket).

#if !defined(ARDUINO)

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buf, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += write(buf[i]);
    return total;
  }
  // Arduino's Print::printf — does printf-style formatting, writes via write()
  size_t printf(const char* fmt, ...) {
    char tmp[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if ((size_t)n > sizeof tmp) n = sizeof tmp;
    return write((const uint8_t*)tmp, (size_t)n);
  }

  size_t print(char c)           { return write((uint8_t)c); }
  size_t print(const char* s)    { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t print(int v)            { char b[16]; int n = snprintf(b, sizeof b, "%d",  v); return write((const uint8_t*)b, n); }
  size_t print(unsigned v)       { char b[16]; int n = snprintf(b, sizeof b, "%u",  v); return write((const uint8_t*)b, n); }
  size_t print(long v)           { char b[24]; int n = snprintf(b, sizeof b, "%ld", v); return write((const uint8_t*)b, n); }
  size_t print(unsigned long v)  { char b[24]; int n = snprintf(b, sizeof b, "%lu", v); return write((const uint8_t*)b, n); }
  size_t print(float v)          { char b[24]; int n = snprintf(b, sizeof b, "%g",  (double)v); return write((const uint8_t*)b, n); }
  size_t print(double v)         { char b[32]; int n = snprintf(b, sizeof b, "%g",  v); return write((const uint8_t*)b, n); }

  size_t println()               { return write((uint8_t)'\n'); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
};

class Stream : public Print {
public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  virtual void flush() {}

  // Arduino Stream API — read up to `len` bytes into `buf`, returns count
  // actually read (might be < len if EOF / would-block).
  virtual size_t readBytes(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
      int b = read();
      if (b < 0) break;
      buf[got++] = (uint8_t)b;
    }
    return got;
  }
  // Convenience overload (some MeshCore code passes char*).
  size_t readBytes(char* buf, size_t len) { return readBytes((uint8_t*)buf, len); }
};

#endif  // !ARDUINO
