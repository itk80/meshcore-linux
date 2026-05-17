#include "LinuxRepeaterMesh.h"
#include <Utils.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace mesh;

// ── Construction ──────────────────────────────────────────────────────

LinuxRepeaterMesh::LinuxRepeaterMesh(MainBoard& board, LinuxTcpRadio& radio,
                                     MillisecondClock& ms, RNG& rng,
                                     RTCClock& rtc, PacketManager& mgr,
                                     MeshTables& tables)
  : Mesh(radio, ms, rng, rtc, mgr, tables),
    _board(&board),
    _tcp_radio(&radio),
    _fs(nullptr),
    acl(),
    region_map(key_store),
    _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
    _logging(false),
    next_local_advert(0),
    next_flood_advert(0)
{
#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // Mirror the ESP32 simple_repeater defaults — see examples/simple_repeater/
  // MyMesh.cpp line ~870 onward. We strip the bridge/sensor/GPS fields
  // because those subsystems aren't present on the Linux build.
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0f;
  _prefs.rx_delay_base = 0.0f;
  _prefs.tx_delay_factor = 0.5f;
  _prefs.direct_tx_delay_factor = 0.3f;
  // Reasonable default name + LoRa profile (matches arduino_base build flags).
  std::snprintf(_prefs.node_name, sizeof(_prefs.node_name), "MeshCore-Linux Repeater");
  _prefs.freq = 869.618f;
  _prefs.bw   = 62.5f;
  _prefs.sf   = 8;
  _prefs.cr   = 8;
  _prefs.tx_power_dbm = 22;
  _prefs.advert_interval = 0;          // DISABLED by default — operator must
                                       // opt in once they've reviewed identity,
                                       // node name, region settings. The
                                       // upstream simple_repeater on ESP32
                                       // also flips this off after first
                                       // manual config via savePrefs.
  _prefs.flood_advert_interval = 0;    // same — disabled until operator opts in
  _prefs.flood_max = 64;
  _prefs.adc_multiplier = 0.0f;
  _prefs.path_hash_mode = 0;
}

// ── bringUp: load/generate IDENTITY first, then persistence + Dispatcher ──
//
// CRITICAL ordering: mesh::Mesh::self_id MUST be a real keypair BEFORE we
// emit anything (advert, ack, reply). Default-constructed LocalIdentity is
// all-zeros pubkey/prvkey — that pollutes other nodes' contact tables and
// corrupts mesh routing. Identity is read from /etc/Meshcore-Linux/config.json
// (the operator never sees this dance after first boot: lazy-generated and
// callback-persisted on the very first start, preserved across reinstalls).

