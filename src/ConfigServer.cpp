#include "ConfigServer.h"
#include <httplib.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
using namespace std::chrono;

static const char* INDEX_HTML = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Meshcore-Linux</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:920px;margin:1em auto;padding:0 1em;color:#222}
 h1,h2{margin:.4em 0}
 textarea{width:100%;height:18em;font-family:ui-monospace,Menlo,monospace;font-size:.9em;padding:.6em;box-sizing:border-box}
 button{padding:.55em 1em;border:0;border-radius:4px;cursor:pointer;font-size:.95em;margin-right:.5em}
 .save{background:#3a7;color:#fff}
 .reboot{background:#c63;color:#fff}
 .status{background:#f4f4f4;padding:1em;border-radius:6px;font-family:ui-monospace,Menlo,monospace;white-space:pre-wrap}
 .ok{color:#2a7}
 .err{color:#c33}
 .row{display:flex;align-items:center;gap:1em;margin:.6em 0}
 .muted{color:#666;font-size:.9em}
</style></head>
<body>
<h1>Meshcore-Linux</h1>
<p class="muted">Local config UI &mdash; same role as <code>config.meshcore.io</code> but for the Linux service. Edits POST to <code>/api/config</code> and persist to <code>/etc/Meshcore-Linux/config.json</code>.</p>

<h2>Status</h2>
<div class="status" id="status">loading&hellip;</div>
<div class="row">
  <button onclick="refresh()">Refresh</button>
  <button class="reboot" onclick="reboot()">Restart service</button>
</div>

<h2>Configuration</h2>
<textarea id="cfg">loading&hellip;</textarea>
<div class="row">
  <button class="save" onclick="save()">Save &amp; apply</button>
  <span id="msg" class="muted"></span>
</div>

<script>
async function load(){
  const r = await fetch('/api/config'); document.getElementById('cfg').value = JSON.stringify(await r.json(), null, 2);
}
async function refresh(){
  const r = await fetch('/api/status'); document.getElementById('status').textContent = JSON.stringify(await r.json(), null, 2);
}
async function save(){
  const msg = document.getElementById('msg');
  msg.textContent = 'saving…'; msg.className = 'muted';
  let body; try { body = JSON.parse(document.getElementById('cfg').value); }
  catch(e){ msg.textContent = 'JSON parse error: ' + e.message; msg.className = 'err'; return; }
  const r = await fetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
  const j = await r.json();
  msg.textContent = j.ok ? 'saved · '+(j.applied||'') : ('error: '+j.error);
  msg.className = j.ok ? 'ok' : 'err';
  setTimeout(refresh, 400);
}
async function reboot(){
  if(!confirm('Restart service?')) return;
  await fetch('/api/reboot', {method:'POST'});
  document.getElementById('msg').textContent = 'service exiting — systemd will restart shortly';
}
load(); refresh(); setInterval(refresh, 5000);
</script>
</body></html>
)HTML";

// ── helpers ──────────────────────────────────────────────────────────

static uint16_t json_u16(const json& j, const char* path, uint16_t dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<uint16_t>();
  } catch (...) { return dflt; }
}
static std::string json_str(const json& j, const char* path, const std::string& dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<std::string>();
  } catch (...) { return dflt; }
}
static double json_dbl(const json& j, const char* path, double dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<double>();
  } catch (...) { return dflt; }
}
static int json_int(const json& j, const char* path, int dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<int>();
  } catch (...) { return dflt; }
}

// ── ConfigServer ─────────────────────────────────────────────────────

ConfigServer::ConfigServer(LinuxTcpRadio& radio,
                           json& live_config,
                           std::mutex& config_mu,
                           const std::string& config_path)
  : _radio(radio),
    _live_config(live_config),
    _config_mu(config_mu),
    _config_path(config_path)
{}

ConfigServer::~ConfigServer() { stop(); }

bool ConfigServer::start(const std::string& bind_addr, uint16_t port) {
  if (_running.load()) return true;
  _server = std::make_unique<httplib::Server>();
  route(*_server);

  if (!_server->bind_to_port(bind_addr.c_str(), port)) {
    fprintf(stderr, "ConfigServer: bind %s:%u failed\n", bind_addr.c_str(), port);
    _server.reset();
    return false;
  }
  _running.store(true);
  _thread = std::thread([this]() { _server->listen_after_bind(); _running.store(false); });
  return true;
}

void ConfigServer::stop() {
  if (_server) _server->stop();
  if (_thread.joinable()) _thread.join();
  _server.reset();
  _running.store(false);
}

bool ConfigServer::persistConfig(const json& cfg, std::string& err_out) {
  std::ofstream f(_config_path);
  if (!f) { err_out = "cannot open " + _config_path + " for writing"; return false; }
  f << cfg.dump(2) << "\n";
  if (!f) { err_out = "write to " + _config_path + " failed"; return false; }
  return true;
}

void ConfigServer::applyHotReload(const json& prev, const json& next) {
  // Modem endpoint change → forceReconnect via setEndpoint
  std::string prev_host = json_str(prev, "/modem/host", "");
  std::string next_host = json_str(next, "/modem/host", "");
  uint16_t    prev_port = json_u16(prev, "/modem/port", 5055);
  uint16_t    next_port = json_u16(next, "/modem/port", 5055);
  if (next_host != prev_host || next_port != prev_port) {
    if (!next_host.empty()) _radio.setEndpoint(next_host.c_str(), next_port);
  }

  // Auth token (hex string)
  std::string next_token_hex = json_str(next, "/modem/token", "");
  if (next_token_hex != json_str(prev, "/modem/token", "")) {
    uint8_t buf[32]; size_t n = 0;
    for (size_t i = 0; i + 1 < next_token_hex.size() && n < sizeof(buf); i += 2) {
      buf[n++] = (uint8_t)std::strtoul(next_token_hex.substr(i, 2).c_str(), nullptr, 16);
    }
    _radio.setAuthToken(buf, n);
  }

  // LoRa params — push to modem via setLoRaParams (forces fresh SET_CONFIG
  // on next handshake / on next setEndpoint-triggered reconnect).
  _radio.setLoRaParams(
    (float)json_dbl(next, "/lora/freq_mhz",   869.618),
    (float)json_dbl(next, "/lora/bw_khz",     62.5),
    (uint8_t)json_int(next, "/lora/sf",         8),
    (uint8_t)json_int(next, "/lora/cr",         8),
    (int8_t) json_int(next, "/lora/tx_power_dbm", 22),
    (uint16_t)json_int(next, "/lora/syncword", 0x0012),
    (uint8_t)json_int(next, "/lora/preamble_len", 16));
}

void ConfigServer::route(httplib::Server& s) {
  s.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(INDEX_HTML, "text/html; charset=utf-8");
  });

  s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(_config_mu);
    res.set_content(_live_config.dump(2), "application/json");
  });

  s.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
    json next;
    try { next = json::parse(req.body); }
    catch (const std::exception& e) {
      res.status = 400;
      res.set_content(json{{"ok",false},{"error", std::string("parse: ")+e.what()}}.dump(),
                      "application/json");
      return;
    }
    json prev;
    {
      std::lock_guard<std::mutex> lk(_config_mu);
      prev = _live_config;
    }
    std::string err;
    if (!persistConfig(next, err)) {
      res.status = 500;
      res.set_content(json{{"ok",false},{"error",err}}.dump(), "application/json");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(_config_mu);
      _live_config = next;
    }
    applyHotReload(prev, next);
    res.set_content(json{
      {"ok", true},
      {"applied", "modem endpoint + LoRa params hot-applied; api_port change needs restart"}
    }.dump(), "application/json");
  });

  s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
    uint64_t now = (uint64_t)time(nullptr);
    uint64_t up0 = _uptime_start_secs.load();
    json j = {
      {"modem", {
        {"host",        _radio.getModemHost()},
        {"port",        _radio.getModemPort()},
        {"connected",   _radio.isConnected()},
        {"handshake",   _radio.isHandshakeComplete()},
        {"rx_mode",     _radio.isInRecvMode()},
        {"reconnects",  _radio.getReconnectCount()},
      }},
      {"stats", {
        {"rx",       _radio.getRxCount()},
        {"tx",       _radio.getTxCount()},
        {"pong",     _radio.getPongCount()},
        {"crc_err",  _radio.getCrcErrors()},
        {"noise_dbm", _radio.getNoiseFloor()},
        {"last_rssi", _radio.getLastRSSI()},
        {"last_snr",  _radio.getLastSNR()},
      }},
      {"service", {
        {"uptime_secs", (up0 > 0 && now >= up0) ? (now - up0) : 0},
      }},
    };
    res.set_content(j.dump(2), "application/json");
  });

  s.Post("/api/command", [this](const httplib::Request& req, httplib::Response& res) {
    if (!_cli_bridge) {
      res.status = 503;
      res.set_content(json{{"ok",false},{"error","no CLI bridge wired"}}.dump(),
                      "application/json");
      return;
    }
    std::string cmd;
    try {
      auto body = json::parse(req.body);
      cmd = body.value("command", "");
    } catch (...) {
      // also accept plain text bodies for curl simplicity
      cmd = req.body;
    }
    if (cmd.empty()) {
      res.status = 400;
      res.set_content(json{{"ok",false},{"error","empty command"}}.dump(),
                      "application/json");
      return;
    }
    std::string reply = _cli_bridge(cmd);
    res.set_content(json{{"ok",true},{"command",cmd},{"reply",reply}}.dump(2),
                    "application/json");
  });

  s.Post("/api/reboot", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(json{{"ok",true},{"msg","exiting — systemd will restart"}}.dump(),
                    "application/json");
    // Schedule exit shortly so the response can flush.
    std::thread([]{ std::this_thread::sleep_for(std::chrono::milliseconds(150)); std::_Exit(0); }).detach();
  });
}
