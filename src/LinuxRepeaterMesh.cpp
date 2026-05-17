#include "LinuxRepeaterMesh.h"
#include <Utils.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

using namespace mesh;

// Protocol constants — port of MyMesh.cpp #defines so the wire format we
// expose to remote clients (MeshCore mobile/desktop app) is bit-identical.
#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY     300
#endif
#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY             200
#endif
#define CLI_REPLY_DELAY_MILLIS      600
#define FIRMWARE_VER_LEVEL          2

#define REQ_TYPE_GET_STATUS         0x01
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07

#define ANON_REQ_TYPE_REGIONS       0x01
#define ANON_REQ_TYPE_OWNER         0x02
#define ANON_REQ_TYPE_BASIC         0x03

#ifndef RESP_SERVER_LOGIN_OK
  #define RESP_SERVER_LOGIN_OK      0
#endif
#ifndef TXT_TYPE_PLAIN
  #define TXT_TYPE_PLAIN            0
#endif
#ifndef TXT_TYPE_CLI_DATA
  #define TXT_TYPE_CLI_DATA         1
#endif

#define LAZY_CONTACTS_WRITE_DELAY   5000

// CTL message subtypes (high nibble) shared with simple_repeater.
#define CTL_TYPE_NODE_DISCOVER_REQ  0x80
#define CTL_TYPE_NODE_DISCOVER_RESP 0x90

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
    // Packet log lives under StateDirectory so the systemd unit's
    // ProtectSystem=strict ReadWritePaths covers it without a second
    // ReadWritePaths entry. Operators can rotate it with logrotate by path.
    _log_path("/var/lib/Meshcore-Linux/packets.log"),
    next_local_advert(0),
    next_flood_advert(0),
    dirty_contacts_expiry(0),
    reply_path_len(-1),
    reply_path_hash_size(1),
    recv_pkt_region(nullptr),
    // Match MyMesh upstream: max 4 anon requests / 3 min, 4 discover / 2 min.
    anon_limiter(4, 180),
    discover_limiter(4, 120),
    pending_discover_tag(0),
    pending_discover_until(0)
{
  std::memset(matching_peer_indexes, 0, sizeof(matching_peer_indexes));
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
  // Advert must carry lat/lon from _prefs (no GPS sensor on Linux), otherwise
  // mobile/desktop clients show the repeater at (0,0) — the map centres on
  // the Pacific (near New Zealand) instead of where the operator set it.
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;
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
  // Upgrade path — old com_prefs blobs persisted from an earlier build will
  // have advert_loc_policy == 0 (NONE). Without GPS sensor that means adverts
  // never carry the operator-set lat/lon. Flip to PREFS on first boot after
  // upgrade so the mobile map shows the right location.
  if (_prefs.advert_loc_policy == ADVERT_LOC_NONE) {
    _prefs.advert_loc_policy = ADVERT_LOC_PREFS;
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

// Loop-counter maximums per path-hash-size (1/2/3-byte). Ported verbatim from
// MyMesh.cpp — once self-id appears N times in the path, drop the packet to
// keep flood loops from spreading. STRICT is the safest, MINIMAL the loosest.
static const uint8_t MAX_LOOP_MINIMAL[]  = { 0, 4, 2, 1 };
static const uint8_t MAX_LOOP_MODERATE[] = { 0, 2, 1, 1 };
static const uint8_t MAX_LOOP_STRICT[]   = { 0, 1, 1, 1 };

bool LinuxRepeaterMesh::isLooped(const mesh::Packet* packet,
                                 const uint8_t max_counters[]) {
  uint8_t hash_size  = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

bool LinuxRepeaterMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  // Determine the region scope for this incoming flood packet so
  // allowPacketForward() can later decide whether we're allowed to
  // re-flood it (region rules can DENY_FLOOD on a per-transport basis).
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = nullptr;
    } else {
      recv_pkt_region = &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = nullptr;
  }
  return false;   // normal processing continues
}

bool LinuxRepeaterMesh::allowPacketForward(const mesh::Packet* packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood() && packet->getPathHashCount() >= _prefs.flood_max) return false;
  if (packet->isRouteFlood() && recv_pkt_region == nullptr) {
    // No matching region (or wildcard explicitly denies flooding) — drop.
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maxima;
    if      (_prefs.loop_detect == LOOP_DETECT_MINIMAL)  maxima = MAX_LOOP_MINIMAL;
    else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) maxima = MAX_LOOP_MODERATE;
    else                                                 maxima = MAX_LOOP_STRICT;
    if (isLooped(packet, maxima)) {
      fprintf(stderr, "[repeater] flood loop detected, dropping packet\n");
      return false;
    }
  }
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

