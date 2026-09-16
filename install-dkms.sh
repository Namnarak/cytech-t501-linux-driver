#!/usr/bin/env bash
set -euo pipefail
VERSION="1.0.6"
NAME="cytech-t501"
SRC="/usr/src/${NAME}-${VERSION}"
if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo ./install-dkms.sh" >&2
  exit 1
fi
install -d "$SRC"
install -m 0644 hid-cytech-t501.c Makefile dkms.conf "$SRC/"
install -m 0644 99-cytech-t501-pad.rules /etc/udev/rules.d/99-cytech-t501-pad.rules
udevadm control --reload-rules
dkms remove -m "$NAME" -v "$VERSION" --all 2>/dev/null || true
dkms add -m "$NAME" -v "$VERSION"
dkms build -m "$NAME" -v "$VERSION"
dkms install -m "$NAME" -v "$VERSION"
modprobe hid-cytech-t501
echo "Installed ${NAME}/${VERSION}. Frame buttons are exposed as native tablet-pad buttons. Reconnect the tablet if it is already plugged in."
