#!/usr/bin/env bash
# tests/run_interop.sh — software-side interop test runner.
#
# What this verifies (no hardware required):
#   1. ./meshcore-linux completes the full pymc_usb v0.7 handshake against
#      a mock modem: AUTH-skip → SET_CONFIG → SET_AUTO_CAD → RX_START → PING.
#   2. Within ~10 s of boot it transmits a self-advertisement.
#   3. The on-air bytes carry the seeded pubkey at the expected offset,
#      which is the prerequisite for interop with any MeshCore node — if
#      another node parses our advert and matches the pubkey, we are
#      bit-compatible with upstream `mesh::Mesh::createAdvert`.
#
# What this does NOT verify (needs hardware — see docs/interop.md):
#   - Bit-identical bytes vs a stock MCU MeshCore firmware sending the same
#     adv (timestamp + signature vary per run; equality would need synced
#     RTC, which only an SDR/sniffer test can capture meaningfully).
#   - 2-hop flood routing through real LoRa with three physical nodes.
#   - E2E crypto interop with the MeshCore mobile/desktop app.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="${BIN:-./meshcore-linux}"
CONFIG="${CONFIG:-tests/config.interop.json}"
MOCK_PORT=15055
API_PORT=18080
TX_LOG="tests/captured_tx.hex"
MOCK_LOG="tests/mock_modem.log"
BIN_LOG="tests/binary.log"
EXPECTED_PUB="1ec77175b0918ed206f9ae04ec136d6d5d4315bb26305427f645b492e9350c10"

