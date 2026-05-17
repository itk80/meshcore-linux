#pragma once

// LinuxRepeaterMesh — full repeater behaviour on Linux.
//
// Inherits mesh::Mesh (low-level) + CommonCLICallbacks. Same shape as the
// ESP32 simple_repeater/MyMesh but stripped of board-specific concerns
// (OLED, sensors, RS232/ESPNow bridges, GPS, battery, BLE). The repeater
// surface MeshCore exposes is largely board-agnostic — adverts, ACL,
// flood-forward decisions, CLI — and ports cleanly.
//
// Persistence root: /var/lib/Meshcore-Linux/ (created via FSImpl::begin).
//   /com_prefs                 NodePrefs blob   (CommonCLI)
//   /identity                  IdentityStore root (LocalIdentity + name list)
//   /acl                       ClientACL persistence
//   /regions                   RegionMap persistence
//
// Packet log lives inside the state dir at /var/lib/Meshcore-Linux/packets.log
// (configurable via setPacketLogPath). Operators can plug logrotate against
// that path.
//
// CLI commands are bridged via processCommand(cmd, reply) — meant to be
// called from the HTTP /api/command endpoint AND from over-air TXT (admin
// clients via onPeerDataRecv).

#include <Mesh.h>
#include <cstdio>
#include <functional>
#include <string>
#include "LinuxTcpRadio.h"
#include "RateLimiter.h"
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/CommonCLI.h>
#include <helpers/ClientACL.h>
#include <helpers/RegionMap.h>
#include <helpers/IdentityStore.h>
#include <helpers/SensorManager.h>
#include <helpers/TransportKeyStore.h>
#include <helpers/AdvertDataHelpers.h>

#ifndef MAX_NEIGHBOURS
  #define MAX_NEIGHBOURS  32
#endif
#ifndef MAX_CLIENTS
  #define MAX_CLIENTS     32
#endif
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION  "v0.1.0-linux"
#endif
#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE  __DATE__
#endif
#define FIRMWARE_ROLE  "repeater"

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t   snr;  // dB × 4
};

class LinuxRepeaterMesh : public mesh::Mesh, public CommonCLICallbacks {
public:
  LinuxRepeaterMesh(mesh::MainBoard& board, LinuxTcpRadio& radio,
                    mesh::MillisecondClock& ms, mesh::RNG& rng,
                    mesh::RTCClock& rtc, mesh::PacketManager& mgr,
                    mesh::MeshTables& tables);

  using OnIdentityGenerated = std::function<void(const std::string& pub_hex,
                                                  const std::string& prv_hex)>;
  void bringUp(FILESYSTEM& fs,
               const std::string& identity_pub_hex,
               const std::string& identity_prv_hex,
               OnIdentityGenerated on_generated);

  void seedPrefsFromConfig(const std::string& name,
                           double lat, double lon,
                           const std::string& admin_password,
                           const std::string& guest_password);

  // Where logRx/logTx/logTxFail append entries when _logging is true.
  // Defaults to /var/log/Meshcore-Linux/packets.log. The log file is
  // truncated by `eraseLogFile()` (called via CLI `log erase`).
  void setPacketLogPath(const std::string& path) { _log_path = path; }

  void processCommand(const char* command, char* reply);

  NodePrefs* getNodePrefs() { return &_prefs; }

  // Snapshot live NodePrefs + identity into config-schema scalars. ConfigServer
  // overlays this on top of its on-disk config so GET /api/config reflects
  // whatever the operator changed via CLI / mobile app since last persist.
  struct LiveSnapshot {
    std::string node_name, owner_info, admin_password, guest_password;
    double node_lat = 0, node_lon = 0;
    float  freq_mhz = 0, bw_khz = 0;
    uint8_t sf = 0, cr = 0;
    int8_t  tx_power_dbm = 0;
    uint8_t advert_interval_min = 0;
    uint8_t flood_advert_interval_h = 0;
    uint8_t flood_max_hops = 0;
    uint8_t duty_cycle_pct = 0;
    int8_t  interference_threshold = 0;
    uint8_t agc_reset_interval = 0;
    float   rx_delay_base = 0, tx_delay_factor = 0, direct_tx_delay_factor = 0;
    uint8_t multi_acks = 0;
    uint8_t loop_detect = 0;       // 0=off 1=minimal 2=moderate 3=strict
    uint8_t path_hash_mode = 0;
    std::string identity_pub_hex, identity_prv_hex;
  };
  void snapshotLivePrefs(LiveSnapshot& out) const;

