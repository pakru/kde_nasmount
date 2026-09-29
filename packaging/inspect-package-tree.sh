#!/bin/bash
# Rootless inspection of an extracted DEB/RPM data tree.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

root=${1:-}
family=${2:-}
[ -d "$root/usr" ] && [[ "$family" =~ ^(deb|rpm)$ ]] || {
    echo "Usage: $0 EXTRACTED_ROOT deb|rpm" >&2
    exit 2
}

manifest="$root/usr/share/nasmount/cleanup-manifest.txt"
family_file="$root/usr/share/nasmount/package-family"
[ -f "$manifest" ] && [ ! -L "$manifest" ]
[ -f "$family_file" ] && [ ! -L "$family_file" ]
[ "$(tr -d '[:space:]' < "$family_file")" = "$family" ]
[ "$(stat -c '%a' "$manifest")" = 644 ]
[ "$(stat -c '%a' "$family_file")" = 644 ]

actual=$(mktemp)
expected=$(mktemp)
trap 'rm -f -- "$actual" "$expected"' EXIT
find "$root/usr" -type f -printf '/usr/%P\n' | LC_ALL=C sort -u > "$actual"
sed '/^[[:space:]]*$/d' "$manifest" | LC_ALL=C sort -u > "$expected"
if ! cmp -s "$actual" "$expected"; then
    echo "ERROR: cleanup manifest does not equal the package's regular-file set" >&2
    diff -u "$expected" "$actual" >&2 || true
    exit 1
fi

if find "$root/usr" -type f -links +1 -print -quit | grep -q .; then
    echo "ERROR: package contains a hard-linked regular file" >&2
    exit 1
fi
while IFS= read -r link; do
    case "${link#"$root"}" in
        /usr/lib/.build-id/*) ;;
        *) echo "ERROR: unexpected package symlink: ${link#"$root"}" >&2; exit 1 ;;
    esac
done < <(find "$root/usr" -type l -print)

if find "$root/usr" -perm /6000 -print -quit | grep -q .; then
    echo "ERROR: package contains a setuid/setgid path" >&2
    exit 1
fi
if find "$root" -path '*/usr/local*' -print -quit | grep -q .; then
    echo "ERROR: package installs beneath /usr/local" >&2
    exit 1
fi

[ -x "$root/usr/bin/nasmount-uninstall" ]
[ -x "$root/usr/bin/nasmount" ]
[ -f "$root/usr/share/applications/nasmount-manager.desktop" ]
guard=$(find "$root/usr" -type f -name nasmount-package-guard -print -quit)
[ -n "$guard" ] && [ -x "$guard" ]
[ "$(stat -c '%a' "$root/usr/share/kio/servicemenus/nasmount.desktop")" = 755 ]

while IFS= read -r candidate; do
    if file -b "$candidate" | grep -q '^ELF '; then
        if readelf -d "$candidate" 2>/dev/null | grep -E '(RPATH|RUNPATH)' | grep -qE '(build|/tmp/)'; then
            echo "ERROR: build-tree runtime path in ${candidate#"$root"}" >&2
            exit 1
        fi
    fi
done < <(find "$root/usr" -type f -print)

echo "$family package tree passed rootless inspection."
