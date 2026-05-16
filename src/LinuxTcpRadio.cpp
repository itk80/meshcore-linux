#include "LinuxTcpRadio.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace pymc_proto;

LinuxTcpRadio::LinuxTcpRadio(const char* host, uint16_t port)
  : _port(port),
    _token_len(0),
    _authenticated(false),
    _rx_started(false),
    _config_acked(false),
    _auto_cad_acked(false),
    _last_reconnect_ms(0),
    _reconnect_count(0),
    _reconnect_backoff_step(0),
    _last_ping_sent_ms(0),
    _last_frame_recv_ms(0),
    _heartbeat_interval_ms(5000),
    _watchdog_timeout_ms(30000),
    _rx_head(0),
    _rx_tail(0),
    _tx_state(TX_IDLE),
    _tx_started_ms(0),
    _last_tx_airtime_us(0),
    _last_rssi(0.0f),
    _last_snr(0.0f),
    _noise_floor_dbm(0),
    _freq_mhz(0.0f),
    _bw_khz(0.0f),
    _sf(0),
    _cr(0),
    _power_dbm(0),
    _syncword(0),
    _preamble_len(0),
    _n_rx(0),
    _n_tx(0),
    _n_crc_err(0),
    _n_pong(0)
{
  if (host) {
    strncpy(_host, host, sizeof(_host) - 1);
    _host[sizeof(_host) - 1] = '\0';
  } else {
    _host[0] = '\0';
  }
  memset(_token, 0, sizeof(_token));
  memset(_rx_ring, 0, sizeof(_rx_ring));
}

// ─── Pre-begin configuration ───────────────────────────────────────────

void LinuxTcpRadio::setAuthToken(const uint8_t* token, size_t len) {
  if (len > sizeof(_token)) len = sizeof(_token);
  if (token && len > 0) { memcpy(_token, token, len); _token_len = (uint8_t)len; }
  else                  { _token_len = 0; }
}

void LinuxTcpRadio::setLoRaParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr,
                                  int8_t power_dbm, uint16_t syncword, uint8_t preamble_len) {
  _freq_mhz = freq_mhz; _bw_khz = bw_khz; _sf = sf; _cr = cr;
  _power_dbm = power_dbm; _syncword = syncword; _preamble_len = preamble_len;
}

// ─── mesh::Radio interface ─────────────────────────────────────────────

void LinuxTcpRadio::begin() {
  connectAndHandshake();
  _last_frame_recv_ms = millis();
}

int LinuxTcpRadio::recvRaw(uint8_t* bytes, int sz) {
  int out_len = 0;
  if (ringPop(bytes, sz, out_len)) return out_len;
  return 0;
}

uint32_t LinuxTcpRadio::getEstAirtimeFor(int len_bytes) {
  return estimate_airtime_ms(len_bytes, _sf, (uint32_t)(_bw_khz * 1000.0f),
                             _cr, _preamble_len);
}

float LinuxTcpRadio::packetScore(float snr, int packet_len) {
  return packet_score(snr, _sf, packet_len);
}

bool LinuxTcpRadio::startSendRaw(const uint8_t* bytes, int len) {
  if (len <= 0 || len > (int)MAX_LORA_PAYLOAD) return false;
  if (_tx_state == TX_PENDING) return false;
  if (!_client.connected() || !_rx_started) return false;
  if (!sendFrame(CMD_TX_REQUEST, bytes, (uint16_t)len)) return false;
  _tx_state = TX_PENDING;
  _tx_started_ms = millis();
  return true;
}

bool LinuxTcpRadio::isSendComplete() {
  return _tx_state == TX_DONE || _tx_state == TX_FAIL;
}

void LinuxTcpRadio::onSendFinished() {
  _tx_state = TX_IDLE;
}

void LinuxTcpRadio::loop() {
  uint32_t now = millis();
  pumpRx();

  if (_client.connected()) {
    if ((int32_t)(now - _last_ping_sent_ms) >= (int32_t)_heartbeat_interval_ms) {
      sendFrame(CMD_PING, nullptr, 0);
      _last_ping_sent_ms = now;
    }
    if ((int32_t)(now - _last_frame_recv_ms) > (int32_t)_watchdog_timeout_ms) {
      Serial.printf("[LinuxTcpRadio] watchdog: no frame for %lums, reconnect\n",
                    (unsigned long)(now - _last_frame_recv_ms));
      _client.stop();
      onDisconnect();
    }
  } else {
    uint32_t step = _reconnect_backoff_step;
    if (step > 4) step = 4;
    uint32_t interval = (uint32_t)5000 << step;
    if (interval > 60000) interval = 60000;
    if ((int32_t)(now - _last_reconnect_ms) >= (int32_t)interval) {
      connectAndHandshake();
    }
  }
}

int  LinuxTcpRadio::getNoiseFloor() const { return _noise_floor_dbm; }
void LinuxTcpRadio::triggerNoiseFloorCalibrate(int /*threshold*/) {
  sendFrame(CMD_NOISE_REQ, nullptr, 0);
}
void LinuxTcpRadio::resetAGC() { /* modem owns AGC */ }

