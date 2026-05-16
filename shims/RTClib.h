#pragma once
// Minimal Adafruit RTClib shim — only DateTime + a no-op RTC base class.
// MeshCore helpers use DateTime mostly for timestamps; full RTC chip drivers
// are unused on Linux (we have the host's clock).

#include <cstdint>
#include <ctime>

class DateTime {
public:
  DateTime() : _t(0) {}
  DateTime(uint32_t unix_t) : _t(unix_t) {}
  DateTime(uint16_t y, uint8_t mo, uint8_t d,
           uint8_t h = 0, uint8_t mi = 0, uint8_t s = 0) {
    struct tm tm = {};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    _t = (uint32_t)timegm(&tm);
  }
  uint32_t unixtime() const { return _t; }
  uint16_t year() const   { struct tm tm; time_t t = _t; gmtime_r(&t,&tm); return (uint16_t)(tm.tm_year + 1900); }
  uint8_t  month() const  { struct tm tm; time_t t = _t; gmtime_r(&t,&tm); return (uint8_t)(tm.tm_mon + 1); }
  uint8_t  day() const    { struct tm tm; time_t t = _t; gmtime_r(&t,&tm); return (uint8_t)tm.tm_mday; }
  uint8_t  hour() const   { struct tm tm; time_t t = _t; gmtime_r(&t,&tm); return (uint8_t)tm.tm_hour; }
  uint8_t  minute() const { struct tm tm; time_t t = _t; gmtime_r(&t,&tm); return (uint8_t)tm.tm_min; }
  uint8_t  second() const { struct tm tm; time_t t = _t; gmtime_r(&t,&tm); return (uint8_t)tm.tm_sec; }
private:
  uint32_t _t;
};

// Token classes — most MeshCore code passes them around but doesn't poke
// hardware on Linux.
class RTC_DS3231 { public: bool begin() { return false; } DateTime now() { return DateTime((uint32_t)time(nullptr)); } void adjust(const DateTime&) {} };
class RTC_PCF8523 : public RTC_DS3231 {};
class RTC_PCF8563 : public RTC_DS3231 {};
class RTC_DS1307  : public RTC_DS3231 {};
