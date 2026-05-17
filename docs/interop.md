# Interoperability tests

This doc covers the two halves of interop verification:

1. **Software half** — runs in CI on every push, no hardware required. Verifies
   that `./meshcore-linux` speaks pymc_usb wire protocol v0.7 correctly and
   produces structurally valid, cryptographically signed MeshCore adverts.
2. **Hardware half** — manual benchwork. Verifies on-air interop with real LoRa
   radios: bit-level comparison vs MCU firmware, 2-hop routing, and end-to-end
   crypto with the MeshCore mobile/desktop app.

Run the software half locally with:

```bash
make test     # 45 unit checks against pymc_proto.h (no MeshCore deps)
make interop  # mock pymc_usb modem + Linux binary + signed-advert verification
```

---

## What the software tests cover

### `make test` — `tests/test_pymc_proto.cpp`

Pure-C++ unit tests against `src/pymc_proto.h`, no MeshCore/Arduino link.

- CRC-16/CCITT vectors (incl. the canonical "123456789" → 0x29B1)
- Frame builder golden bytes (PING, SET_AUTO_CAD, SET_CONFIG)
- `RadioConfig` 14-byte on-wire layout (drift-detector)
- Frame parser round-trips: PING, random 7-byte payload
- Parser robustness: skips pre-SYNC garbage; rejects bad CRC and rearms
- `EVT_RX_PACKET` / `EVT_TX_DONE` payload decoders
- Airtime estimator monotonicity + EU/UK Narrow regime bounds
- Packet score: threshold behaviour and SNR monotonicity

### `make interop` — `tests/run_interop.sh`

End-to-end software interop using a Python mock modem:

1. `tests/mock_modem.py` accepts the TCP connection and ACKs the
   `SET_CONFIG → SET_AUTO_CAD → RX_START → PING` handshake. Every `TX_REQUEST`
   payload is appended to `tests/captured_tx.hex` (one frame per line).
2. `./meshcore-linux` is booted with `tests/config.interop.json`, which
   pins the Ed25519 keypair to the canonical MeshCore upstream test
   client (`Identity.cpp:59-74`), so the on-air bytes are reproducible.
3. `set name InteropTestNode` and `advert` are POSTed to `/api/command`.
4. The captured TX frame is parsed and asserted to be:
   - header `0x11` = `(PAYLOAD_TYPE_ADVERT << 2) | ROUTE_TYPE_FLOOD`
   - path_len = 0 (fresh self-advert)
   - pubkey (32 B) = seeded `1ec77175b0918ed2…`
   - AdvertData type = `ADV_TYPE_REPEATER`, name = `InteropTestNode`
   - **Ed25519 signature over `pub || timestamp || app_data` verifies
     against the seeded pubkey** — the same check every receiving
     MeshCore node performs in `Mesh::onRecvData` (Mesh.cpp:239+).

This is a stronger interop check than bit-identity: it guarantees that
any MeshCore node parsing our advert will accept it. Bit-identity would
only confirm two implementations serialise in the same byte order;
signature verification proves the whole crypto + framing pipeline.

---

## Hardware half — manual procedures

Run these on a bench with the gear listed. Capture artefacts (logs, hex
dumps, photos of OLED) under `docs/interop_runs/<YYYY-MM-DD>/` and update
`STATUS.md` with the result of each.

### Bench setup