bool LinuxRepeaterMesh::saveRegions() {
  if (!_fs) return false;
  bool ok = region_map.save(_fs);
  fprintf(stderr, "[repeater] saveRegions() -> %s (%d regions)\n",
          ok ? "OK" : "FAIL", region_map.getCount());
  return ok;
}

void LinuxRepeaterMesh::onDefaultRegionChanged(const RegionEntry* r) {
  // Refresh the default transport scope so outgoing flood replies are
  // tagged correctly. We don't keep a `default_scope` field separately
  // (sendFloodReply derives scope from recv_pkt_region on the wire),
  // so just log for now — region change still takes effect via save().
  fprintf(stderr, "[repeater] default region -> %s\n",
          r ? r->name : "<null>");
}

// ── CLI bridge ────────────────────────────────────────────────────────

void LinuxRepeaterMesh::processCommand(const char* command, char* reply) {
  // CommonCLI mutates `command` (strchr/strtok-style), so copy.
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", command);
  reply[0] = '\0';
  char* cmd = buf;
  while (*cmd == ' ') cmd++;

  // MeshCore mobile/desktop apps prefix admin TXT-CLI commands with
  // "XX|" (2-char hex tag) so they can match responses to requests:
  //   '0e|get lat'  -> reply '0e|> 52.214'
  // Without this, the app fails to correlate replies and times out
  // even though we processed the command. Reflect the prefix verbatim
  // back into the caller's reply buffer, then advance both pointers
  // so the rest of the pipeline (CLI handlers below) write past it.
  if (std::strlen(cmd) > 4 && cmd[2] == '|') {
    std::memcpy(reply, cmd, 3);
    reply[3] = '\0';
    reply += 3;
    cmd   += 3;
  }

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

void LinuxRepeaterMesh::snapshotLivePrefs(LiveSnapshot& out) const {
  out.node_name      = _prefs.node_name;
  out.owner_info     = _prefs.owner_info;
  out.admin_password = _prefs.password;
  out.guest_password = _prefs.guest_password;
  out.node_lat       = _prefs.node_lat;
  out.node_lon       = _prefs.node_lon;
  out.freq_mhz       = _prefs.freq;
  out.bw_khz         = _prefs.bw;
  out.sf             = _prefs.sf;
  out.cr             = _prefs.cr;
  out.tx_power_dbm   = _prefs.tx_power_dbm;
  out.advert_interval_min     = _prefs.advert_interval;
  out.flood_advert_interval_h = _prefs.flood_advert_interval;
  out.flood_max_hops          = _prefs.flood_max;
  // CommonCLI's `set dutycycle N` stores airtime_factor as N/100 (1.0 = 100%).
  out.duty_cycle_pct          = (uint8_t)std::round(_prefs.airtime_factor * 100.0f);
  out.interference_threshold  = _prefs.interference_threshold;
  out.agc_reset_interval      = _prefs.agc_reset_interval;
  out.rx_delay_base           = _prefs.rx_delay_base;
  out.tx_delay_factor         = _prefs.tx_delay_factor;
  out.direct_tx_delay_factor  = _prefs.direct_tx_delay_factor;
  out.multi_acks              = _prefs.multi_acks;
  out.loop_detect             = _prefs.loop_detect;
  out.path_hash_mode          = _prefs.path_hash_mode;
  // Public key only — prv stays in /etc/Meshcore-Linux/config.json (mode 0600
  // ideally) and is never sent over the HTTP overlay. Operators who need to
  // back it up read the config file directly.
  static const char* hex = "0123456789abcdef";
  out.identity_pub_hex.resize(PUB_KEY_SIZE * 2);
  for (size_t i = 0; i < PUB_KEY_SIZE; i++) {
    out.identity_pub_hex[i*2]     = hex[self_id.pub_key[i] >> 4];
    out.identity_pub_hex[i*2 + 1] = hex[self_id.pub_key[i] & 0x0F];
  }
  out.identity_prv_hex.clear();
}

void LinuxRepeaterMesh::tick() {
  mesh::Mesh::loop();   // drives Dispatcher (radio.loop / recv / send)

  if (next_local_advert > 0 && millisHasNowPassed(next_local_advert)) {
    sendSelfAdvertisement(0, false);
    updateAdvertTimer();
  }
  if (next_flood_advert > 0 && millisHasNowPassed(next_flood_advert)) {
    sendSelfAdvertisement(0, true);
    updateFloodAdvertTimer();
  }
  if (dirty_contacts_expiry != 0 && millisHasNowPassed(dirty_contacts_expiry)) {
    if (_fs) acl.save(_fs);
    dirty_contacts_expiry = 0;
  }
}

// ════════════════════════════════════════════════════════════════════════
// Remote management over LoRa — ported from examples/simple_repeater/MyMesh.cpp.
// All wire formats preserved bit-identically so MeshCore mobile/desktop
// clients (which speak this protocol against any repeater) can log in to
// the Linux instance, query stats/neighbours, change ACL etc. over LoRa.
// ════════════════════════════════════════════════════════════════════════

int LinuxRepeaterMesh::searchPeersByHash(const uint8_t* hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i;
      if (n >= MAX_CLIENTS) break;
    }
  }
  return n;
}

void LinuxRepeaterMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    std::memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  }
}

uint8_t LinuxRepeaterMesh::handleLoginReq(const mesh::Identity& sender,
                                          const uint8_t* secret,
                                          uint32_t sender_timestamp,
                                          const uint8_t* data, bool is_flood) {
  ClientInfo* client = nullptr;
  if (data[0] == 0) {   // blank password → keep existing ACL entry, if any
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
  }
  if (client == nullptr) {
    uint8_t perms;
    if (std::strcmp((const char*)data, _prefs.password) == 0) {
      perms = PERM_ACL_ADMIN;
    } else if (std::strcmp((const char*)data, _prefs.guest_password) == 0) {
      perms = PERM_ACL_GUEST;
    } else {
      return 0;   // bad password
    }
    client = acl.putClient(sender, 0);
    if (client == nullptr || sender_timestamp <= client->last_timestamp) {
      // Table full -or- replay attack.
      return 0;
    }
    client->last_timestamp = sender_timestamp;
    client->last_activity  = getRTCClock()->getCurrentTime();
    client->permissions = (client->permissions & ~PERM_ACL_ROLE_MASK) | perms;
    std::memcpy(client->shared_secret, secret, PUB_KEY_SIZE);
    if (perms != PERM_ACL_GUEST) {
      // Persist after a short coalescing delay (multiple logins in a burst
      // shouldn't each cost a file write).
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }
  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;   // force path rediscovery
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  std::memcpy(reply_data, &now, 4);
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;     // legacy: keep-alive interval (deprecated)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // entropy for packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;
  return 13;
}

uint8_t LinuxRepeaterMesh::handleAnonRegionsReq(const mesh::Identity& /*sender*/,
                                                uint32_t sender_timestamp,
                                                const uint8_t* data) {
  if (!anon_limiter.allow(getRTCClock()->getCurrentTime())) {
    fprintf(stderr, "[anon-rx] regions: rate-limited (max 4/3min) — request dropped\n");
    return 0;
  }
  reply_path_len       = *data & 63;
  reply_path_hash_size = (*data >> 6) + 1;
  data++;
  std::memcpy(reply_path, data, (uint8_t)reply_path_len * reply_path_hash_size);

  std::memcpy(reply_data, &sender_timestamp, 4);
  uint32_t now = getRTCClock()->getCurrentTime();
  std::memcpy(&reply_data[4], &now, 4);
  return 8 + region_map.exportNamesTo((char*)&reply_data[8],
                                      sizeof(reply_data) - 12,
                                      REGION_DENY_FLOOD);
}

uint8_t LinuxRepeaterMesh::handleAnonOwnerReq(const mesh::Identity& /*sender*/,
                                              uint32_t sender_timestamp,
                                              const uint8_t* data) {
  if (!anon_limiter.allow(getRTCClock()->getCurrentTime())) {
    fprintf(stderr, "[anon-rx] owner: rate-limited (max 4/3min) — request dropped\n");
    return 0;
  }
  reply_path_len       = *data & 63;
  reply_path_hash_size = (*data >> 6) + 1;
  data++;
  std::memcpy(reply_path, data, (uint8_t)reply_path_len * reply_path_hash_size);

  std::memcpy(reply_data, &sender_timestamp, 4);
  uint32_t now = getRTCClock()->getCurrentTime();
  std::memcpy(&reply_data[4], &now, 4);
  std::snprintf((char*)&reply_data[8], sizeof(reply_data) - 8, "%s\n%s",
                _prefs.node_name, _prefs.owner_info);
  return 8 + (uint8_t)std::strlen((char*)&reply_data[8]);
}

uint8_t LinuxRepeaterMesh::handleAnonClockReq(const mesh::Identity& /*sender*/,
                                              uint32_t sender_timestamp,
                                              const uint8_t* data) {
  if (!anon_limiter.allow(getRTCClock()->getCurrentTime())) {
    fprintf(stderr, "[anon-rx] clock: rate-limited (max 4/3min) — request dropped\n");
    return 0;
  }
  reply_path_len       = *data & 63;
  reply_path_hash_size = (*data >> 6) + 1;
  data++;
  std::memcpy(reply_path, data, (uint8_t)reply_path_len * reply_path_hash_size);

  std::memcpy(reply_data, &sender_timestamp, 4);
  uint32_t now = getRTCClock()->getCurrentTime();
  std::memcpy(&reply_data[4], &now, 4);
  reply_data[8] = 0;                                 // features (no bridges on Linux)
  if (_prefs.disable_fwd) reply_data[8] |= 0x80;     // is disabled
  return 9;
}

int LinuxRepeaterMesh::handleRequest(ClientInfo* sender, uint32_t sender_timestamp,
                                     uint8_t* payload, size_t payload_len) {
  // Reflect sender's timestamp back as a tag — also helps with packet-hash
  // uniqueness on responses sent right back along the same path.
  std::memcpy(reply_data, &sender_timestamp, 4);

  if (payload[0] == REQ_TYPE_GET_STATUS) {
    // Pack the same RepeaterStats struct shape MeshCore clients expect.
    // Fields with no Linux equivalent (battery, MCU temp) are reported as 0.
    struct __attribute__((packed)) RepeaterStats {
      uint16_t batt_milli_volts;
      uint16_t curr_tx_queue_len;
      int16_t  noise_floor;
      int16_t  last_rssi;
      uint32_t n_packets_recv;
      uint32_t n_packets_sent;
      uint32_t total_air_time_secs;
      uint32_t total_up_time_secs;
      uint32_t n_sent_flood, n_sent_direct;
      uint32_t n_recv_flood, n_recv_direct;
      uint16_t err_events;
      int16_t  last_snr;
      uint16_t n_direct_dups, n_flood_dups;
      uint32_t total_rx_air_time_secs;
      uint32_t n_recv_errors;
    } stats{};
    stats.batt_milli_volts = _board ? _board->getBattMilliVolts() : 0;
    stats.curr_tx_queue_len = _mgr ? _mgr->getOutboundTotal() : 0;
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi   = (int16_t)_radio->getLastRSSI();
    stats.n_packets_recv = _tcp_radio ? _tcp_radio->getRxCount() : 0;
    stats.n_packets_sent = _tcp_radio ? _tcp_radio->getTxCount() : 0;
    stats.total_air_time_secs = (uint32_t)(getTotalAirTime() / 1000);
    stats.total_up_time_secs  = (uint32_t)(_ms->getMillis() / 1000);
    stats.n_sent_flood  = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood  = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events    = 0;
    stats.last_snr      = (int16_t)(_radio->getLastSNR() * 4);
    auto* tables = (SimpleMeshTables*)getTables();
    stats.n_direct_dups = tables ? tables->getNumDirectDups() : 0;
    stats.n_flood_dups  = tables ? tables->getNumFloodDups()  : 0;
    stats.total_rx_air_time_secs = (uint32_t)(getReceiveAirTime() / 1000);
    stats.n_recv_errors = _tcp_radio ? _tcp_radio->getCrcErrors() : 0;
    std::memcpy(&reply_data[4], &stats, sizeof(stats));
    return 4 + (int)sizeof(stats);
  }

  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    // Linux has no battery / MCU temp / GPS sensors plumbed in. Return an
    // empty CayenneLPP buffer (just the sender_timestamp tag). MeshCore
    // clients handle a zero-length payload gracefully.
    return 4;
  }

  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    if (payload[1] == 0 && payload[2] == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients()
                      && ofs + 7 <= (uint8_t)(sizeof(reply_data) - 4); i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;
        std::memcpy(&reply_data[ofs], c->id.pub_key, 6);
        ofs += 6;
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }

  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    if (payload[1] != 0) return 0;   // request version
    int reply_offset = 4;
    uint8_t  count               = payload[2];
    uint16_t offset;              std::memcpy(&offset, &payload[3], 2);
    uint8_t  order_by            = payload[5];
    uint8_t  pubkey_prefix_length = payload[6];
    if (pubkey_prefix_length > PUB_KEY_SIZE) pubkey_prefix_length = PUB_KEY_SIZE;

    int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
    NeighbourInfo* sorted[MAX_NEIGHBOURS];
    for (int i = 0; i < MAX_NEIGHBOURS; i++) {
      if (neighbours[i].heard_timestamp > 0) sorted[neighbours_count++] = &neighbours[i];
    }
    auto cmp_newest   = [](const NeighbourInfo* a, const NeighbourInfo* b)
                          { return a->heard_timestamp > b->heard_timestamp; };
    auto cmp_oldest   = [](const NeighbourInfo* a, const NeighbourInfo* b)
                          { return a->heard_timestamp < b->heard_timestamp; };
    auto cmp_stronger = [](const NeighbourInfo* a, const NeighbourInfo* b)
                          { return a->snr > b->snr; };
    auto cmp_weaker   = [](const NeighbourInfo* a, const NeighbourInfo* b)
                          { return a->snr < b->snr; };
    if      (order_by == 0) std::sort(sorted, sorted + neighbours_count, cmp_newest);
    else if (order_by == 1) std::sort(sorted, sorted + neighbours_count, cmp_oldest);
    else if (order_by == 2) std::sort(sorted, sorted + neighbours_count, cmp_stronger);
    else if (order_by == 3) std::sort(sorted, sorted + neighbours_count, cmp_weaker);
#endif

    int results_count = 0, results_offset = 0;
    uint8_t results_buffer[130];
    for (int index = 0; index < count && index + offset < neighbours_count; index++) {
      int entry_size = pubkey_prefix_length + 4 + 1;
      if (results_offset + entry_size > (int)sizeof(results_buffer)) break;
#if MAX_NEIGHBOURS
      auto n = sorted[index + offset];
      uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - n->heard_timestamp;
      std::memcpy(&results_buffer[results_offset], n->id.pub_key, pubkey_prefix_length);
      results_offset += pubkey_prefix_length;
      std::memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4);
      results_offset += 4;
      std::memcpy(&results_buffer[results_offset], &n->snr, 1);
      results_offset += 1;
      results_count++;
#endif
    }
    std::memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
    std::memcpy(&reply_data[reply_offset], &results_count,    2); reply_offset += 2;
    std::memcpy(&reply_data[reply_offset], results_buffer, results_offset);
    reply_offset += results_offset;
    return reply_offset;
  }

  if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    std::snprintf((char*)&reply_data[4], sizeof(reply_data) - 4, "%s\n%s\n%s",
                  FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + (int)std::strlen((char*)&reply_data[4]);
  }

  return 0;   // unknown
}

void LinuxRepeaterMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt,
                                        uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void LinuxRepeaterMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis,
                                       uint8_t path_hash_size) {
  if (recv_pkt_region && !recv_pkt_region->isWildcard()) {
    TransportKey scope;
    if (region_map.getTransportKeysFor(*recv_pkt_region, &scope, 1) > 0) {
      sendFloodScoped(scope, packet, delay_millis, path_hash_size);
      return;
    }
  }
  sendFlood(packet, delay_millis, path_hash_size);
}

void LinuxRepeaterMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                                       const mesh::Identity& sender,
                                       uint8_t* data, size_t len) {
  if (packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ) return;

  uint32_t timestamp;
  std::memcpy(&timestamp, data, 4);
  data[len] = 0;   // ensure null terminator for string compares

  // Diagnostic — see exactly which ANON_REQ subtype the app is sending and
  // whether it's flood or direct. Helps debug "timeout from app" reports.
  fprintf(stderr, "[anon-rx] from=%02X%02X%02X%02X len=%zu route=%s subtype=0x%02X "
                  "first='%c'(%d)\n",
          sender.pub_key[0], sender.pub_key[1], sender.pub_key[2], sender.pub_key[3],
          len, packet->isRouteFlood() ? "F" : (packet->isRouteDirect() ? "D" : "?"),
          (unsigned)data[4],
          (data[4] >= ' ' && data[4] < 127) ? (char)data[4] : '.', (int)data[4]);

  reply_path_len = -1;
  uint8_t reply_len = 0;
  const char* handled_as = "UNHANDLED";
  if (data[4] == 0 || data[4] >= ' ') {
    handled_as = "LOGIN";
    reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
  } else if (data[4] == ANON_REQ_TYPE_REGIONS) {
    handled_as = "REGIONS";
    reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
  } else if (data[4] == ANON_REQ_TYPE_OWNER) {
    handled_as = "OWNER";
    reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
  } else if (data[4] == ANON_REQ_TYPE_BASIC) {
    handled_as = "BASIC";
    reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
  }
  fprintf(stderr, "[anon-rx] handler=%s reply_len=%u reply_path_len=%d\n",
          handled_as, reply_len, (int)reply_path_len);
  if (reply_len == 0) return;

  if (packet->isRouteFlood()) {
    mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                          PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
    if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
  } else if (reply_path_len < 0) {
    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret,
                                         reply_data, reply_len);
    if (reply) sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
  } else {
    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret,
                                         reply_data, reply_len);
    uint8_t path_len = ((reply_path_hash_size - 1) << 6) | (reply_path_len & 63);
    if (reply) sendDirect(reply, reply_path, path_len, SERVER_RESPONSE_DELAY);
  }
}

void LinuxRepeaterMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                                       const uint8_t* secret, uint8_t* data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) return;
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) {
    uint32_t timestamp;
    std::memcpy(&timestamp, data, 4);
    if (timestamp <= client->last_timestamp) return;   // replay

    int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
    if (reply_len == 0) return;

    client->last_timestamp = timestamp;
    client->last_activity  = getRTCClock()->getCurrentTime();

    if (packet->isRouteFlood()) {
      mesh::Packet* path = createPathReturn(client->id, secret, packet->path,
                                            packet->path_len, PAYLOAD_TYPE_RESPONSE,
                                            reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret,
                                           reply_data, reply_len);
      if (reply) {
        if (client->out_path_len != OUT_PATH_UNKNOWN) {
          sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
        } else {
          sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
        }
      }
    }
    return;
  }

  if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) {
    // Admin client sent a CLI command over LoRa.
    uint32_t sender_timestamp;
    std::memcpy(&sender_timestamp, data, 4);
    uint8_t flags = (data[4] >> 2);
    // Log raw incoming so we can debug what the mobile app sends.
    data[len] = 0;
    fprintf(stderr, "[peer-txt] from=%02X%02X%02X%02X len=%zu flags=%u cmd='%s'\n",
            client->id.pub_key[0], client->id.pub_key[1],
            client->id.pub_key[2], client->id.pub_key[3],
            len, flags, (const char*)&data[5]);
    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) return;
    if (sender_timestamp < client->last_timestamp) return;   // replay
    bool is_retry = (sender_timestamp == client->last_timestamp);
    client->last_timestamp = sender_timestamp;
    client->last_activity  = getRTCClock()->getCurrentTime();
    data[len] = 0;

    if (flags == TXT_TYPE_PLAIN) {
      // Legacy CLI: send an Ack so the client doesn't retransmit.
      uint32_t ack_hash;
      mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data,
                          5 + std::strlen((char*)&data[5]),
                          client->id.pub_key, PUB_KEY_SIZE);
      mesh::Packet* ack = createAck(ack_hash);
      if (ack) {
        if (client->out_path_len == OUT_PATH_UNKNOWN) {
          sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
        } else {
          sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
        }
      }
    }

    uint8_t temp[166];
    char* command = (char*)&data[5];
    char* reply   = (char*)&temp[5];
    if (is_retry) {
      *reply = 0;
    } else {
      processCommand(command, reply);
    }
    int text_len = (int)std::strlen(reply);
    if (text_len > 0) {
      uint32_t ts = getRTCClock()->getCurrentTimeUnique();
      if (ts == sender_timestamp) ts++;   // ensure distinct timestamps
      std::memcpy(temp, &ts, 4);
      temp[4] = (TXT_TYPE_CLI_DATA << 2);
      mesh::Packet* rep = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret,
                                         temp, 5 + text_len);
      if (rep) {
        if (client->out_path_len == OUT_PATH_UNKNOWN) {
          sendFloodReply(rep, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
        } else {
          sendDirect(rep, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
        }
      }
    }
  }
}

