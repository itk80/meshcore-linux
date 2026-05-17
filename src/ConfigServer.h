#pragma once

// ConfigServer — HTTP/JSON config + status API on port 5060.
//
// Endpoints:
//   GET  /                  → tiny HTML page (textarea editor + status panel)
//   GET  /api/config        → current config as JSON
//   POST /api/config        → replace config, persist to file, hot-apply
//                             modem endpoint + LoRa params if they changed
//   GET  /api/status        → runtime status (modem connect/handshake/stats)
//   POST /api/reboot        → exit(0) — systemd will restart the service
//
// No authentication for v1; serve on LAN only. Run on its own thread so the
// main loop (radio I/O) keeps running while the server handles requests.

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

private:
  LinuxTcpRadio&     _radio;
  nlohmann::json&    _live_config;     // mutable, shared with main
  std::mutex&        _config_mu;       // guards reads/writes of _live_config
  std::string        _config_path;     // /etc/Meshcore-Linux/config.json
  CliBridge          _cli_bridge;      // optional /api/command bridge

  std::unique_ptr<httplib::Server> _server;
  std::thread        _thread;
  std::atomic<bool>  _running{false};

  std::atomic<uint64_t> _uptime_start_secs{0};

  void route(httplib::Server& s);
  bool persistConfig(const nlohmann::json& cfg, std::string& err_out);
  void applyHotReload(const nlohmann::json& prev, const nlohmann::json& next);
};
