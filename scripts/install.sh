#!/usr/bin/env bash
#
# install.sh — install Meshcore-Linux as a systemd service.
#
# Run from the repo root:
#   sudo ./scripts/install.sh
#
# Expects ./meshcore-linux (build with `make` first). On first install creates
# /etc/Meshcore-Linux/config.json from config/config.example.json; on upgrade
# the existing config is preserved.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_SRC="$ROOT/meshcore-linux"
UNIT_SRC="$ROOT/systemd/Meshcore-Linux.service"
CFG_SRC="$ROOT/config/config.example.json"

# Match the .deb path so the shared systemd unit's ExecStart=/usr/sbin/
# meshcore-linux works for both manual and packaged installs.
BIN_DST=/usr/sbin/meshcore-linux
UNIT_DST=/etc/systemd/system/Meshcore-Linux.service
CFG_DIR=/etc/Meshcore-Linux
CFG_DST="$CFG_DIR/config.json"

if [[ $EUID -ne 0 ]]; then
  echo "must be run as root (try: sudo $0)" >&2
  exit 1
fi
if [[ ! -x "$BIN_SRC" ]]; then
  echo "binary not found at $BIN_SRC — run 'make' first" >&2
  exit 1
fi

echo "[install] copying binary"
install -m 0755 "$BIN_SRC" "$BIN_DST"

echo "[install] creating $CFG_DIR"
mkdir -p "$CFG_DIR"
if [[ -f "$CFG_DST" ]]; then
  echo "[install] $CFG_DST already exists — leaving it alone"
else
  install -m 0644 "$CFG_SRC" "$CFG_DST"
  echo "[install] seeded $CFG_DST from example — review before enabling"
fi

echo "[install] installing systemd unit"
install -m 0644 "$UNIT_SRC" "$UNIT_DST"
systemctl daemon-reload

echo "[install] enabling + (re)starting Meshcore-Linux.service"
systemctl enable Meshcore-Linux.service
systemctl restart Meshcore-Linux.service

sleep 1
systemctl --no-pager --lines=15 status Meshcore-Linux.service || true

echo
echo "Done. Follow logs with:  journalctl -fu Meshcore-Linux"
echo "Config API:              http://$(hostname -I | awk '{print $1}'):8080/"
