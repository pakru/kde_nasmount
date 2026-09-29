#!/bin/bash
#
# Builds and installs KDE nasmount
#
# Run WITHOUT sudo. The build happens as you; only `cmake --install` elevates.
#
# Re-running it over an existing source install replaces the program files in
# place and leaves shares, their credentials and configuration untouched;
# nothing is migrated.

set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "Do not run this as root - the build should not run as root." >&2
    echo "It will prompt for authentication when it needs it." >&2
    exit 1
fi

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$SRC/build"

# Prefix /usr, not /usr/local: D-Bus only scans /usr/share/dbus-1 for system
# services, and polkit only scans /usr/share/polkit-1/actions. A /usr/local
# install would build fine and then silently fail to authenticate.
PREFIX=/usr

for tool in cmake g++ systemd-escape systemctl; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool not found" >&2; exit 1; }
done
[ -x /usr/sbin/mount.cifs ] || command -v mount.cifs >/dev/null || {
    echo "ERROR: mount.cifs not found - install cifs-utils" >&2; exit 1; }

echo "Configuring..."
cmake -S "$SRC" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DNASMOUNT_PACKAGE_FAMILY=source \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

echo "Building..."
cmake --build "$BUILD" -j"$(nproc)"

echo "Installing (authentication required)..."
sudo cmake --install "$BUILD"

echo
echo "Installed:"
sed 's/^/  /' "$BUILD/install_manifest.txt"


echo
echo "Enabling nasmount-boot.service..."
sudo systemctl daemon-reload
sudo systemctl enable --now nasmount-boot.service

echo
echo "Refreshing Dolphin's service menu cache and System Settings' KCM cache..."
kbuildsycoca6 --noincremental 2>/dev/null || true

echo
echo "Done. Restart Dolphin and System Settings to pick up nasmount."