- **Modem node:** Heltec V3 / Lilygo T3S3 / RAK3112 flashed with
  [pymc_usb v0.7.x](https://github.com/itk80/pymc_usb), Wi-Fi joined
  to the bench LAN, mDNS name visible (`modem.local`) or known IP.
- **Linux node:** RPi 4 / x86 box running `meshcore-linux` (.deb installed
  or `make run`), configured to point at the modem.
- **Witness node A:** stock MeshCore firmware on a second LoRa-capable
  board (e.g. Heltec_v3_repeater) — for "is the linux box visible in the
  mesh?" check.
- **Witness node B:** third stock MeshCore node — needed only for the
  2-hop routing scenario.
- **Mobile / desktop:** MeshCore companion app (Android / iOS / Linux)
  paired with witness node A — needed for the E2E crypto scenario.

All four nodes on the same frequency / SF / CR / syncword.

### H1 — bit-identical adverts vs MCU

**Goal:** prove that meshcore-linux and a stock MCU MeshCore repeater,
loaded with the **same** Ed25519 keypair and the **same** node name, emit
byte-identical adverts modulo timestamp + signature.

**Steps:**

1. Flash a stock MCU repeater with the test keypair (`Identity.cpp:59-74`)
   and node name `InteropTestNode`. Easiest path: dev build with the
   keypair hard-coded in `setup()`, or upload via the companion app.
2. Configure meshcore-linux with the same identity in
   `/etc/Meshcore-Linux/config.json` and `set name InteropTestNode` via
   the HTTP CLI bridge.
3. Run a sniffer somewhere within range — either:
   - **SDR:** an RTL-SDR + `gr-lora_sdr` capturing the right band, or
   - **Sniffer node:** a third MeshCore node with the repeater build,
     `set log on`, dumping every received packet to its packet log.
4. Trigger one advert on each node (`advert` CLI verb). Repeat at the same
   wall-clock second on both nodes for a fair comparison.
5. Extract the on-air payload bytes from both captures. Strip the 1-byte
   header + 1-byte path_len prefix (Packet::writeTo, Packet.cpp:52-63),
   then compare:
   - bytes 0..31 (pub_key) must be **identical**
   - bytes 32..35 (timestamp LE) will differ by the second offset (OK if
     within ±3 s and both nodes have an RTC source)
   - bytes 36..99 (signature) will differ because Ed25519 over different
     timestamps produces different signatures — verify each independently
     against the shared pubkey
   - bytes 100.. (AdvertData) must be **identical** (same flags, same
     name; lat/lon both zero on both ends)

**Pass criterion:** pubkey + AdvertData identical; both signatures
verify; timestamps within ±3 s.

### H2 — 2-hop flood routing

**Goal:** prove a packet originating on MCU node A is repeated by
meshcore-linux and received intact by MCU node B, when A and B are out
of direct radio range but both in range of meshcore-linux.

**Steps:**

1. Physically separate witness A and witness B so they cannot hear each
   other. Place meshcore-linux's modem antenna where it can hear both
   (or arrange shielding).
2. From A's CLI, send an `advert` (flood, 3 hops max).
3. On meshcore-linux: `journalctl -fu Meshcore-Linux` should show
   `recv_flood` increment, then a forwarded send.
4. On B's CLI, `get contacts` should now show A's identity.
5. Optional follow-up: send a direct text message A → B using the path
   that goes through meshcore-linux; confirm delivery + ACK.

**Pass criterion:** A's advert reaches B; `meshcore-linux`'s packet log
shows the RX from A and the TX-out toward B with the path appended.

### H3 — E2E crypto with the mobile app

**Goal:** prove a message encrypted by the mobile app for some contact
is decryptable on meshcore-linux if meshcore-linux is the contact.

**Steps:**

1. In the mobile app paired with witness A, add meshcore-linux as a
   contact (scan its QR / paste its pubkey from
   `GET /api/config → identity.pub`).
2. From the mobile app, send a text DM to meshcore-linux.
3. On meshcore-linux, `journalctl -fu Meshcore-Linux` should show the
   received packet; CLI `get contacts` should now include the mobile
   client.

**Pass criterion:** the message text appears in the meshcore-linux packet
log (logs the decrypted content for DM destined at us — exact log line
format depends on logger settings).

### Recording results

For each procedure: capture the relevant log file, mark pass/fail in
`STATUS.md`, and (for failures) file an issue with the artefacts
attached. Pattern under `docs/interop_runs/`:

```
docs/interop_runs/2026-05-17/
  H1-pass.md         # short note + linked captures
  H1-mcu-adv.hex
  H1-linux-adv.hex
  H2-pass.md
  packets-A.log
  packets-mcl.log
  packets-B.log
  H3-fail.md         # if it failed
  Meshcore-Linux.log
```
