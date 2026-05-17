// MeshCore-Linux entry point — wires LinuxTcpRadio (pymc_usb wire
// protocol v0.7), LinuxRepeaterMesh, host abstractions and ConfigServer.

#include "LinuxTcpRadio.h"
#include "LinuxPlatform.h"
#include "LinuxRepeaterMesh.h"
#include "ConfigServer.h"

#include <FS.h>     // FSImpl backed by /var/lib/Meshcore-Linux

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

static void logf(const char* level, const char* fmt, ...) {
  char tbuf[32];
  time_t now = std::time(nullptr);
  std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  std::fprintf(stderr, "%s %s ", tbuf, level);
  va_list ap; va_start(ap, fmt); std::vfprintf(stderr, fmt, ap); va_end(ap);
  std::fprintf(stderr, "\n");
}
#define LOGI(...) logf("INFO", __VA_ARGS__)
#define LOGW(...) logf("WARN", __VA_ARGS__)
#define LOGE(...) logf("ERR ", __VA_ARGS__)

static std::string slurp(const std::string& path) {
  std::ifstream f(path); if (!f) return {};
  std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

template <typename T>
static T jget(const json& j, const char* ptr, T dflt) {
  try {
    auto p = json::json_pointer(ptr);
    return j.contains(p) ? j.at(p).get<T>() : dflt;
  } catch (...) { return dflt; }
}

static std::atomic<bool> g_shutdown{false};
static void on_signal(int sig) {
  logf("INFO", "signal %d received, shutting down", sig);
  g_shutdown.store(true);
}

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
  int         api_port = jget<int>        (cfg, "/config_api/port", 8080);
  std::string state_dir = jget<std::string>(cfg, "/state_dir", "/var/lib/Meshcore-Linux");

  LOGI("MeshCore-Linux full-repeater starting | modem=%s:%d | api=%s:%d | state=%s",
       host.c_str(), port, api_bind.c_str(), api_port, state_dir.c_str());

  // ── Platform impls ────────────────────────────────────────────────
  LinuxMainBoard    board;
  LinuxMillisClock  ms_clock;
  LinuxRTCClock     rtc; rtc.begin();
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

  // ── Persistent state filesystem ───────────────────────────────────
  FSImpl fs(state_dir);
  if (!fs.begin()) {
    LOGE("cannot create state dir %s — running without persistence", state_dir.c_str());
  }

  // ── Mesh + Repeater ──────────────────────────────────────────────
  LinuxRepeaterMesh mesh(board, radio, ms_clock, rng, rtc, pkt_mgr, tables);

  // Identity lives IN the JSON config (operator never has to manage it).
  // Lazy-generated on first start, persisted atomically, preserved across
  // reinstalls because install.sh leaves an existing config.json alone.
  std::string id_pub = jget<std::string>(cfg, "/identity/pub", "");
  std::string id_prv = jget<std::string>(cfg, "/identity/prv", "");

  auto identity_persist =
    [&cfg, &cfg_path](const std::string& pub_hex, const std::string& prv_hex) {
      // Persist back into the live JSON + flush to disk atomically.
      cfg["identity"]["pub"] = pub_hex;
      cfg["identity"]["prv"] = prv_hex;
      std::string tmp = cfg_path + ".tmp";
      std::ofstream f(tmp);
      if (f) {
        f << cfg.dump(2) << "\n";
        f.close();
        if (std::rename(tmp.c_str(), cfg_path.c_str()) != 0) {
          fprintf(stderr, "[main] WARNING: identity NOT persisted (rename failed); "
                  "will be regenerated on next start\n");
        } else {
          fprintf(stderr, "[main] identity persisted to %s\n", cfg_path.c_str());
        }
      } else {
        fprintf(stderr, "[main] WARNING: cannot write %s — identity NOT persisted\n",
                tmp.c_str());
      }
    };

  mesh.bringUp(fs, id_pub, id_prv, identity_persist);

  // Seed NodePrefs on FIRST boot from config.json — name, location, passwords.
  // Idempotent: only fills empty fields, so on subsequent boots the persisted
  // com_prefs wins.
  mesh.seedPrefsFromConfig(
    jget<std::string>(cfg, "/node/name", ""),
    jget<double>     (cfg, "/node/lat",  0.0),
    jget<double>     (cfg, "/node/lon",  0.0),
    jget<std::string>(cfg, "/node/admin_password", ""),
    jget<std::string>(cfg, "/node/guest_password", ""));
  LOGI("repeater up; node='%s' freq=%.3fMHz sf=%u cr=%u",
       mesh.getNodePrefs()->node_name,
       (double)mesh.getNodePrefs()->freq,
       (unsigned)mesh.getNodePrefs()->sf,
       (unsigned)mesh.getNodePrefs()->cr);

  // First self-advert is scheduled by bringUp() (next_local_advert = +5s),
  // so by the time we hit the main loop tick the identity is already
  // loaded/generated. Do NOT pre-emptively call sendSelfAdvertisement here.

  // ── HTTP config API + CLI bridge ──────────────────────────────────
  std::mutex cfg_mu;
  std::mutex mesh_mu;   // serialises HTTP-thread CLI access vs main-loop mesh.tick()
  ConfigServer api(radio, cfg, cfg_mu, cfg_path);
  api.setUptimeStart((uint64_t)std::time(nullptr));
  api.setCliBridge([&mesh, &mesh_mu](const std::string& cmd) -> std::string {
    static thread_local char reply[4096];
    reply[0] = '\0';
    std::lock_guard<std::mutex> lk(mesh_mu);
    mesh.processCommand(cmd.c_str(), reply);
    return std::string(reply);
  });
  // GET /api/config overlay — refresh node/lora/repeater scalars from current
  // NodePrefs so the configurator UI shows whatever was last set via CLI or
  // over LoRa from a mobile admin client.
  api.setLiveOverlay([&mesh, &mesh_mu](nlohmann::json& cfg) {
    LinuxRepeaterMesh::LiveSnapshot s;
    {
      std::lock_guard<std::mutex> lk(mesh_mu);
      mesh.snapshotLivePrefs(s);
    }
    cfg["node"]["name"]            = s.node_name;
    cfg["node"]["lat"]             = s.node_lat;
    cfg["node"]["lon"]             = s.node_lon;
    cfg["node"]["admin_password"]  = s.admin_password;
    cfg["node"]["guest_password"]  = s.guest_password;
    cfg["node"]["owner_info"]      = s.owner_info;
    cfg["lora"]["freq_mhz"]        = s.freq_mhz;
    cfg["lora"]["bw_khz"]          = s.bw_khz;
    cfg["lora"]["sf"]              = s.sf;
    cfg["lora"]["cr"]              = s.cr;
    cfg["lora"]["tx_power_dbm"]    = s.tx_power_dbm;
    cfg["repeater"]["advert_interval_min"]      = s.advert_interval_min;
    cfg["repeater"]["flood_advert_interval_h"]  = s.flood_advert_interval_h;
    cfg["repeater"]["flood_max_hops"]           = s.flood_max_hops;
    cfg["repeater"]["duty_cycle_pct"]           = s.duty_cycle_pct;
    cfg["repeater"]["interference_threshold"]   = s.interference_threshold;
    cfg["repeater"]["agc_reset_interval"]       = s.agc_reset_interval;
    cfg["repeater"]["rx_delay_base"]            = s.rx_delay_base;
    cfg["repeater"]["tx_delay_factor"]          = s.tx_delay_factor;
    cfg["repeater"]["direct_tx_delay_factor"]   = s.direct_tx_delay_factor;
    cfg["repeater"]["multi_acks"]               = (bool)s.multi_acks;
    cfg["repeater"]["path_hash_mode"]           = s.path_hash_mode;
    static const char* LOOP_NAMES[] = {"off","minimal","moderate","strict"};
    cfg["repeater"]["loop_detect"]              = LOOP_NAMES[std::min<uint8_t>(s.loop_detect, 3)];
    // Identity pub only — we never expose prv over HTTP (operator can read
    // /etc/Meshcore-Linux/config.json directly if they need to back it up).
    cfg["identity"]["pub"]                      = s.identity_pub_hex;
  });
  if (!api.start(api_bind, (uint16_t)api_port)) {
    LOGW("config API failed to bind %s:%d", api_bind.c_str(), api_port);
  } else {
    LOGI("config API listening on %s:%d", api_bind.c_str(), api_port);
  }

  // ── Main loop ─────────────────────────────────────────────────────
  while (!g_shutdown.load()) {
    {
      std::lock_guard<std::mutex> lk(mesh_mu);
      mesh.tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  api.stop();
  LOGI("clean shutdown (rx=%u tx=%u pong=%u crc=%u, mesh sent_flood=%u recv_flood=%u)",
       radio.getRxCount(), radio.getTxCount(),
       radio.getPongCount(), radio.getCrcErrors(),
       (unsigned)mesh.getNumSentFlood(), (unsigned)mesh.getNumRecvFlood());
  return 0;
}
