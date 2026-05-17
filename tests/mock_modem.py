#!/usr/bin/env python3
# Mock pymc_usb modem (wire protocol v0.7) used by tests/run_interop.sh.
#
# Speaks the framing exactly like a real Heltec_V3 / Lilygo_T3S3 running
# pymc_usb v0.7 firmware would: SYNC | CMD | LEN_LE | PAYLOAD | CRC_LE
# with CRC-16/CCITT (poly 0x1021, init 0xFFFF) over CMD+LEN+PAYLOAD.
#
# Accepts a single TCP client (real modem rejects a second), ACKs the
# standard handshake (AUTH → SET_CONFIG → SET_AUTO_CAD → RX_START → PING),
# answers TX_REQUEST with a deterministic EVT_TX_DONE (airtime=0), and
# appends every TX_REQUEST payload as one hex line to --tx-log so the
# integration test can diff against a golden vector.

import argparse
import datetime
import socket
import struct
import sys
import threading
import time

SYNC = 0xAA

# Host → Modem
CMD_TX_REQUEST       = 0x01
CMD_SET_CONFIG       = 0x10
CMD_NOISE_REQ        = 0x22
CMD_RX_START         = 0x31
CMD_SET_AUTO_CAD     = 0x4A
CMD_AUTH             = 0x50
CMD_PING             = 0xFF

