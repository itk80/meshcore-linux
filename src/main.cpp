// MeshCore-Linux — full stack entry point.
//
// Wires together:
//   - LinuxTcpRadio   (our mesh::Radio impl, talks to a pymc_usb modem)
//   - LinuxMesh       (mesh::Mesh subclass — boots Dispatcher + Mesh tables)
//   - LinuxMainBoard  (mesh::MainBoard host stubs)
//   - LinuxRTCClock   (clock_gettime/time())
//   - LinuxRNG        (getrandom(2))
//   - StaticPoolPacketManager + SimpleMeshTables (from upstream MeshCore)
//   - ConfigServer    (HTTP/JSON config API on port 5060)
//
// The Mesh is currently in passive-listener mode (LinuxMesh::onRecvPacket
// just logs + ACTION_RELEASE). Upgrading to full repeater = swap the base
// class to BaseChatMesh and implement its hooks (same body as MyMesh.cpp
// from the ESP32 simple_repeater example).

#include "LinuxTcpRadio.h"
#include "LinuxPlatform.h"
#include "LinuxMesh.h"
#include "ConfigServer.h"

#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include <nlohmann_json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;

// ── logger ──────────────────────────────────────────────────────────

static void logf(const char* level, const char* fmt, ...) {
  char tbuf[32];
  time_t now = std::time(nullptr);
  std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  std::fprintf(stderr, "%s %s ", tbuf, level);
  va_list ap; va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "\n");
}
#define LOGI(...) logf("INFO", __VA_ARGS__)
#define LOGW(...) logf("WARN", __VA_ARGS__)
#define LOGE(...) logf("ERR ", __VA_ARGS__)

// ── helpers ─────────────────────────────────────────────────────────

static std::string slurp(const std::string& path) {
  std::ifstream f(path); if (!f) return {};
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

template <typename T>
static T jget(const json& j, const char* ptr, T dflt) {
  try {
    auto p = json::json_pointer(ptr);
    return j.contains(p) ? j.at(p).get<T>() : dflt;
  } catch (...) { return dflt; }
}

// ── signal handling ─────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};
static void on_signal(int sig) {
  logf("INFO", "signal %d received, shutting down", sig);
  g_shutdown.store(true);
}

// ── main ────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  std::string cfg_path = (argc > 1) ? argv[1] : "/etc/Meshcore-Linux/config.json";
  std::string raw = slurp(cfg_path);
  if (raw.empty()) { LOGE("config file not found: %s", cfg_path.c_str()); return 2; }

  json cfg;
  try { cfg = json::parse(raw); }
  catch (const std::exception& e) { LOGE("config parse: %s", e.what()); return 2; }

  std::string host  = jget<std::string>(cfg, "/modem/host", "");
  int         port  = jget<int>        (cfg, "/modem/port", 5055);
  std::string token = jget<std::string>(cfg, "/modem/token", "");
  if (host.empty()) { LOGE("config: missing modem.host"); return 2; }

  std::string api_bind = jget<std::string>(cfg, "/config_api/bind", "0.0.0.0");
  int         api_port = jget<int>        (cfg, "/config_api/port", 5060);

  LOGI("MeshCore-Linux full-stack starting | modem=%s:%d | api=%s:%d",
       host.c_str(), port, api_bind.c_str(), api_port);

  // ── Platform impls ────────────────────────────────────────────────
  LinuxMainBoard    board;
  LinuxMillisClock  ms_clock;
  LinuxRTCClock     rtc;  rtc.begin();
  LinuxRNG          rng;
  StaticPoolPacketManager pkt_mgr(32);
  SimpleMeshTables  tables;
  LinuxTcpRadio     radio(host.c_str(), (uint16_t)port);

  radio.setLoRaParams(
    (float)  jget<double>(cfg, "/lora/freq_mhz",     869.618),
    (float)  jget<double>(cfg, "/lora/bw_khz",       62.5),
    (uint8_t)jget<int>   (cfg, "/lora/sf",           8),
    (uint8_t)jget<int>   (cfg, "/lora/cr",           8),
    (int8_t) jget<int>   (cfg, "/lora/tx_power_dbm", 22),
    (uint16_t)jget<int>  (cfg, "/lora/syncword",     0x0012),
    (uint8_t)jget<int>   (cfg, "/lora/preamble_len", 16));

  if (!token.empty()) {
    uint8_t buf[32]; size_t tlen = 0;
    for (size_t i = 0; i + 1 < token.size() && tlen < sizeof(buf); i += 2) {
      buf[tlen++] = (uint8_t)std::strtoul(token.substr(i, 2).c_str(), nullptr, 16);
    }
    radio.setAuthToken(buf, tlen);
  }

  // ── Mesh stack ────────────────────────────────────────────────────
  LinuxMesh mesh(radio, ms_clock, rng, rtc, pkt_mgr, tables);
  mesh.begin();          // brings the radio up (handshake) via Dispatcher::begin

  LOGI("mesh stack initialised (passive listener; full repeater behaviour"
       " = override BaseChatMesh hooks — follow-up)");

  // ── HTTP config API ───────────────────────────────────────────────
  std::mutex cfg_mu;
  ConfigServer api(radio, cfg, cfg_mu, cfg_path);
  api.setUptimeStart((uint64_t)std::time(nullptr));
  if (!api.start(api_bind, (uint16_t)api_port)) {
    LOGW("config API failed to bind %s:%d — continuing without it",
         api_bind.c_str(), api_port);
  } else {
    LOGI("config API listening on %s:%d", api_bind.c_str(), api_port);
  }

  // ── Main loop ─────────────────────────────────────────────────────
  while (!g_shutdown.load()) {
    mesh.loop();   // pumps Dispatcher (which pumps radio.loop, recvRaw etc.)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  api.stop();
  LOGI("clean shutdown (rx=%u tx=%u pong=%u crc=%u)",
       radio.getRxCount(), radio.getTxCount(),
       radio.getPongCount(), radio.getCrcErrors());
  return 0;
}