bool LinuxRepeaterMesh::onPeerPathRecv(mesh::Packet* /*packet*/, int sender_idx,
                                       const uint8_t* /*secret*/, uint8_t* path,
                                       uint8_t path_len, uint8_t /*extra_type*/,
                                       uint8_t* /*extra*/, uint8_t /*extra_len*/) {
  int i = matching_peer_indexes[sender_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    auto client = acl.getClientByIdx(i);
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  }
  return false;   // we do NOT send a reciprocal path back
}

void LinuxRepeaterMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd
      && discover_limiter.allow(getRTCClock()->getCurrentTime())) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;   std::memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since = 0;
    if (packet->payload_len >= (size_t)(i + 4)) std::memcpy(&since, &packet->payload[i], 4);

    if ((filter & (1 << ADV_TYPE_REPEATER))
        && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;
      data[1] = packet->_snr;
      std::memcpy(&data[2], &tag, 4);
      std::memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) sendZeroHop(resp, getRetransmitDelay(resp) * 4);
    }
    return;
  }
  if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) return;
    if (packet->payload_len < 6 + PUB_KEY_SIZE) return;
    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    std::memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) return;

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) return;
    putNeighbour(id, getRTCClock()->getCurrentTime(), packet->getSNR());
  }
}

// ════════════════════════════════════════════════════════════════════════
// Packet log — append-mode plain text in /var/log/Meshcore-Linux/packets.log.
// ════════════════════════════════════════════════════════════════════════

