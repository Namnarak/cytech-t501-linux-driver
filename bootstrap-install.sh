#!/usr/bin/env bash
set -euo pipefail

REPO="Namnarak/cytech-t501-linux-driver"
ASSET="cytech-t501-linux-driver.tar.gz"
URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for cmd in curl tar sudo; do
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "Missing required command: $cmd" >&2
    exit 1
  }
done

curl -fL --retry 3 -o "$TMP/$ASSET" "$URL"
tar -xzf "$TMP/$ASSET" -C "$TMP"
cd "$TMP/cytech-t501-linux-driver"
sudo ./install-dkms.sh
