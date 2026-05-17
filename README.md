# Meshcore-Linux

Linux port of the MeshCore mesh radio repeater. The service has no on-host
LoRa peripheral: all `mesh::Radio` operations are tunnelled over TCP (wire
protocol v0.7) to a remote modem.

> **Remote modem requirement.** The TCP-reachable radio MUST be running
> firmware from the [**pymc_usb**](https://github.com/itk80/pymc_usb)
> project (any board variant — Heltec V3, Lilygo T3S3, RAK3112, …, as long
> as the firmware is built from `pymc_usb`). No other modem firmware is
> supported; the protocol details (framing, CRC, opcode set, RX_PACKET
> metadata layout) are bound to that firmware.

## What it is

- **`LinuxTcpRadio`** — `mesh::Radio` implementation that does TCP+pymc_usb
  v0.7: AUTH, SET_CONFIG, SET_AUTO_CAD (Variant A LBT), RX_START, heartbeat
  ping, watchdog, exp-backoff reconnect, SO_LINGER RST-on-close.
- **`LinuxMesh`** — `mesh::Mesh` subclass that boots the upstream Dispatcher
  + de-dup tables and logs every received packet.
- **`LinuxPlatform.h`** — host abstractions: `LinuxMainBoard` (stubs +
  `reboot()` exits for systemd), `LinuxMillisClock`, `LinuxRTCClock`,
  `LinuxRNG` (`getrandom(2)` with `/dev/urandom` fallback).
- **`ConfigServer`** — HTTP/JSON API on port 8080 (cpp-httplib). The
  default port is **not** 5060 even though that's what an initial spec
  asked for: modern browsers hard-code 5060 (SIP) in their
  [unsafe-port list](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/net/base/port_util.cc)
  and refuse to fetch HTTP from it (`ERR_UNSAFE_PORT`). 8080 is the
  classic alt-HTTP port and works everywhere. Endpoints:
  - `GET /` &mdash; tiny HTML editor + live status panel
  - `GET  /api/config` &mdash; current config as JSON
  - `POST /api/config` &mdash; persist + hot-apply (modem endpoint, LoRa params)
  - `GET  /api/status` &mdash; modem state + counters + uptime
  - `POST /api/reboot` &mdash; `exit(0)` (systemd restarts)
- **systemd unit** + `scripts/install.sh` for a 1-command production deploy.

## Status

| Phase | Status |
|---|---|
| Probe Linux host                         | ✅ |
| Project scaffold                         | ✅ |
| WiFiClient → POSIX socket shim           | ✅ |
| `LinuxTcpRadio : mesh::Radio`            | ✅ |
| `LinuxRepeaterMesh : mesh::Mesh + CommonCLICallbacks` (full repeater) | ✅ |
| Identity persistence in config.json (Ed25519 keypair lazy-generated, survives reinstall) | ✅ |
| JSON config (`/etc/Meshcore-Linux/config.json`)         | ✅ |
| HTTP config API on `:8080` + `/api/command` CLI bridge  | ✅ |
| systemd unit + install script + `StateDirectory`        | ✅ |

## Identity

The repeater's Ed25519 keypair lives **inside `/etc/Meshcore-Linux/config.json`**:

```json
"identity": {
  "pub": "<64 hex chars — Ed25519 public key>",
  "prv": "<128 hex chars — Ed25519 secret key>"
}
```

On first start (empty fields, or block missing) the service generates a
fresh keypair via `getrandom(2)` and writes it back atomically. **Reinstall
preserves the keypair**: `install.sh` never overwrites an existing
`config.json`. Backup `/etc/Meshcore-Linux/config.json` to back up the
repeater identity.

Adverts are **disabled by default** (`advert_interval=0`) so a freshly
seeded node never beacons before the operator has confirmed identity,
node name, region and LoRa params are correct. Enable via CLI:

```bash
curl -X POST -d 'set advert.interval 1' http://<host>:8080/api/command
```

## Build

Requirements: `g++ ≥ 11` (we use `-std=c++17`), `make`. Tested on Ubuntu
24.04.4 LTS.

This tree references the sibling [MeshCore-tcp](https://github.com/itk80/MeshCore)
checkout (branch `tcpradio`) for upstream sources, Crypto, and ed25519.
Both repos must be cloned side-by-side:

```
~/code/
├── MeshCore/             # itk80/MeshCore tcpradio branch, with pio libs cached
└── meshcore-linux/       # this repo
```

Inside `meshcore-linux/`:

```bash
make            # produces ./meshcore-linux
make run        # local run against ./config/config.example.json
```

## Deploy

```bash
sudo ./scripts/install.sh
```

Copies the binary to `/usr/local/bin/`, seeds `/etc/Meshcore-Linux/config.json`
on first install, installs `Meshcore-Linux.service`, `daemon-reload`,
enables + starts. Logs: `journalctl -fu Meshcore-Linux`.

Config UI: `http://<host>:5060/`.

## Layout

```
src/                   ConfigServer + LinuxMesh + LinuxTcpRadio + LinuxPlatform + main
shims/                 Arduino-API + WiFiClient + Stream + FS + base64 + RNG stub
third_party/           Vendored single-headers (nlohmann/json, cpp-httplib)
systemd/               Unit file
scripts/install.sh     Production deploy
config/                Example JSON config
Makefile               g++ in-tree build (no cmake required)
```