# Modem → Host
EVT_TX_DONE              = 0x02
EVT_RX_PACKET            = 0x04
EVT_CONFIG_RESP          = 0x12
EVT_NOISE_RESP           = 0x23
EVT_RX_STARTED           = 0x33
EVT_SET_AUTO_CAD_RESP    = 0x4B
EVT_AUTH_OK              = 0x51
EVT_LOG_MSG              = 0x80
EVT_PONG                 = 0xFF


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    hdr = bytes([cmd, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    crc = crc16_ccitt(hdr + payload)
    return bytes([SYNC]) + hdr + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def log(msg: str) -> None:
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"[mock_modem {ts}] {msg}", flush=True)


class FrameParser:
    """Byte-by-byte parser matching pymc_proto::FrameParser semantics."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.state = "SYNC"
        self.cmd = 0
        self.length = 0
        self.payload = bytearray()
        self.crc_lo = 0

    def feed(self, b: int):
        """Returns (cmd, payload_bytes) on a complete validated frame, else None."""
        if self.state == "SYNC":
            if b == SYNC:
                self.state = "CMD"
            return None
        if self.state == "CMD":
            self.cmd = b
            self.state = "LEN_LO"
            return None
        if self.state == "LEN_LO":
            self.length = b
            self.state = "LEN_HI"
            return None
        if self.state == "LEN_HI":
            self.length |= b << 8
            if self.length > 261:
                # Oversize; hunt for next SYNC.
                self.reset()
                return None
            self.payload = bytearray()
            self.state = "PAYLOAD" if self.length else "CRC_LO"
            return None
        if self.state == "PAYLOAD":
            self.payload.append(b)
            if len(self.payload) >= self.length:
                self.state = "CRC_LO"
            return None
        if self.state == "CRC_LO":
            self.crc_lo = b
            self.state = "CRC_HI"
            return None
        if self.state == "CRC_HI":
            crc_rx = self.crc_lo | (b << 8)
            hdr = bytes([self.cmd, self.length & 0xFF, (self.length >> 8) & 0xFF])
            crc_calc = crc16_ccitt(hdr + bytes(self.payload))
            cmd = self.cmd
            payload = bytes(self.payload)
            self.reset()
            return (cmd, payload) if crc_calc == crc_rx else None
        return None


class MockModem:
    def __init__(self, host: str, port: int, tx_log: str, require_auth: bool,
                 inject_rx_after: float, inject_rx_hex: str) -> None:
        self.host = host
        self.port = port
        self.tx_log_path = tx_log
        self.require_auth = require_auth
        self.inject_rx_after = inject_rx_after
        self.inject_rx_payload = bytes.fromhex(inject_rx_hex) if inject_rx_hex else b""
        self._sock = None
        self._stop = threading.Event()

    def start(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.port))
        srv.listen(1)
        srv.settimeout(0.5)
        self._sock = srv
        log(f"listening on {self.host}:{self.port}, tx-log={self.tx_log_path}")
        # Truncate the tx log so each run starts clean.
        open(self.tx_log_path, "w").close()
        while not self._stop.is_set():
            try:
                client, addr = srv.accept()
            except socket.timeout:
                continue
            log(f"client connected from {addr[0]}:{addr[1]}")
            try:
                self._serve(client)
            except (ConnectionResetError, BrokenPipeError) as e:
                log(f"client gone: {e}")
            finally:
                client.close()
                log("client closed")

    def stop(self) -> None:
        self._stop.set()

    def _serve(self, client: socket.socket) -> None:
        client.setblocking(False)
        parser = FrameParser()
        authed = not self.require_auth
        start = time.monotonic()
        rx_injected = False

        def send(cmd: int, payload: bytes = b"") -> None:
            client.sendall(build_frame(cmd, payload))

        # Banner log to the host, matches real firmware behaviour.
        send(EVT_LOG_MSG, bytes([1]) + b"mock_modem ready")

        while not self._stop.is_set():
            # Drive RX injection if requested.
            if (not rx_injected and self.inject_rx_payload
                    and (time.monotonic() - start) >= self.inject_rx_after):
                meta = struct.pack("<hhh", -55, 80, -55)  # rssi=-55dBm, snr=8.0dB
                send(EVT_RX_PACKET, meta + self.inject_rx_payload)
                rx_injected = True
                log(f"injected EVT_RX_PACKET ({len(self.inject_rx_payload)}B)")

            try:
                chunk = client.recv(4096)
            except BlockingIOError:
                time.sleep(0.005)
                continue
            if not chunk:
                return
            for b in chunk:
                frame = parser.feed(b)
                if frame is None:
                    continue
                cmd, payload = frame
                self._handle(send, cmd, payload, authed_ref=[authed])
                # AUTH transition.
                if self.require_auth and cmd == CMD_AUTH:
                    authed = True

    def _handle(self, send, cmd: int, payload: bytes, authed_ref) -> None:
        if cmd == CMD_PING:
            send(EVT_PONG)
            return
        if cmd == CMD_AUTH:
            log(f"AUTH token={payload.hex()} → EVT_AUTH_OK")
            send(EVT_AUTH_OK)
            return
        if cmd == CMD_SET_CONFIG:
            if len(payload) == 14:
                freq, bw, sf, cr, pwr, sw, pre = struct.unpack("<IIbbbHB", payload)
                log(f"SET_CONFIG freq={freq}Hz bw={bw}Hz sf={sf} cr={cr} pwr={pwr}dBm "
                    f"sync=0x{sw:04X} preamble={pre} → EVT_CONFIG_RESP")
            else:
                log(f"SET_CONFIG len={len(payload)} (unexpected) → EVT_CONFIG_RESP")
            send(EVT_CONFIG_RESP, b"\x00")
            return
        if cmd == CMD_SET_AUTO_CAD:
            v = payload[0] if payload else 0
            log(f"SET_AUTO_CAD enable={v} → EVT_SET_AUTO_CAD_RESP(status=0)")
            send(EVT_SET_AUTO_CAD_RESP, b"\x00")
            return
        if cmd == CMD_RX_START:
            log("RX_START → EVT_RX_STARTED")
            send(EVT_RX_STARTED)
            return
        if cmd == CMD_NOISE_REQ:
            send(EVT_NOISE_RESP, struct.pack("<h", -1150))  # -115.0 dBm
            return
        if cmd == CMD_TX_REQUEST:
            with open(self.tx_log_path, "a") as f:
                f.write(payload.hex() + "\n")
            log(f"TX_REQUEST {len(payload)}B captured → EVT_TX_DONE")
            send(EVT_TX_DONE, struct.pack("<I", 0))
            return
        log(f"unhandled cmd=0x{cmd:02X} payload={payload.hex()}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5055)
    ap.add_argument("--tx-log", default="tests/captured_tx.hex")
    ap.add_argument("--require-auth", action="store_true")
    ap.add_argument("--inject-rx-after", type=float, default=0.0,
                    help="Seconds after connect to inject one EVT_RX_PACKET")
    ap.add_argument("--inject-rx-hex", default="",
                    help="Hex bytes for the injected RX LoRa payload")
    args = ap.parse_args()

    modem = MockModem(args.host, args.port, args.tx_log, args.require_auth,
                      args.inject_rx_after, args.inject_rx_hex)
    try:
        modem.start()
    except KeyboardInterrupt:
        log("Ctrl-C, exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