static bool parseHex(const std::string& s, uint8_t* out, size_t expected_len) {
  if (s.size() != expected_len * 2) return false;
  for (size_t i = 0; i < expected_len; i++) {
    int hi = -1, lo = -1;
    char c1 = s[i*2], c2 = s[i*2 + 1];
    if      (c1 >= '0' && c1 <= '9') hi = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
    else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
    if      (c2 >= '0' && c2 <= '9') lo = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
    else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}
static std::string toHex(const uint8_t* in, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s; s.resize(n * 2);
  for (size_t i = 0; i < n; i++) {
    s[i*2]     = d[in[i] >> 4];
    s[i*2 + 1] = d[in[i] & 0x0F];
  }
  return s;
}

void LinuxRepeaterMesh::bringUp(FILESYSTEM& fs,
                                const std::string& identity_pub_hex,
                                const std::string& identity_prv_hex,
                                OnIdentityGenerated on_generated) {
  _fs = &fs;

  // 1. Load identity from config, or generate + persist via callback.
  // LocalIdentity has a (prv_hex, pub_hex) ctor that accepts our hex blobs
  // directly, so we don't need to poke the private prv_key field.
  uint8_t pub_check[PUB_KEY_SIZE];
  bool parsed = !identity_pub_hex.empty() && !identity_prv_hex.empty()
             && identity_pub_hex.size() == PUB_KEY_SIZE * 2
             && identity_prv_hex.size() == PRV_KEY_SIZE * 2
             && parseHex(identity_pub_hex, pub_check, PUB_KEY_SIZE);

  if (parsed) {
    self_id = mesh::LocalIdentity(identity_prv_hex.c_str(), identity_pub_hex.c_str());
    fprintf(stderr, "[repeater] identity loaded from config (pub=%s...)\n",
            identity_pub_hex.substr(0, 16).c_str());
  } else {
    fprintf(stderr, "[repeater] config has no identity — generating fresh keypair\n");
    self_id = mesh::LocalIdentity(getRNG());
    int tries = 0;
    while (tries < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
      self_id = mesh::LocalIdentity(getRNG()); tries++;
    }
    // Serialise identity via the public writeTo(uint8_t*, size_t) — gives us
    // PRV_KEY_SIZE + PUB_KEY_SIZE bytes [prv ‖ pub] in fixed layout.
    uint8_t blob[PRV_KEY_SIZE + PUB_KEY_SIZE];
    self_id.writeTo(blob, sizeof(blob));
    if (on_generated) {
      on_generated(toHex(blob + PRV_KEY_SIZE, PUB_KEY_SIZE),  // pub at offset PRV_KEY_SIZE
                   toHex(blob,                PRV_KEY_SIZE)); // prv at offset 0
    }
    fprintf(stderr, "[repeater] new identity persisted to config; pub=%s...\n",
            toHex(self_id.pub_key, 8).c_str());
  }

  // 2. Load remaining persistence (prefs, ACL, regions).
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  region_map.load(_fs);

  // 3. NOW bring up the radio + dispatcher.
  mesh::Mesh::begin();

  // 4. Schedule recurring adverts ONLY if prefs explicitly enable them.
  //    Operators opt in via CLI ("set advert_interval N") after they've
  //    confirmed identity, node name, region, LoRa params are correct.
  updateAdvertTimer();
  updateFloodAdvertTimer();
  fprintf(stderr, "[repeater] advert_interval=%u (½-min) flood_interval=%uh — "
          "set values >0 to enable beaconing\n",
          (unsigned)_prefs.advert_interval,
          (unsigned)_prefs.flood_advert_interval);
}

void LinuxRepeaterMesh::seedPrefsFromConfig(const std::string& name,
                                            double lat, double lon,
                                            const std::string& admin_password,
                                            const std::string& guest_password) {
  // Only seed empty fields — if loadPrefs already restored a value the
  // operator picked, leave it alone. The config.json is the FIRST-RUN
  // seed; com_prefs is the authoritative live state after that.
  bool dirty = false;
  if (_prefs.node_name[0] == '\0' && !name.empty()) {
    std::snprintf(_prefs.node_name, sizeof(_prefs.node_name), "%s", name.c_str());
    dirty = true;
  }
  if (_prefs.node_lat == 0.0 && lat != 0.0) { _prefs.node_lat = lat; dirty = true; }
  if (_prefs.node_lon == 0.0 && lon != 0.0) { _prefs.node_lon = lon; dirty = true; }
  if (_prefs.password[0] == '\0' && !admin_password.empty()) {
    std::snprintf(_prefs.password, sizeof(_prefs.password), "%s", admin_password.c_str());
    dirty = true;
  }
  if (_prefs.guest_password[0] == '\0' && !guest_password.empty()) {
    std::snprintf(_prefs.guest_password, sizeof(_prefs.guest_password), "%s", guest_password.c_str());
    dirty = true;
  }
  if (dirty) savePrefs();
}

// ── Forwarding / timing hooks ─────────────────────────────────────────

int LinuxRepeaterMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((std::pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t LinuxRepeaterMesh::getRetransmitDelay(const mesh::Packet* packet) {
  uint32_t toa = _radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2);
  return (uint32_t)(toa * _prefs.tx_delay_factor);
}

uint32_t LinuxRepeaterMesh::getDirectRetransmitDelay(const mesh::Packet* packet) {
  uint32_t toa = _radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2);
  return (uint32_t)(toa * _prefs.direct_tx_delay_factor);
}

bool LinuxRepeaterMesh::allowPacketForward(const mesh::Packet* packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->getPathHashCount() >= _prefs.flood_max) return false;
  return true;
}

void LinuxRepeaterMesh::onAdvertRecv(mesh::Packet* /*packet*/, const mesh::Identity& id,
                                     uint32_t timestamp, const uint8_t* /*app_data*/,
                                     size_t /*app_data_len*/) {
  putNeighbour(id, timestamp, _radio->getLastSNR());
}

void LinuxRepeaterMesh::putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS
  int free_slot = -1, oldest = 0;
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    if (neighbours[i].advert_timestamp == 0) { free_slot = i; break; }
    if (memcmp(neighbours[i].id.pub_key, id.pub_key, PUB_KEY_SIZE) == 0) {
      neighbours[i].advert_timestamp = timestamp;
      neighbours[i].heard_timestamp = (uint32_t)getRTCClock()->getCurrentTime();
      neighbours[i].snr = (int8_t)(snr * 4);
      return;
    }
    if (neighbours[i].heard_timestamp < neighbours[oldest].heard_timestamp) oldest = i;
  }
  int slot = free_slot >= 0 ? free_slot : oldest;
  neighbours[slot].id = id;
  neighbours[slot].advert_timestamp = timestamp;
  neighbours[slot].heard_timestamp = (uint32_t)getRTCClock()->getCurrentTime();
  neighbours[slot].snr = (int8_t)(snr * 4);
