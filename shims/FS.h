#pragma once
// FS.h — minimal Arduino FS/SPIFFS/LittleFS shim backed by POSIX stdio.
//
// IdentityStore / ClientACL / CommonCLI take a `FILESYSTEM&` and use it to
// open/read/write/remove small binary blobs (identity, prefs, contacts).
// We model just enough of the Arduino `File` + `FILESYSTEM` API to keep
// MeshCore's persistence code happy on Linux.

#include "Stream.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>

#ifndef FILE_O_WRITE
  #define FILE_O_WRITE "w"
#endif
#ifndef FILE_O_READ
  #define FILE_O_READ "r"
#endif

class File : public Stream {
public:
  File() = default;
  explicit File(FILE* f) : _f(f) {}
  ~File() { close(); }

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept : _f(other._f) { other._f = nullptr; }
  File& operator=(File&& other) noexcept {
    if (this != &other) { close(); _f = other._f; other._f = nullptr; }
    return *this;
  }

  explicit operator bool() const { return _f != nullptr; }

  // Print/Stream overrides (single-byte and bulk write/read).
  size_t write(uint8_t b) override { return _f ? std::fwrite(&b, 1, 1, _f) : 0; }
  size_t write(const uint8_t* buf, size_t n) override {
    return _f ? std::fwrite(buf, 1, n, _f) : 0;
  }
  int read() override {
    if (!_f) return -1;
    int c = std::fgetc(_f);
    return c == EOF ? -1 : c;
  }
  int peek() override {
    if (!_f) return -1;
    int c = std::fgetc(_f);
    if (c != EOF) std::ungetc(c, _f);
    return c == EOF ? -1 : c;
  }
  void flush() override { if (_f) std::fflush(_f); }

  // Bulk read used by IdentityStore via Stream::readBytes — base class
  // version loops through read(), which works but is slow; override for
  // raw fread efficiency.
  size_t readBytes(uint8_t* buf, size_t n) override {
    return _f ? std::fread(buf, 1, n, _f) : 0;
  }

  // Direct conveniences (some MeshCore code uses these).
  size_t read(uint8_t* buf, size_t n) {
    return _f ? std::fread(buf, 1, n, _f) : 0;
  }
  int    available() override {
    if (!_f) return 0;
    long pos = std::ftell(_f);
    std::fseek(_f, 0, SEEK_END);
    long end = std::ftell(_f);
    std::fseek(_f, pos, SEEK_SET);
    return (int)(end - pos);
  }
  size_t size() {
    if (!_f) return 0;
    long pos = std::ftell(_f);
    std::fseek(_f, 0, SEEK_END);
    long end = std::ftell(_f);
    std::fseek(_f, pos, SEEK_SET);
    return (size_t)end;
  }
  size_t position() const { return _f ? (size_t)std::ftell(_f) : 0; }
  bool   seek(size_t pos) { return _f && std::fseek(_f, (long)pos, SEEK_SET) == 0; }
  void   close() { if (_f) { std::fclose(_f); _f = nullptr; } }

private:
  FILE* _f = nullptr;
};

class FSImpl {
public:
  FSImpl() = default;
  explicit FSImpl(const std::string& root) : _root(root) {}

  // Initialise root dir (mkdir -p). `format_on_fail` ignored.
  bool begin(bool /*format_on_fail*/ = false, const char* /*partition_label*/ = nullptr) {
    return mkdirs(_root.c_str());
  }
  void end() {}

  bool exists(const char* path) {
    std::string p = abspath(path);
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
  }
  bool remove(const char* path) {
    return ::unlink(abspath(path).c_str()) == 0;
  }
  bool rename(const char* from, const char* to) {
    return ::rename(abspath(from).c_str(), abspath(to).c_str()) == 0;
  }
  bool mkdir(const char* path) { return ::mkdir(abspath(path).c_str(), 0755) == 0; }

  // Arduino API variants we see in MeshCore:
  //   open(path)            → read
  //   open(path, "r"|"w")   → mode-specified
  //   open(path, "w", true) → create if missing (ESP32 SPIFFS quirk)
  File open(const char* path) {
    std::string p = abspath(path);
    return File(std::fopen(p.c_str(), "rb"));
  }
  File open(const char* path, const char* mode, bool create = false) {
    std::string p = abspath(path);
    if (create) {
      // ensure parent exists
      auto slash = p.find_last_of('/');
      if (slash != std::string::npos) mkdirs(p.substr(0, slash).c_str());
    }
    const char* fmode = "rb";
    if (mode && *mode == 'w') fmode = "wb";
    else if (mode && *mode == 'a') fmode = "ab";
    else if (mode && *mode == 'r' && mode[1] == 'b') fmode = "rb";
    else if (mode && *mode == 'w' && mode[1] == 'b') fmode = "wb";
    return File(std::fopen(p.c_str(), fmode));
  }

  // Some MeshCore code uses these constants for File::open mode.
  // The strings already cover "r"/"w" — provide aliases if needed.

  void setRoot(const std::string& r) { _root = r; }
  const std::string& root() const { return _root; }

private:
  std::string _root = ".";

  std::string abspath(const char* p) const {
    if (!p) return _root;
    if (p[0] == '/') return _root + p;   // treat /foo as relative to root
    return _root + "/" + p;
  }
  static bool mkdirs(const char* path) {
    if (!path || !*path) return true;
    std::string s(path);
    for (size_t i = 1; i <= s.size(); i++) {
      if (i == s.size() || s[i] == '/') {
        std::string seg = s.substr(0, i);
        if (!seg.empty() && ::mkdir(seg.c_str(), 0755) != 0 && errno != EEXIST) return false;
      }
    }
    return true;
  }
};

using FILESYSTEM = FSImpl;
