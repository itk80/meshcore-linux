# Changelog

All notable changes to this project will be documented in this file.
Synced with `debian/changelog` for the package version.

## [1.0.1] — 2026-05-17

### Added
- Interop test harness. `make test` runs unit checks against
  `src/pymc_proto.h` (CRC-16/CCITT vectors, frame builder/parser
  round-trips, RX/TX/Noise payload decoders, airtime + packet-score
  math). `make interop` boots the binary against a Python mock pymc_usb
  modem and verifies a self-advert end-to-end — including Ed25519
  signature verification over `pub || timestamp || app_data`, the same
  check every receiving MeshCore node performs.
- `docs/interop.md` covers what each test proves plus manual bench
  procedures for the three hardware-only checks (bit-identical adverts
  vs MCU, 2-hop flood routing, end-to-end crypto with the mobile app).
- CI runs `make test` + `make interop` on every push and PR.

### Fixed
- Release notes install snippet now strips the leading `v` from the
  tag so the suggested `wget` URL matches the published artefact name.

## [1.0.0] — 2026-05-17

### Added
- `mesh::Radio` over the pymc_usb wire protocol v0.7 (`LinuxTcpRadio`).
- Full `mesh::Mesh` + `CommonCLICallbacks` repeater (`LinuxRepeaterMesh`):
  adverts, ACL, loop detection, region scoping, region save, remote LoRa
  admin (login, stats, neighbours, CLI bridge).
- HTTP/JSON configurator on `:8080` with 4-tab UI (Radio / Node /
  Settings / Terminal), sticky save bar, live status panel auto-refresh
  every 3s, CLI terminal with localStorage history.
- Identity persistence inside `/etc/Meshcore-Linux/config.json` — lazy
  generation via `getrandom(2)`, atomic write, survives upgrades and
  reinstalls.
- Tag-prefix (`XX|…`) admin CLI reflection so MeshCore mobile and
  desktop clients work over LoRa unchanged.
- Packet log under `/var/lib/Meshcore-Linux/packets.log`.
- systemd unit hardened (`ProtectSystem=strict`, `ReadWritePaths`,
  `StateDirectory`, `Restart=always`).
- Debian packaging (`debian/`), CI on GitHub Actions, release workflow
  that publishes `amd64` + `arm64` `.deb` artefacts to GitHub Releases.
- MIT license + attribution to MeshCore upstream and bundled libraries
  (rweather/Crypto, ed25519-donna, CayenneLPP, cpp-httplib, nlohmann/json).

[1.0.1]: https://github.com/itk80/meshcore-linux/releases/tag/v1.0.1
[1.0.0]: https://github.com/itk80/meshcore-linux/releases/tag/v1.0.0