#endif
}

// ── CommonCLICallbacks ────────────────────────────────────────────────

bool LinuxRepeaterMesh::formatFileSystem() {
  // We don't really format /var/lib — too destructive. Just remove the
  // known persistence blobs; the FS gets re-seeded on the next save.
  if (!_fs) return false;
  _fs->remove("/com_prefs");
  _fs->remove("/identity");
  _fs->remove("/acl");
  _fs->remove("/regions");
  return true;
}

mesh::Packet* LinuxRepeaterMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len;
  app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);
  return mesh::Mesh::createAdvert(self_id, app_data, app_data_len);
}

void LinuxRepeaterMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  // Guard against ever transmitting with a default (all-zeros) keypair.
  // If we somehow get here before bringUp() loaded/generated the identity
  // we'd pollute every neighbour's contact table with a ghost node.
  bool zero_key = true;
  for (size_t i = 0; i < PUB_KEY_SIZE; i++) {
    if (self_id.pub_key[i] != 0) { zero_key = false; break; }
  }
  if (zero_key) {
    fprintf(stderr, "[repeater] REFUSING to advert — self_id pub_key is all zeros "
                    "(identity not yet loaded). Skipping send.\n");
    return;
  }
  mesh::Packet* pkt = createSelfAdvert();
  if (!pkt) return;
  if (flood) pkt->header |= ROUTE_TYPE_FLOOD;
  sendPacket(pkt, 0, delay_millis);
}

void LinuxRepeaterMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) {
    next_local_advert = futureMillis((unsigned long)_prefs.advert_interval * 2UL * 60UL * 1000UL);
  } else {
    next_local_advert = 0;
  }
}
void LinuxRepeaterMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) {
    next_flood_advert = futureMillis((unsigned long)_prefs.flood_advert_interval * 3600UL * 1000UL);
  } else {
    next_flood_advert = 0;
  }
}

void LinuxRepeaterMesh::setTxPower(int8_t power_dbm) {
  _prefs.tx_power_dbm = power_dbm;
  if (_tcp_radio) _tcp_radio->setTxPower(power_dbm);   // pushes fresh SET_CONFIG to modem
}

void LinuxRepeaterMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int /*timeout_mins*/) {
  _prefs.freq = freq;
  _prefs.bw   = bw;
  _prefs.sf   = sf;
  _prefs.cr   = cr;
  if (_tcp_radio) {
    _tcp_radio->setParams(freq, bw, sf, cr);   // hot-pushes SET_CONFIG to modem
  }
  savePrefs();
}

void LinuxRepeaterMesh::formatNeighborsReply(char* reply) {
#if MAX_NEIGHBOURS
  int n_active = 0;
  for (int i = 0; i < MAX_NEIGHBOURS; i++) if (neighbours[i].advert_timestamp) n_active++;
  std::snprintf(reply, 160, "neighbours=%d/%d", n_active, (int)MAX_NEIGHBOURS);
#else
  std::strcpy(reply, "neighbours support disabled");
#endif
}

void LinuxRepeaterMesh::formatStatsReply(char* reply) {
  std::snprintf(reply, 160,
    "uptime_air=%lus  flood_sent=%u/recv=%u  direct_sent=%u/recv=%u",
    (unsigned long)(getTotalAirTime()/1000),
    (unsigned)getNumSentFlood(), (unsigned)getNumRecvFlood(),
    (unsigned)getNumSentDirect(), (unsigned)getNumRecvDirect());
}
void LinuxRepeaterMesh::formatRadioStatsReply(char* reply) {
  std::snprintf(reply, 160, "noise=%ddBm  rssi=%g  snr=%g",
    _radio->getNoiseFloor(), (double)_radio->getLastRSSI(), (double)_radio->getLastSNR());
}
void LinuxRepeaterMesh::formatPacketStatsReply(char* reply) {
  std::snprintf(reply, 160,
    "flood:sent=%u/recv=%u  direct:sent=%u/recv=%u  tx_budget=%lums",
    (unsigned)getNumSentFlood(), (unsigned)getNumRecvFlood(),
    (unsigned)getNumSentDirect(), (unsigned)getNumRecvDirect(),
    (unsigned long)getRemainingTxBudget());
}

void LinuxRepeaterMesh::saveIdentity(const mesh::LocalIdentity& new_id) {
  if (!_fs) return;
  IdentityStore store(*_fs, "/identity");
  store.save("_main", new_id);
  self_id = new_id;
}

