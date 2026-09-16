#!/usr/bin/env bash
set -euo pipefail

NAME="cytech-t501"
VERSION="$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)"/\1/p' dkms.conf)"
SRC="/usr/src/${NAME}-${VERSION}"

if [[ -z "$VERSION" ]]; then
  echo "Unable to read PACKAGE_VERSION from dkms.conf" >&2
  exit 1
fi

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo ./install-dkms.sh" >&2
  exit 1
fi

for cmd in dkms make modprobe udevadm; do
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "Missing required command: $cmd" >&2
    exit 1
  }
done

install -d "$SRC"
install -m 0644 hid-cytech-t501.c Makefile dkms.conf "$SRC/"
install -m 0644 99-cytech-t501-pad.rules /etc/udev/rules.d/99-cytech-t501-pad.rules
udevadm control --reload-rules

dkms remove -m "$NAME" -v "$VERSION" --all 2>/dev/null || true
dkms add -m "$NAME" -v "$VERSION"
dkms build -m "$NAME" -v "$VERSION"
dkms install -m "$NAME" -v "$VERSION"
modprobe hid-cytech-t501

echo "Installed ${NAME}/${VERSION}. Reconnect the tablet if it is already plugged in."
