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
- **`ConfigServer`** — HTTP/JSON API on port 5060 (cpp-httplib):
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
| `LinuxMesh` (Mesh + Dispatcher boot)     | ✅ |
| JSON config (`/etc/Meshcore-Linux/config.json`) | ✅ |
| HTTP config API on `:5060`               | ✅ |
| systemd unit + install script            | ✅ |
| **Full repeater behaviour** (BaseChatMesh subclass with `onMessageRecv`, `onDiscoveredContact`, ACL, advertisements, contacts) | ⏳ follow-up |

The currently-shipped `LinuxMesh` is a **passive listener**: it receives /
parses / de-dups raw packets but does not advertise itself, does not route,
and does not manage contacts. The pieces required to upgrade
(`BaseChatMesh.cpp`, `ClientACL.cpp`, `CommonCLI.cpp`, …) are already
compiled into the binary — the next step is to swap `LinuxMesh`'s base
class to `BaseChatMesh` and implement its ~20 pure virtuals, mirroring
`examples/simple_repeater/MyMesh.cpp` from upstream MeshCore.

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
