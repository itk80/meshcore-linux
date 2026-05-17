#pragma once

// LinuxTcpRadio — mesh::Radio implementation that tunnels every Radio call
// through the pymc_usb wire protocol v0.7 over a TCP socket. Linux-side
// counterpart to the ESP32 CustomTCPRadioWrapper. Same protocol, same state
// machines, different transport (POSIX socket vs Arduino WiFiClient).

#include "../shims/PosixArduinoCompat.h"   // WiFiClient + millis/delay/Serial/esp_random
#include "helpers/tcpradio/pymc_proto.h"   // shared protocol/framing (lives in MeshCore)

#include <Dispatcher.h>                    // mesh::Radio base

class LinuxTcpRadio : public mesh::Radio {
public:
  LinuxTcpRadio(const char* host, uint16_t port);

  // Pre-begin configuration.
  void setAuthToken(const uint8_t* token, size_t len);
  void setLoRaParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr,
                     int8_t power_dbm, uint16_t syncword, uint8_t preamble_len);

  // Runtime LoRa reconfig (subset, used by CLI verbs `set freq/bw/sf/cr`).
  // Updates the cached params and immediately re-sends SET_CONFIG to the
  // modem if connected.
  void setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
  void setTxPower(int8_t power_dbm);

  // mesh::Radio interface.
  void     begin() override;
  int      recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  float    packetScore(float snr, int packet_len) override;
  bool     startSendRaw(const uint8_t* bytes, int len) override;
  bool     isSendComplete() override;
  void     onSendFinished() override;
  void     loop() override;
  int      getNoiseFloor() const override;
  void     triggerNoiseFloorCalibrate(int threshold) override;
  void     resetAGC() override;
  bool     isInRecvMode() const override;
  bool     isReceiving() override;
  float    getLastRSSI() const override;
  float    getLastSNR() const override;

  // Diagnostics.
  bool     isConnected() const;
  bool     isHandshakeComplete() const { return _authenticated && _rx_started; }
  uint32_t getReconnectCount() const   { return _reconnect_count; }
  uint32_t getRxCount() const          { return _n_rx; }
  uint32_t getTxCount() const          { return _n_tx; }
  uint32_t getPongCount() const        { return _n_pong; }
  uint32_t getCrcErrors() const        { return _n_crc_err; }
  uint32_t getRngSeed();
  const char* getModemHost() const     { return _host; }
  uint16_t getModemPort() const        { return _port; }
  void     forceReconnect();
  void     setEndpoint(const char* host, uint16_t port);

  // Runtime knobs.
  void setHeartbeatInterval(uint32_t ms) { _heartbeat_interval_ms = ms; }
  void setWatchdogTimeout(uint32_t ms)   { _watchdog_timeout_ms = ms; }

private:
  WiFiClient _client;     // shim-provided POSIX socket
  char       _host[64];
  uint16_t   _port;
  uint8_t    _token[32];
  uint8_t    _token_len;
  bool       _authenticated;
  bool       _rx_started;
  bool       _config_acked;
  bool       _auto_cad_acked;
  uint32_t   _last_reconnect_ms;
  uint32_t   _reconnect_count;
  uint8_t    _reconnect_backoff_step;

  pymc_proto::FrameParser _parser;

  uint32_t _last_ping_sent_ms;
  uint32_t _last_frame_recv_ms;
  uint32_t _heartbeat_interval_ms;
  uint32_t _watchdog_timeout_ms;

  struct RxFrame {
    uint8_t  data[pymc_proto::MAX_LORA_PAYLOAD];
    uint8_t  len;
    int16_t  rssi_dbm;
    int16_t  snr_x10;
    int16_t  sig_rssi_dbm;
    uint32_t recv_ms;
  };
  static constexpr size_t RX_RING = 8;
  RxFrame  _rx_ring[RX_RING];
  uint8_t  _rx_head, _rx_tail;

  enum TxState : uint8_t { TX_IDLE, TX_PENDING, TX_DONE, TX_FAIL };
  TxState  _tx_state;
  uint32_t _tx_started_ms;
  uint32_t _last_tx_airtime_us;

  float _last_rssi, _last_snr;
  int   _noise_floor_dbm;

  float    _freq_mhz, _bw_khz;
  uint8_t  _sf, _cr;
  int8_t   _power_dbm;
  uint16_t _syncword;
  uint8_t  _preamble_len;

  uint32_t _n_rx, _n_tx, _n_crc_err, _n_pong;

  bool sendFrame(uint8_t cmd, const uint8_t* payload, uint16_t len);
  void pumpRx();
  void handleEvent(uint8_t cmd, const uint8_t* payload, uint16_t len);

  bool tcpConnect();
  bool sendAuth();
  bool sendConfig();
  bool sendAutoCad(bool enable);
  bool sendRxStart();
  void onDisconnect();
  bool awaitFlag(bool& flag, uint32_t timeout_ms);
  bool connectAndHandshake();

  bool ringPush(const pymc_proto::RxPacketMeta& meta, const uint8_t* data, uint8_t data_len);
  bool ringPop(uint8_t* dst, int max_len, int& out_len);
};
