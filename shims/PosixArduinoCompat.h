#pragma once

// PosixArduinoCompat.h — minimal Arduino-API shim for meshcore-linux.
// Header-only, guarded by `#if !ARDUINO`. Exposes the subset MeshCore
// needs on a POSIX host: millis/delay/Serial/esp_random/WiFiClient.

#if !defined(ARDUINO) && !defined(ESP32) && !defined(ARDUINO_ARCH_ESP32)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if defined(__linux__)
  #include <sys/random.h>
#endif
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

// ── millis() / delay() ────────────────────────────────────────────────

static inline unsigned long millis() {
  using namespace std::chrono;
  return (unsigned long)duration_cast<milliseconds>(
      steady_clock::now().time_since_epoch()).count();
}

static inline void delay(unsigned long ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ── Serial: prints to stderr with [time] prefix per line ─────────────

class SerialStub {
public:
  void begin(unsigned long /*baud*/) { /* no-op on Linux */ }

  void printf(const char* fmt, ...) {
    prefix();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }

  void print(const char* s)   { prefix(); fputs(s, stderr); }
  void print(int v)           { prefix(); fprintf(stderr, "%d", v); }
  void print(unsigned v)      { prefix(); fprintf(stderr, "%u", v); }
  void print(long v)          { prefix(); fprintf(stderr, "%ld", v); }
  void print(unsigned long v) { prefix(); fprintf(stderr, "%lu", v); }
  void print(float v)         { prefix(); fprintf(stderr, "%g", (double)v); }
  void print(double v)        { prefix(); fprintf(stderr, "%g", v); }
  void print(char c)          { prefix(); fputc(c, stderr); }

  void println()              { fputc('\n', stderr); _line_active = false; }
  template <typename T> void println(T v) { print(v); println(); }

  size_t write(const uint8_t* buf, size_t n) {
    prefix();
    return fwrite(buf, 1, n, stderr);
  }
  size_t write(uint8_t b) { prefix(); fputc(b, stderr); return 1; }

  int available() { return 0; }
  int read()      { return -1; }
  void flush()    { fflush(stderr); }

private:
  bool _line_active = false;
  void prefix() {
    if (_line_active) return;
    char tbuf[32];
    time_t now = std::time(nullptr);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    fprintf(stderr, "%s ", tbuf);
    _line_active = true;
  }
};

static SerialStub Serial;

// ── esp_random() ─────────────────────────────────────────────────────

static inline uint32_t esp_random() {
  uint32_t r = 0;
#if defined(__linux__)
  ssize_t n = getrandom(&r, sizeof(r), 0);
  if (n == (ssize_t)sizeof(r)) return r;
#endif
  // Fallback / primary path on non-Linux POSIX (e.g. macOS dev).
  FILE* f = fopen("/dev/urandom", "rb");
  if (f) {
    if (fread(&r, 1, sizeof(r), f) != sizeof(r)) r = (uint32_t)millis();
    fclose(f);
  } else {
    r = (uint32_t)millis();
  }
  return r;
}

// WiFiClient (POSIX socket) — connect/connected/available/read/write/
// stop/setNoDelay/fd. read() is byte-at-a-time. stop() forces TCP RST
// via SO_LINGER={1,0} so single-client modems free their slot immediately.

class WiFiClient {
public:
  WiFiClient() = default;
  ~WiFiClient() { stop(); }

  WiFiClient(const WiFiClient&) = delete;
  WiFiClient& operator=(const WiFiClient&) = delete;

  bool connect(const char* host, uint16_t port) {
    stop();
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) return false;
    _fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (_fd < 0) { freeaddrinfo(res); return false; }
    int ok = ::connect(_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ok != 0) { ::close(_fd); _fd = -1; return false; }
    int fl = fcntl(_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(_fd, F_SETFL, fl | O_NONBLOCK);
    return true;
  }

  bool connected() {
    if (_fd < 0) return false;
    char dummy;
    ssize_t n = ::recv(_fd, &dummy, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return true;
    if (n == 0) { stop(); return false; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    stop();
    return false;
  }

  int available() {
    if (_fd < 0) return 0;
    char probe;
    ssize_t n = ::recv(_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    return (n > 0) ? 1 : 0;
  }

  int read() {
    if (_fd < 0) return -1;
    uint8_t b;
    ssize_t n = ::recv(_fd, &b, 1, MSG_DONTWAIT);
    if (n == 1) return b;
    if (n == 0) { stop(); return -1; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    stop();
    return -1;
  }

  size_t write(const uint8_t* data, size_t n) {
    if (_fd < 0) return 0;
    size_t sent = 0;
    while (sent < n) {
      ssize_t w = ::send(_fd, data + sent, n - sent, MSG_NOSIGNAL);
      if (w < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        stop();
        return sent;
      }
      sent += (size_t)w;
    }
    return sent;
  }

  void stop() {
    if (_fd >= 0) {
      struct linger lg = { 1, 0 };
      setsockopt(_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
      ::close(_fd);
      _fd = -1;
    }
  }

  void setNoDelay(bool on) {
    if (_fd < 0) return;
    int v = on ? 1 : 0;
    setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
  }

  int fd() const { return _fd; }

private:
  int _fd = -1;
};

#endif  // !ARDUINO