static void ensureDirFor(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return;
  std::string dir = path.substr(0, slash);
  // mkdir -p, ignore EEXIST silently.
  ::mkdir(dir.c_str(), 0755);
}

void LinuxRepeaterMesh::eraseLogFile() {
  std::remove(_log_path.c_str());
}

void LinuxRepeaterMesh::dumpLogFile() {
  // CommonCLI invokes this when `log dump` runs from CLI; on MeshCore the
  // upstream implementation streams the file over the Serial CLI. We don't
  // have a direct way to stream into the caller's reply buffer here (the
  // CommonCLI dump uses its own line writer), so log to stderr — operators
  // can `journalctl -u Meshcore-Linux` to read.
  FILE* f = std::fopen(_log_path.c_str(), "r");
  if (!f) {
    std::fprintf(stderr, "[repeater] dumpLogFile: cannot open %s\n", _log_path.c_str());
    return;
  }
  char line[256];
  std::fprintf(stderr, "[repeater] ── %s dump begin ──\n", _log_path.c_str());
  while (std::fgets(line, sizeof(line), f)) std::fputs(line, stderr);
  std::fprintf(stderr, "[repeater] ── %s dump end ──\n", _log_path.c_str());
  std::fclose(f);
}

void LinuxRepeaterMesh::logTextLine(const char* prefix, mesh::Packet* pkt, int len,
                                    float score) {
  if (!_logging || _log_path.empty()) return;
  ensureDirFor(_log_path);
  FILE* f = std::fopen(_log_path.c_str(), "a");
  if (!f) return;
  time_t now = std::time(nullptr);
  char tbuf[32];
  std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  std::fprintf(f, "%s %s len=%d type=%d route=%s payload_len=%d",
               tbuf, prefix, len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
  if (score >= 0) {
    std::fprintf(f, " snr=%d rssi=%d score=%d",
                 (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(),
                 (int)(score * 1000));
  }
  uint8_t pt = pkt->getPayloadType();
  if (pt == PAYLOAD_TYPE_PATH || pt == PAYLOAD_TYPE_REQ
      || pt == PAYLOAD_TYPE_RESPONSE || pt == PAYLOAD_TYPE_TXT_MSG) {
    std::fprintf(f, " [%02X->%02X]", pkt->payload[1], pkt->payload[0]);
  }
  std::fputc('\n', f);
  std::fclose(f);
}

void LinuxRepeaterMesh::logRx(mesh::Packet* pkt, int len, float score) {
  logTextLine("RX", pkt, len, score);
}
void LinuxRepeaterMesh::logTx(mesh::Packet* pkt, int len) {
  logTextLine("TX", pkt, len, -1.0f);
}
void LinuxRepeaterMesh::logTxFail(mesh::Packet* pkt, int len) {
  logTextLine("TX-FAIL", pkt, len, -1.0f);
}
