#pragma once

// ConfigServer — HTTP/JSON config + status API (default :8080).
// Endpoints: GET / (HTML form), GET/POST /api/config, POST /api/command,
// GET /api/status, POST /api/reboot. No auth — LAN-only. Runs on its
// own thread so radio I/O in main keeps flowing.

#include "LinuxTcpRadio.h"
#include <nlohmann_json.hpp>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; }

class ConfigServer {
public:
  ConfigServer(LinuxTcpRadio& radio,
               nlohmann::json& live_config,
               std::mutex& config_mu,
               const std::string& config_path);
  ~ConfigServer();

  // Bind + serve on a background thread. Returns false on bind failure.
  bool start(const std::string& bind_addr, uint16_t port);
  void stop();

  // Stats exposed via /api/status (set by main loop).
  void setUptimeStart(uint64_t epoch_secs) { _uptime_start_secs = epoch_secs; }

  // CLI bridge — when set, POST /api/command {"command":"..."} forwards the
  // command to this lambda and returns its text reply. Used to plumb the
  // CommonCLI verbs (set freq, get neighbours, advert, etc.) over HTTP.
  using CliBridge = std::function<std::string(const std::string&)>;
  void setCliBridge(CliBridge fn) { _cli_bridge = std::move(fn); }

  // Live-overlay getter — when set, GET /api/config calls this callback to
  // pull current NodePrefs into the response. Caller is responsible for
  // serialising against the main mesh loop (mesh_mu in main.cpp).
  using LiveOverlay = std::function<void(nlohmann::json&)>;
  void setLiveOverlay(LiveOverlay fn) { _live_overlay = std::move(fn); }

private:
  LinuxTcpRadio&     _radio;
  nlohmann::json&    _live_config;     // mutable, shared with main
  std::mutex&        _config_mu;       // guards reads/writes of _live_config
  std::string        _config_path;     // /etc/Meshcore-Linux/config.json
  CliBridge          _cli_bridge;      // optional /api/command bridge
  LiveOverlay        _live_overlay;    // optional GET /api/config overlay

  std::unique_ptr<httplib::Server> _server;
  std::thread        _thread;
  std::atomic<bool>  _running{false};

  std::atomic<uint64_t> _uptime_start_secs{0};

  void route(httplib::Server& s);
  bool persistConfig(const nlohmann::json& cfg, std::string& err_out);
  void applyHotReload(const nlohmann::json& prev, const nlohmann::json& next);
};
