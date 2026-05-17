# Changelog

All notable changes to this project will be documented in this file.
Synced with `debian/changelog` for the package version.

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

[1.0.0]: https://github.com/itk80/meshcore-linux/releases/tag/v1.0.0
