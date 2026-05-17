# Contributing

PRs welcome. Quick rules:

- **Base branch**: `main`. No long-lived feature branches.
- **License**: MIT (see `LICENSE`). By submitting a PR you agree your code
  is licensed under the same terms.
- **Style**: match what's already in the file. We don't auto-format —
  keep diffs minimal.
- **Comments**: English only; keep them short and explain the WHY, not the
  WHAT.
- **No upstream forks here**: changes to MeshCore core go to
  https://github.com/ripplebiz/MeshCore, not this repo.
- **Test before pushing**: `make` must succeed; run the binary against a
  real or stub modem and confirm the systemd unit starts.
- **Commits**: small, focused, imperative subject line ("Fix X", not
  "Fixed X"). Reference issues with `Fixes #N` when applicable.

## Reporting tested setups

When opening an issue or discussion about a deployed setup, please share:

- Hardware (host + modem board)
- Frequency / region
- LoRa params (sf, bw, cr)
- Approximate uptime / packet counts (`systemctl status` excerpts)

This helps build a picture of what works in the wild.