void LinuxRepeaterMesh::clearStats() {
  resetStats();
}

// ── CLI bridge ────────────────────────────────────────────────────────

void LinuxRepeaterMesh::processCommand(const char* command, char* reply) {
  // CommonCLI mutates `command` (strchr/strtok-style), so copy.
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", command);
  reply[0] = '\0';
  char* cmd = buf;
  while (*cmd == ' ') cmd++;

  // ── MyMesh-only verbs (those that simple_repeater handles BEFORE the
  //    fallthrough to CommonCLI). Same shape, minus Heltec-specific stuff
  //    (bridges, ESP-NOW, OLED).

  if (std::memcmp(cmd, "setperm ", 8) == 0) {
    // setperm {pubkey-hex} {permissions-int8}
    char* hex = cmd + 8;
    char* sp = std::strchr(hex, ' ');
    if (!sp) { std::strcpy(reply, "Err - usage: setperm <pubkey-hex> <permissions>"); return; }
    *sp++ = '\0';
    uint8_t pubkey[PUB_KEY_SIZE];
    int hex_len = std::strlen(hex);
    if (hex_len > (int)(PUB_KEY_SIZE * 2)) hex_len = PUB_KEY_SIZE * 2;
    if (!mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
      std::strcpy(reply, "Err - bad pubkey hex"); return;
    }
    uint8_t perms = (uint8_t)std::atoi(sp);
    if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
      if (_fs) acl.save(_fs);   // optional filter dropped — save all
      std::strcpy(reply, "OK");
    } else {
      std::strcpy(reply, "Err - invalid params");
    }
    return;
  }
  if (std::strcmp(cmd, "get acl") == 0) {
    char* p = reply;
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;   // skip deleted/guest
      p += std::sprintf(p, "%02X ", c->permissions);
      mesh::Utils::toHex(p, c->id.pub_key, PUB_KEY_SIZE);
      p += PUB_KEY_SIZE * 2;
      *p++ = '\n';
    }
    *p = '\0';
    if (reply[0] == '\0') std::strcpy(reply, "(empty ACL)");
    return;
  }
  if (std::memcmp(cmd, "discover.neighbors", 18) == 0) {
    // Equivalent to MyMesh::sendNodeDiscoverReq — sends a DISCOVER request
    // that asks each neighbour to reply with their own advert. We just
    // schedule a fresh self-advert (flood) which neighbours respond to.
    sendSelfAdvertisement(0, true);
    std::strcpy(reply, "OK - Discover sent");
    return;
  }

  // Fallthrough — common verbs (set/get freq, tx, radio, name, lat, lon,
  // password, advert.interval, flood.advert.interval, flood.max, dutycycle,
  // af, rxdelay, txdelay, direct.txdelay, int.thresh, agc.reset.interval,
  // multi.acks, loop.detect, path.hash.mode, public.key, owner.info,
  // neighbors, stats-core, stats-radio, stats-packets, ver, role, advert,
  // flood.advert, log start/stop/erase, …).

  // Snapshot LoRa-affecting fields BEFORE the CLI runs so we can hot-apply
  // changes to the modem (CommonCLI's `set radio` / `set tx` write into
  // _prefs and reply "reboot to apply" — we make it actually apply now).
  float    pre_freq = _prefs.freq;
  float    pre_bw   = _prefs.bw;
  uint8_t  pre_sf   = _prefs.sf;
  uint8_t  pre_cr   = _prefs.cr;
  int8_t   pre_tx   = _prefs.tx_power_dbm;

  _cli.handleCommand(0, cmd, reply);

  if (_tcp_radio) {
    bool lora_changed = (pre_freq != _prefs.freq) || (pre_bw != _prefs.bw)
                     || (pre_sf   != _prefs.sf)   || (pre_cr != _prefs.cr);
    if (lora_changed) _tcp_radio->setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    if (pre_tx != _prefs.tx_power_dbm) _tcp_radio->setTxPower(_prefs.tx_power_dbm);
  }
}

// ── Periodic tick ─────────────────────────────────────────────────────

void LinuxRepeaterMesh::tick() {
  mesh::Mesh::loop();   // drives Dispatcher (radio.loop / recv / send)

  unsigned long now = _ms->getMillis();
  if (next_local_advert > 0 && millisHasNowPassed(next_local_advert)) {
    sendSelfAdvertisement(0, false);
    updateAdvertTimer();
  }
  if (next_flood_advert > 0 && millisHasNowPassed(next_flood_advert)) {
    sendSelfAdvertisement(0, true);
    updateFloodAdvertTimer();
  }
  (void)now;
}