bool LinuxTcpRadio::isInRecvMode() const {
  return _authenticated && _rx_started &&
         const_cast<WiFiClient&>(_client).connected();
}

bool LinuxTcpRadio::isReceiving() { return false; }
float LinuxTcpRadio::getLastRSSI() const { return _last_rssi; }
float LinuxTcpRadio::getLastSNR()  const { return _last_snr; }

// ─── Diagnostics ───────────────────────────────────────────────────────

bool LinuxTcpRadio::isConnected() const {
  return const_cast<WiFiClient&>(_client).connected();
}

void LinuxTcpRadio::forceReconnect() {
  _client.stop();
  onDisconnect();
}

uint32_t LinuxTcpRadio::getRngSeed() { return esp_random(); }

void LinuxTcpRadio::setEndpoint(const char* host, uint16_t port) {
  if (!host || host[0] == '\0' || port == 0) return;
  if (strncmp(_host, host, sizeof(_host)) == 0 && _port == port) return;
  strncpy(_host, host, sizeof(_host) - 1);
  _host[sizeof(_host) - 1] = '\0';
  _port = port;
  Serial.printf("[LinuxTcpRadio] endpoint changed to %s:%u — forcing reconnect\n",
                _host, (unsigned)_port);
  forceReconnect();
}

// ─── Frame I/O ─────────────────────────────────────────────────────────

bool LinuxTcpRadio::sendFrame(uint8_t cmd, const uint8_t* payload, uint16_t len) {
  if (!_client.connected()) return false;
  static uint8_t out[MAX_FRAME_SIZE];
  size_t n = build_frame(cmd, payload, len, out);
  if (n == 0) return false;
  return _client.write(out, n) == n;
}

void LinuxTcpRadio::pumpRx() {
  while (_client.connected()) {
    int v = _client.read();
    if (v < 0) return;
    uint8_t b = (uint8_t)v;
    if (_parser.feed(b)) {
      _last_frame_recv_ms = millis();
      handleEvent(_parser.cmd, _parser.payload, _parser.payload_len);
      _parser.reset();
    } else if (_parser.crc_failed) {
      _n_crc_err++;
    }
  }
}

void LinuxTcpRadio::handleEvent(uint8_t cmd, const uint8_t* payload, uint16_t len) {
  switch (cmd) {
    case EVT_PONG:
      _n_pong++;
      break;

    case EVT_RX_PACKET: {
      RxPacketMeta meta;
      const uint8_t* data = nullptr;
      int dlen = parse_rx_packet(payload, len, meta, data);
      if (dlen < 0) break;
      if (dlen > (int)MAX_LORA_PAYLOAD) dlen = MAX_LORA_PAYLOAD;
      ringPush(meta, data, (uint8_t)dlen);
      _n_rx++;
      break;
    }

    case EVT_TX_DONE:
      parse_tx_done(payload, len, _last_tx_airtime_us);
      _tx_state = TX_DONE;
      _n_tx++;
      break;

    case EVT_TX_FAIL:
      _tx_state = TX_FAIL;
      Serial.println("[LinuxTcpRadio] EVT_TX_FAIL");
      break;

    case EVT_NOISE_RESP: {
      int n = 0;
      if (parse_noise_resp(payload, len, n)) _noise_floor_dbm = n;
      break;
    }

    case EVT_LOG_MSG:
      if (len >= 1) {
        Serial.printf("[modem lvl=%u] ", (unsigned)payload[0]);
        Serial.write(payload + 1, len - 1);
        Serial.println();
      }
      break;

    case EVT_ERROR:
      if (len >= 1) Serial.printf("[LinuxTcpRadio] EVT_ERROR code=0x%02X\n", payload[0]);
      // Any error while a TX is in flight finishes it (e.g. ERR_CHANNEL_BUSY
      // after auto-CAD exhaustion) so the Dispatcher unblocks immediately.
      if (_tx_state == TX_PENDING) _tx_state = TX_FAIL;
      break;

    case EVT_CONFIG_RESP:
      _config_acked = true;
      break;

    case EVT_SET_AUTO_CAD_RESP:
      if (len >= 1 && payload[0] == 0) _auto_cad_acked = true;
      break;

    case EVT_RX_STARTED:
      _rx_started = true;
      break;

    case EVT_AUTH_OK:
      _authenticated = true;
      break;

    default:
      break;
  }
}

// ─── Connection helpers ────────────────────────────────────────────────

bool LinuxTcpRadio::tcpConnect() {
  _last_reconnect_ms = millis();
  if (_host[0] == '\0' || _port == 0) return false;
  if (_client.connected()) return true;
  _client.stop();
  _parser.reset();
  bool ok = _client.connect(_host, _port);
  if (ok) {
    _client.setNoDelay(true);
    _reconnect_count++;
    _last_frame_recv_ms = millis();
    Serial.printf("[LinuxTcpRadio] connected to %s:%u (attempt #%lu)\n",
                  _host, (unsigned)_port, (unsigned long)_reconnect_count);
  } else {
    Serial.printf("[LinuxTcpRadio] connect to %s:%u failed\n", _host, (unsigned)_port);
  }
  return ok;
}

