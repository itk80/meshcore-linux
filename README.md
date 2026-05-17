# meshcore-linux

Linux systemd service running the MeshCore repeater stack natively. Works
over TCP to [pymc_tcp / pymc_usb](https://github.com/itk80/pymc_usb)
modem firmware — the host has no LoRa peripheral, all `mesh::Radio`
operations are tunnelled.

> **Remote modem requirement.** The TCP-reachable radio MUST be running
> firmware from the [**pymc_usb**](https://github.com/itk80/pymc_usb)
> project (any board variant — Heltec V3, Lilygo T3S3, RAK3112 — as long
> as the firmware is built from `pymc_usb`). No other modem firmware is
> supported; the protocol details (framing, CRC, opcode set, RX_PACKET
> metadata layout) are bound to that firmware.

## Quick install (.deb, recommended)

```bash
# Pick the matching architecture from the latest release.
ARCH=$(dpkg --print-architecture)   # amd64 or arm64
wget "https://github.com/itk80/meshcore-linux/releases/latest/download/meshcore-linux_${ARCH}.deb"
sudo dpkg -i "meshcore-linux_${ARCH}.deb"

# Edit modem.host (default placeholder is "modem.local") and restart.
sudo nano /etc/Meshcore-Linux/config.json
sudo systemctl restart Meshcore-Linux
```

Tail logs: `journalctl -fu Meshcore-Linux`. Open the UI:
`http://<host>:8080/`.

## What it is

- **`LinuxTcpRadio`** — `mesh::Radio` over TCP + pymc_usb v0.7
  (AUTH, SET_CONFIG, SET_AUTO_CAD, RX_START, heartbeat, watchdog,
  exp-backoff reconnect, SO_LINGER RST-on-close).
- **`LinuxRepeaterMesh`** — `mesh::Mesh` + `CommonCLICallbacks` full
  repeater: adverts, ACL, loop detection, region scoping, region save,
  remote LoRa admin (login + stats + neighbours + CLI bridge).
- **`LinuxPlatform.h`** — host abstractions: `LinuxMainBoard` (stubs +
  `reboot()` exits for systemd), `LinuxMillisClock`, `LinuxRTCClock`,
  `LinuxRNG` (`getrandom(2)` with `/dev/urandom` fallback).
- **`ConfigServer`** — HTTP/JSON API on `:8080`. Default port is **not**
  5060 because Chrome blocks it (`ERR_UNSAFE_PORT`, SIP). Endpoints:
  - `GET /` — HTML configurator + live status panel + CLI terminal
  - `GET  /api/config` — live config (overlays NodePrefs onto on-disk JSON)
  - `POST /api/config` — persist + hot-apply
  - `POST /api/command` — CLI bridge (every CommonCLI verb)
  - `GET  /api/status` — modem state + counters + uptime
  - `POST /api/reboot` — `exit(0)` (systemd restarts)
- **systemd unit** with `ProtectSystem=strict`, `Restart=always`,
  `StateDirectory=Meshcore-Linux`.

## Identity

The Ed25519 keypair lives **inside `/etc/Meshcore-Linux/config.json`**:

```json
"identity": {
  "pub": "<64 hex chars — Ed25519 public key>",
  "prv": "<128 hex chars — Ed25519 secret key>"
}
```

Empty fields trigger lazy generation via `getrandom(2)` on first start;
the JSON is atomically rewritten. Reinstall / upgrade preserves the
keypair — `dpkg` treats `config.json` as a conffile and never overwrites
operator changes. Back up `/etc/Meshcore-Linux/config.json` and you've
backed up the identity.

Adverts are **disabled by default** (`advert_interval=0`) so a freshly
seeded node never beacons before the operator has confirmed identity,
node name, region and LoRa params. Enable through the UI or:

```bash
curl -X POST -d '{"command":"set advert.interval 1"}' \
     -H 'Content-Type: application/json' \
     http://<host>:8080/api/command
```

## Build from source

Requirements: `g++ ≥ 11` (`-std=c++17`), `make`, sibling MeshCore checkout.

```
~/code/
├── MeshCore/             # ripplebiz/MeshCore checkout with PlatformIO libs cached
└── meshcore-linux/       # this repo
```

```bash
make            # produces ./meshcore-linux
make run        # local run against ./config/config.example.json
```

## Build a .deb locally

```bash
sudo apt install build-essential debhelper devscripts
dpkg-buildpackage -b -uc -us
ls ../meshcore-linux_*.deb
```

## Layout

```
src/                   ConfigServer + LinuxRepeaterMesh + LinuxTcpRadio + LinuxPlatform + main
shims/                 Arduino-API + WiFiClient + Stream + FS + RNG stub
third_party/           Vendored single-headers (nlohmann/json, cpp-httplib)
systemd/               Unit file
scripts/install.sh     Manual deploy (alternative to .deb)
debian/                Package metadata for dpkg-buildpackage
config/                Example JSON config
.github/workflows/     CI + release automation
```

## License

MIT — see [LICENSE](LICENSE). Built on the MIT-licensed
[MeshCore](https://github.com/ripplebiz/MeshCore) mesh stack by Scott
Powell / rippleradios.com; full attribution in [THIRD_PARTY.md](THIRD_PARTY.md).
