#!/bin/bash
#
# Full nasmount uninstall. The installed cleanup coordinator runs while the
# KAuth helper and policy still exist, purges every verified artifact created
# by nasmount, then this script removes the installed program files.

set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "Do not run this as root." >&2
    exit 1
fi

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$SRC/build/install_manifest.txt"
CLEANUP=/usr/bin/nasmount-cleanup

if [ ! -f "$MANIFEST" ]; then
    echo "ERROR: $MANIFEST not found; refusing an unscoped uninstall." >&2
    echo "Re-run install.sh to create a complete, validated install manifest." >&2
    exit 1
fi
if [ ! -x "$CLEANUP" ]; then
    echo "ERROR: $CLEANUP is not installed." >&2
    echo "Install this version first; cleanup must run before the helper is removed." >&2
    exit 1
fi

# The manifest is user-writable, so resolve it into a checked array before
# any privileged operation. The installed cleanup executable independently
# applies the same finite allowlist before it requests purge authorization.
allowed_pattern='^/usr/(bin/nasmount(-(dialog|cleanup|uninstall))?'
allowed_pattern+='|((lib/[^/]+/libexec|libexec)/kf6/kauth|lib/kf6/kauth/libexec)/nasmount-helper'
allowed_pattern+='|(lib/[^/]+/libexec|lib64/libexec|libexec)/nasmount-(boot|package-guard)'
allowed_pattern+='|(lib/[^/]+|lib64)/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount\.so'
allowed_pattern+='|lib/systemd/system/nasmount-boot\.service'
allowed_pattern+='|lib/systemd/system-preset/90-nasmount\.preset'
allowed_pattern+='|share/applications/(kcm_nasmount|nasmount-manager)\.desktop'
allowed_pattern+='|share/polkit-1/actions/io\.github\.pakru\.nasmount\.policy'
allowed_pattern+='|share/dbus-1/system\.d/io\.github\.pakru\.nasmount\.conf'
allowed_pattern+='|share/dbus-1/system-services/io\.github\.pakru\.nasmount\.service'
allowed_pattern+='|share/kio/servicemenus/nasmount\.desktop'
allowed_pattern+='|share/nasmount/(cleanup-manifest\.txt|package-family)'
allowed_pattern+='|share/(doc|licenses)/nasmount/LICENSE'
allowed_pattern+='|share/doc/nasmount/(changelog\.Debian\.gz|copyright))$'

targets=()
declare -A seen=()
# `|| [ -n "$line" ]` is required, not defensive: CMake writes
# install_manifest.txt with no trailing newline, so a plain `read` loop
# discards the final path -- silently leaving the last-installed file behind
# on every uninstall.
while IFS= read -r line || [ -n "$line" ]; do
    [ -n "$line" ] || continue
    if [[ ! "$line" =~ $allowed_pattern ]]; then
        echo "ERROR: manifest contains an unexpected path: $line" >&2
        exit 1
    fi
    if [ -n "${seen[$line]:-}" ]; then
        echo "ERROR: manifest contains a duplicate path: $line" >&2
        exit 1
    fi
    seen["$line"]=1
    targets+=("$line")
done < "$MANIFEST"

if [ ${#targets[@]} -eq 0 ]; then
    echo "ERROR: manifest lists nothing to remove." >&2
    exit 1
fi

echo "This will permanently remove:"
echo "  all nasmount-managed systemd share definitions"
echo "  System and Session credential files and runtime records"
echo "  ~/.config/nasmountrc"
echo "  all installed nasmount binaries, services, policy, and UI files"
echo
echo "Mount-point directories will be left in place."
echo
printf '  %s\n' "${targets[@]}"
echo
read -r -p "Proceed with full purge and uninstall? [y/N] " answer
[ "$answer" = "y" ] || [ "$answer" = "Y" ] || { echo "Aborted."; exit 0; }

echo "Purging managed shares and application data (authentication required)..."
"$CLEANUP" --manifest "$MANIFEST"

# With all managed definitions gone, stop/disable coordinators before their
# unit files and binaries disappear.
sudo systemctl disable --now nasmount-boot.service >/dev/null 2>&1 || true

echo "Removing installed files..."
sudo rm -fv -- "${targets[@]}"
sudo rmdir /usr/share/nasmount /usr/share/doc/nasmount /usr/share/licenses/nasmount 2>/dev/null || true

systemctl --user daemon-reload
sudo systemctl daemon-reload
kbuildsycoca6 --noincremental 2>/dev/null || true

echo
echo "Done. All nasmount artifacts were purged; mount-point directories were left in place."