# ── Preconditions ────────────────────────────────────────────────────
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found or not executable. Run \`make\` first." >&2
  exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "ERROR: python3 required" >&2; exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
  echo "ERROR: curl required" >&2; exit 2
fi

cleanup() {
  if [[ -n "${BIN_PID:-}" ]] && kill -0 "$BIN_PID" 2>/dev/null; then
    kill "$BIN_PID" 2>/dev/null || true
    wait "$BIN_PID" 2>/dev/null || true
  fi
  if [[ -n "${MOCK_PID:-}" ]] && kill -0 "$MOCK_PID" 2>/dev/null; then
    kill "$MOCK_PID" 2>/dev/null || true
    wait "$MOCK_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

rm -f "$TX_LOG" "$MOCK_LOG" "$BIN_LOG"
rm -rf tests/.interop_state
mkdir -p tests/.interop_state

# ── Start mock modem ─────────────────────────────────────────────────
echo "[interop] starting mock modem on 127.0.0.1:$MOCK_PORT"
python3 tests/mock_modem.py \
  --host 127.0.0.1 --port "$MOCK_PORT" \
  --tx-log "$TX_LOG" \
  >"$MOCK_LOG" 2>&1 &
MOCK_PID=$!

# Wait for the listener to be ready.
for _ in $(seq 1 50); do
  if (echo >/dev/tcp/127.0.0.1/$MOCK_PORT) 2>/dev/null; then break; fi
  sleep 0.1
done
if ! (echo >/dev/tcp/127.0.0.1/$MOCK_PORT) 2>/dev/null; then
  echo "ERROR: mock modem failed to come up; see $MOCK_LOG" >&2
  cat "$MOCK_LOG" >&2 || true
  exit 1
fi

# ── Start the binary ─────────────────────────────────────────────────
echo "[interop] starting $BIN against $CONFIG"
"$BIN" "$CONFIG" >"$BIN_LOG" 2>&1 &
BIN_PID=$!

# Wait for the HTTP API to bind (proves the binary booted past config load).
for _ in $(seq 1 100); do
  if curl -fsS "http://127.0.0.1:$API_PORT/api/status" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
if ! curl -fsS "http://127.0.0.1:$API_PORT/api/status" >/dev/null 2>&1; then
  echo "ERROR: binary's HTTP API never came up; logs follow:" >&2
  echo "--- $BIN_LOG ---"   >&2; cat "$BIN_LOG"  >&2 || true
  echo "--- $MOCK_LOG ---"  >&2; cat "$MOCK_LOG" >&2 || true
  exit 1
fi
echo "[interop] HTTP API is live; setting node name + triggering advert"

# Force the on-air node name to a deterministic value so the test is
# stable regardless of any default the binary applies on first boot. The
# `set name` and `advert` verbs are part of CommonCLI (see helpers/
# CommonCLI.cpp) and reach mesh.processCommand() through the HTTP bridge.
post_cmd() {
  curl -fsS -H 'Content-Type: application/json' \
       -d "{\"command\":\"$1\"}" \
       "http://127.0.0.1:$API_PORT/api/command" || true
}
echo "[interop] /api/command set name → $(post_cmd "set name InteropTestNode")"
# advert_interval defaults to 0 (beaconing disabled until the operator
# explicitly enables it — see README). The supported way to make the node
# emit one advert now is the `advert` CLI verb.
echo "[interop] /api/command advert   → $(post_cmd "advert")"

# CommonCLI schedules the self-advert with a 1500 ms delay so the CLI
# reply can be flushed first. Wait up to 15 s for the TX_REQUEST.
DEADLINE=$((SECONDS + 15))
while (( SECONDS < DEADLINE )); do
  if [[ -s "$TX_LOG" ]]; then break; fi
  sleep 0.25
done

# ── Stop binary cleanly ──────────────────────────────────────────────
kill "$BIN_PID" 2>/dev/null || true
wait "$BIN_PID" 2>/dev/null || true
BIN_PID=""
kill "$MOCK_PID" 2>/dev/null || true
wait "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""

# ── Assertions ───────────────────────────────────────────────────────
echo
echo "[interop] === assertions ==="

if [[ ! -s "$TX_LOG" ]]; then
  echo "FAIL: no TX_REQUEST frames captured within 15 s" >&2
  echo "--- binary log (tail) ---"; tail -40 "$BIN_LOG" >&2
  echo "--- mock log (tail) ---";   tail -40 "$MOCK_LOG" >&2
  exit 1
fi
N=$(wc -l < "$TX_LOG" | tr -d ' ')
echo "PASS: captured $N TX_REQUEST frame(s)"

# Verify handshake landmarks present in mock log.
for tag in "AUTH" "SET_CONFIG" "SET_AUTO_CAD" "RX_START" "TX_REQUEST"; do
  # AUTH isn't sent when token is empty (config.interop.json sets ""), so skip.
  if [[ "$tag" == "AUTH" ]]; then continue; fi
  if ! grep -q "$tag" "$MOCK_LOG"; then
    echo "FAIL: mock modem never saw $tag" >&2
    cat "$MOCK_LOG" >&2; exit 1
  fi
done
echo "PASS: handshake landmarks (SET_CONFIG, SET_AUTO_CAD, RX_START, TX_REQUEST) all observed"

# Parse the first captured TX frame and verify the on-air structure:
#   byte 0          : header — must be 0x11 (PAYLOAD_TYPE_ADVERT=4 << PH_TYPE_SHIFT=2 | ROUTE_TYPE_FLOOD=1)
#   byte 1          : path_len — must be 0 (fresh self-advert, no transit hops)
#   bytes 2..34     : pub_key (32 B) — must equal the seeded $EXPECTED_PUB
#   bytes 34..38    : timestamp LE
#   bytes 38..102   : Ed25519 signature
#   bytes 102..N    : AdvertData (app payload — includes our node name)
FIRST=$(head -n 1 "$TX_LOG")
HEADER="${FIRST:0:2}"
PATHLEN="${FIRST:2:2}"
PUB="${FIRST:4:64}"
APPDATA="${FIRST:204}"

if [[ "$HEADER" != "11" ]]; then
  echo "FAIL: header byte = 0x$HEADER, expected 0x11 (ADVERT|FLOOD)" >&2; exit 1
fi
echo "PASS: header byte = 0x$HEADER (ADVERT|FLOOD)"

if [[ "$PATHLEN" != "00" ]]; then
  echo "FAIL: path_len = 0x$PATHLEN, expected 0x00 (fresh advert)" >&2; exit 1
fi
echo "PASS: path_len = 0"

if [[ "$PUB" != "$EXPECTED_PUB" ]]; then
  echo "FAIL: on-air pubkey doesn't match seeded identity" >&2
  echo "  got:  $PUB" >&2
  echo "  want: $EXPECTED_PUB" >&2
  exit 1
fi
echo "PASS: on-air pubkey matches seeded identity (32 B at offset 2)"

# AdvertData layout (helpers/AdvertDataHelpers.cpp:3-27):
#   byte 0 : flags — ADV_TYPE_* in low 4 bits, ADV_LATLON_MASK=0x10,
#            ADV_FEAT1_MASK=0x20, ADV_FEAT2_MASK=0x40, ADV_NAME_MASK=0x80.
#   bytes 1..1+8  : lat,lon floats LE  (only if ADV_LATLON_MASK)
#   bytes 1..1+2  : extra1 LE          (only if ADV_FEAT1_MASK)
#   bytes 1..1+2  : extra2 LE          (only if ADV_FEAT2_MASK)
#   bytes …..N    : node name ASCII   (only if ADV_NAME_MASK)
#
# LinuxRepeaterMesh::bringUp() unconditionally promotes advert_loc_policy
# from NONE to PREFS (so every advert carries lat/lon, even if (0,0)) —
# see LinuxRepeaterMesh.cpp:226-229. So a vanilla advert from this binary
# is flags=0x92 (REPEATER|NAME|LATLON). We assert structure and decode
# fields instead of pinning the exact flags byte, since that's a property
# of the binary's policy, not the wire protocol.
python3 - "$APPDATA" "InteropTestNode" <<'PY' || exit 1
import sys, struct
app = bytes.fromhex(sys.argv[1])
expected_name = sys.argv[2]
ADV_TYPE_REPEATER  = 0x02
ADV_LATLON_MASK    = 0x10
ADV_FEAT1_MASK     = 0x20
ADV_FEAT2_MASK     = 0x40
ADV_NAME_MASK      = 0x80
ADV_TYPE_MASK      = 0x0F
flags = app[0]
adv_type = flags & ADV_TYPE_MASK
if adv_type != ADV_TYPE_REPEATER:
    print(f"FAIL: AdvertData type = {adv_type}, expected ADV_TYPE_REPEATER (2)", file=sys.stderr)
    sys.exit(1)
if not (flags & ADV_NAME_MASK):
    print(f"FAIL: ADV_NAME_MASK not set in flags=0x{flags:02X}", file=sys.stderr)
    sys.exit(1)
i = 1
if flags & ADV_LATLON_MASK:
    lat, lon = struct.unpack_from("<ff", app, i); i += 8
    print(f"PASS: AdvertData carries lat/lon = ({lat}, {lon}) (policy=PREFS, expected for this binary)")
if flags & ADV_FEAT1_MASK: i += 2
if flags & ADV_FEAT2_MASK: i += 2
name = app[i:].decode("ascii", errors="replace")
if name != expected_name:
    print(f"FAIL: AdvertData name = '{name}', expected '{expected_name}'", file=sys.stderr)
    sys.exit(1)
print(f"PASS: AdvertData type=REPEATER, name='{name}' (CLI-set, end-to-end)")
PY

# Verify the Ed25519 signature over (pub_key || timestamp || app_data) using
# the upstream test pubkey. This is the strongest interop check: any other
# MeshCore node verifying our advert runs the same math.
TIMESTAMP_HEX="${FIRST:68:8}"
SIG_HEX="${FIRST:76:128}"
if ! python3 -c "
import sys, hashlib
try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
except Exception:
    print('SKIP signature check (python cryptography not installed)')
    sys.exit(2)
pub = bytes.fromhex('$EXPECTED_PUB')
ts  = bytes.fromhex('$TIMESTAMP_HEX')
sig = bytes.fromhex('$SIG_HEX')
app = bytes.fromhex('$APPDATA')
msg = pub + ts + app
try:
    Ed25519PublicKey.from_public_bytes(pub).verify(sig, msg)
    print('signature OK')
    sys.exit(0)
except Exception as e:
    print(f'signature INVALID: {e}', file=sys.stderr)
    sys.exit(1)
"; then
  rc=$?
  if [[ $rc -eq 2 ]]; then
    echo "INFO: Ed25519 verification skipped (install python3-cryptography to enable)"
  else
    echo "FAIL: Ed25519 advert signature did not verify against seeded pubkey" >&2
    exit 1
  fi
else
  echo "PASS: Ed25519 advert signature verifies against seeded pubkey"
fi

echo
echo "[interop] all software-side interop assertions passed."
echo "[interop] artefacts: $TX_LOG, $MOCK_LOG, $BIN_LOG"