  // ── mesh::Mesh / Dispatcher hooks (override) ───────────────────────
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }
  int   getInterferenceThreshold() const override { return _prefs.interference_threshold; }
  int   getAGCResetInterval() const override { return _prefs.agc_reset_interval * 4000; }
  uint8_t getExtraAckTransmitCount() const override { return _prefs.multi_acks; }

  int  calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  bool allowPacketForward(const mesh::Packet* packet) override;
  bool filterRecvFloodPacket(mesh::Packet* pkt) override;

  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id,
                    uint32_t timestamp, const uint8_t* app_data,
                    size_t app_data_len) override;

  // Remote-management surface (admin clients reach us over LoRa via these).
  int  searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                      const mesh::Identity& sender, uint8_t* data,
                      size_t len) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                      const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
                      uint8_t* path, uint8_t path_len, uint8_t extra_type,
                      uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

  // Packet log hooks — write timestamped one-line entries to _log_path
  // when _logging is true.
  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;

  // ── CommonCLICallbacks (override) ──────────────────────────────────
  void  savePrefs() override { _cli.savePrefs(_fs); }
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override   { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override        { return FIRMWARE_ROLE; }
  bool  formatFileSystem() override;
  void  sendSelfAdvertisement(int delay_millis, bool flood) override;
  void  updateAdvertTimer() override;
  void  updateFloodAdvertTimer() override;
  void  setLoggingOn(bool enable) override { _logging = enable; }
  void  eraseLogFile() override;
  void  dumpLogFile() override;
  void  setTxPower(int8_t power_dbm) override;
  void  formatNeighborsReply(char* reply) override;
  void  formatStatsReply(char* reply) override;
  void  formatRadioStatsReply(char* reply) override;
  void  formatPacketStatsReply(char* reply) override;
  mesh::LocalIdentity& getSelfId() override { return self_id; }
  void  saveIdentity(const mesh::LocalIdentity& new_id) override;
  void  clearStats() override;

  // Region persistence — `region put NAME` mutates _region_map in memory,
  // then `region save` (or `region default ...`) calls saveRegions(). Without
  // this override CommonCLI's default returns false and the app shows
  // "Err - save failed".
  bool  saveRegions() override;
  void  onDefaultRegionChanged(const RegionEntry* r) override;
  void  applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr,
                             int timeout_mins) override;

  // Periodic tick — call from main loop (drives Dispatcher + advert timers).
  void tick();

private:
  // ── Over-air request handlers (port of MyMesh) ─────────────────────
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret,
                         uint32_t sender_timestamp, const uint8_t* data,
                         bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender,
                               uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq  (const mesh::Identity& sender,
                               uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq  (const mesh::Identity& sender,
                               uint32_t sender_timestamp, const uint8_t* data);
  int  handleRequest(ClientInfo* sender, uint32_t sender_timestamp,
                     uint8_t* payload, size_t payload_len);

  // ── Helpers ────────────────────────────────────────────────────────
  void sendFloodReply  (mesh::Packet* packet, unsigned long delay_millis,
                        uint8_t path_hash_size);
  void sendFloodScoped (const TransportKey& scope, mesh::Packet* pkt,
                        uint32_t delay_millis, uint8_t path_hash_size);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);
  mesh::Packet* createSelfAdvert();
  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  void logTextLine  (const char* prefix, mesh::Packet* pkt, int len, float score);

  // ── State ──────────────────────────────────────────────────────────
  mesh::MainBoard*  _board;
  LinuxTcpRadio*    _tcp_radio;
  FILESYSTEM*       _fs;
  NodePrefs         _prefs;
  ClientACL         acl;
  RegionMap         region_map;
  TransportKeyStore key_store;
  SensorManager     sensors;
  CommonCLI         _cli;

  bool          _logging;
  std::string   _log_path;
  unsigned long _start_millis;          // _ms->getMillis() at bringUp()
  unsigned long next_local_advert;
  unsigned long next_flood_advert;
  unsigned long dirty_contacts_expiry;

  // Reply scratch (one-at-a-time — Mesh::onPeerDataRecv is serialised).
  uint8_t  reply_data[MAX_PACKET_PAYLOAD];
  uint8_t  reply_path[MAX_PATH_SIZE];
  int8_t   reply_path_len;
  uint8_t  reply_path_hash_size;

  int      matching_peer_indexes[MAX_CLIENTS];
  RegionEntry* recv_pkt_region;
  RateLimiter  anon_limiter;
  RateLimiter  discover_limiter;
  uint32_t     pending_discover_tag;
  unsigned long pending_discover_until;

#if MAX_NEIGHBOURS
  NeighbourInfo    neighbours[MAX_NEIGHBOURS];
#endif
};
