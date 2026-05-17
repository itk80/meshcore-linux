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
// CLI commands are bridged via processCommand(cmd, reply) — meant to be
// called from the HTTP /api/command endpoint (and could be wired to stdin
// or a unix-socket fronted CLI in a follow-up).

#include <Mesh.h>
#include <functional>
#include <string>
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
  LinuxRepeaterMesh(mesh::MainBoard& board, mesh::Radio& radio,
                    mesh::MillisecondClock& ms, mesh::RNG& rng,
                    mesh::RTCClock& rtc, mesh::PacketManager& mgr,
                    mesh::MeshTables& tables);

  // Bring up persistence + run mesh::Mesh::begin (which kicks Dispatcher).
  // `identity_pub_hex` / `identity_prv_hex` come from /etc/Meshcore-Linux/
  // config.json. If either is empty the function generates a fresh keypair
  // via the host RNG and calls `onIdentityGenerated(pubhex, prvhex)` so the
  // caller can persist it back to the config file. NEVER returns / sends
  // anything with a zero key.
  using OnIdentityGenerated = std::function<void(const std::string& pub_hex,
                                                  const std::string& prv_hex)>;
  void bringUp(FILESYSTEM& fs,
               const std::string& identity_pub_hex,
               const std::string& identity_prv_hex,
               OnIdentityGenerated on_generated);

  // Bridge CLI command (called from HTTP /api/command). Returns nothing —
  // reply text goes into `reply` (caller must provide at least 160 B).
  void processCommand(const char* command, char* reply);

  NodePrefs* getNodePrefs() { return &_prefs; }

  // ── mesh::Mesh / Dispatcher hooks (override) ───────────────────────
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }
  int   getInterferenceThreshold() const override { return _prefs.interference_threshold; }
  int   getAGCResetInterval() const override { return _prefs.agc_reset_interval * 4000; }
  uint8_t getExtraAckTransmitCount() const override { return _prefs.multi_acks; }

  int  calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  bool allowPacketForward(const mesh::Packet* packet) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id,
                    uint32_t timestamp, const uint8_t* app_data,
                    size_t app_data_len) override;

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
  void  eraseLogFile() override {}
  void  dumpLogFile() override {}
  void  setTxPower(int8_t power_dbm) override;
  void  formatNeighborsReply(char* reply) override;
  void  formatStatsReply(char* reply) override;
  void  formatRadioStatsReply(char* reply) override;
  void  formatPacketStatsReply(char* reply) override;
  mesh::LocalIdentity& getSelfId() override { return self_id; }
  void  saveIdentity(const mesh::LocalIdentity& new_id) override;
  void  clearStats() override;
  void  applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr,
                             int timeout_mins) override;

  // ── Periodic tick — call from main loop, handles advert timers etc. ─
  void tick();

private:
  mesh::MainBoard* _board;
  FILESYSTEM*      _fs;
  NodePrefs        _prefs;
  ClientACL        acl;
  RegionMap        region_map;
  TransportKeyStore key_store;
  SensorManager    sensors;
  CommonCLI        _cli;

  bool             _logging;
  unsigned long    next_local_advert;
  unsigned long    next_flood_advert;

#if MAX_NEIGHBOURS
  NeighbourInfo    neighbours[MAX_NEIGHBOURS];
#endif

  mesh::Packet* createSelfAdvert();
  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
};
