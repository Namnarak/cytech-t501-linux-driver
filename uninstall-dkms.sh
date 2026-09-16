#!/usr/bin/env bash
set -euo pipefail

NAME="cytech-t501"
VERSION="$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)"/\1/p' dkms.conf)"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo ./uninstall-dkms.sh" >&2
  exit 1
fi

modprobe -r hid_cytech_t501 2>/dev/null || true
dkms remove -m "$NAME" -v "$VERSION" --all 2>/dev/null || true
rm -rf "/usr/src/${NAME}-${VERSION}"
rm -f /etc/udev/rules.d/99-cytech-t501-pad.rules
udevadm control --reload-rules

echo "Removed ${NAME}/${VERSION}."