bool LinuxTcpRadio::sendAuth() {
  if (_token_len == 0) return false;
  return sendFrame(CMD_AUTH, _token, _token_len);
}

bool LinuxTcpRadio::sendConfig() {
  RadioConfig cfg{};
  cfg.freq_hz      = (uint32_t)(_freq_mhz * 1e6f);
  cfg.bandwidth_hz = (uint32_t)(_bw_khz * 1000.0f);
  cfg.sf           = _sf;
  cfg.cr           = _cr;
  cfg.power_dbm    = _power_dbm;
  cfg.syncword     = _syncword;
  cfg.preamble_len = _preamble_len;
  uint8_t wire[14];
  serialize_radio_config(cfg, wire);
  return sendFrame(CMD_SET_CONFIG, wire, sizeof(wire));
}

bool LinuxTcpRadio::sendAutoCad(bool enable) {
  uint8_t v = enable ? 1 : 0;
  return sendFrame(CMD_SET_AUTO_CAD, &v, 1);
}

bool LinuxTcpRadio::sendRxStart() {
  return sendFrame(CMD_RX_START, nullptr, 0);
}

void LinuxTcpRadio::onDisconnect() {
  _authenticated = _rx_started = _config_acked = _auto_cad_acked = false;
  _tx_state = TX_IDLE;
  _parser.reset();
}

bool LinuxTcpRadio::awaitFlag(bool& flag, uint32_t timeout_ms) {
  uint32_t deadline = millis() + timeout_ms;
  while ((int32_t)(deadline - millis()) > 0) {
    pumpRx();
    if (flag) return true;
    delay(2);
  }
  return flag;
}

bool LinuxTcpRadio::connectAndHandshake() {
  if (!tcpConnect()) {
    if (_reconnect_backoff_step < 4) _reconnect_backoff_step++;
    return false;
  }

  if (_token_len > 0) {
    _authenticated = false;
    if (!sendAuth() || !awaitFlag(_authenticated, 2000)) {
      Serial.println("[LinuxTcpRadio] auth failed or timeout");
      _client.stop(); onDisconnect();
      if (_reconnect_backoff_step < 4) _reconnect_backoff_step++;
      return false;
    }
  } else {
    _authenticated = true;
  }

  _config_acked = false;
  if (!sendConfig() || !awaitFlag(_config_acked, 2000)) {
    Serial.println("[LinuxTcpRadio] SET_CONFIG ack timeout");
    _client.stop(); onDisconnect();
    if (_reconnect_backoff_step < 4) _reconnect_backoff_step++;
    return false;
  }

  _auto_cad_acked = false;
  if (!sendAutoCad(true) || !awaitFlag(_auto_cad_acked, 1000)) {
    Serial.println("[LinuxTcpRadio] AUTO_CAD not acked (v0.6 modem?), continuing");
  }

  _rx_started = false;
  if (!sendRxStart() || !awaitFlag(_rx_started, 2000)) {
    Serial.println("[LinuxTcpRadio] RX_START ack timeout");
    _client.stop(); onDisconnect();
    if (_reconnect_backoff_step < 4) _reconnect_backoff_step++;
    return false;
  }

  Serial.printf("[LinuxTcpRadio] handshake complete: auth=%d cfg=%d auto_cad=%d rx=%d\n",
                _authenticated, _config_acked, _auto_cad_acked, _rx_started);
  _reconnect_backoff_step = 0;
  return true;
}

// ─── RX ring ───────────────────────────────────────────────────────────

bool LinuxTcpRadio::ringPush(const pymc_proto::RxPacketMeta& meta,
                             const uint8_t* data, uint8_t data_len) {
  uint8_t next = (uint8_t)((_rx_head + 1) % RX_RING);
  if (next == _rx_tail) {
    // Full — drop oldest.
    _rx_tail = (uint8_t)((_rx_tail + 1) % RX_RING);
  }
  RxFrame& f = _rx_ring[_rx_head];
  f.len = data_len;
  f.rssi_dbm = meta.rssi_dbm;
  f.snr_x10 = meta.snr_x10;
  f.sig_rssi_dbm = meta.sig_rssi_dbm;
  f.recv_ms = millis();
  if (data && data_len) memcpy(f.data, data, data_len);
  _rx_head = next;
  return true;
}

bool LinuxTcpRadio::ringPop(uint8_t* dst, int max_len, int& out_len) {
  if (_rx_head == _rx_tail) { out_len = 0; return false; }
  RxFrame& f = _rx_ring[_rx_tail];
  int n = f.len;
  if (n > max_len) n = max_len;
  if (dst && n > 0) memcpy(dst, f.data, n);
  _last_rssi = (float)f.rssi_dbm;
  _last_snr  = (float)f.snr_x10 / 10.0f;
  _rx_tail = (uint8_t)((_rx_tail + 1) % RX_RING);
  out_len = n;
  return true;
}
