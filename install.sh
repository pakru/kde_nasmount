#!/bin/bash
#
# Builds and installs nasmount: the Dolphin service menu, the kcm_nasmount
# System Settings module, and the boot coordinator.
#
# Run WITHOUT sudo. The build happens as you; only `cmake --install` elevates.
#
# Re-running it over an existing source install replaces the program files in
# place and leaves shares, their credentials and configuration untouched;
# nothing is migrated. Do not run uninstall.sh first to "upgrade": it purges
# every share.

set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "Do not run this as root — the build should not run as root." >&2
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
    echo "ERROR: mount.cifs not found — install cifs-utils" >&2; exit 1; }

echo "Configuring..."
cmake -S "$SRC" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DNASMOUNT_PACKAGE_FAMILY=source \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

echo "Building..."
cmake --build "$BUILD" -j"$(nproc)"

echo "Running tests..."
for t in unitspec_test unitvalue_test verify_test helperinvoke_test store_test \
         mountactions_test mountmodel_test durablefs_test inventory_test operations_test \
         arming_test credentialstore_test cleanupvalidation_test packagestate_test smburl_test \
         credentiallookup_test shareform_qml_test goldenunits_test; do
    "$BUILD/bin/$t" || {
        echo "ERROR: $t failed — refusing to install." >&2
        exit 1
    }
done
bash "$SRC/tests/removed_api_gates.sh"
bash "$SRC/tests/qml_invokable_gate.sh"
bash "$SRC/tests/package_scripts_test.sh"
bash "$SRC/tests/packaging_metadata_test.sh"
ctest --test-dir "$BUILD" -R '^(appstreamtest|version_metadata)$' \
      --output-on-failure --no-tests=error

echo "Installing (authentication required)..."
sudo cmake --install "$BUILD"

echo
echo "Installed:"
sed 's/^/  /' "$BUILD/install_manifest.txt"


echo
echo "Enabling the boot coordinator (arms every share at boot)..."
sudo systemctl daemon-reload
sudo systemctl enable --now nasmount-boot.service

echo
echo "Refreshing Dolphin's service menu cache and System Settings' KCM cache..."
kbuildsycoca6 --noincremental 2>/dev/null || true

echo
echo "Done. Restart Dolphin and System Settings to pick up nasmount."
echo
echo "Then type smb://<your-nas>/ in the location bar, right-click a share"
echo "and choose 'Mount as Network Drive…' — or open System Settings →"
echo "Network Mounts to add one directly."
