#!/bin/bash
# Install one native package, inspect it, then remove it over seeded state.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CI and the release workflow run this same script, so a package cannot pass
# one smoke test and fail the other because the two inline copies drifted. It
# must run as root in a disposable container of the matching distribution:
# it installs, removes and purges real packages and writes under /etc.
#
# The container has no running systemd, so the unmount half of the removal
# scriptlets is short-circuited; that half belongs to the VM release checklist.

set -euo pipefail

family=${1:-}
version=${2:-}
package_dir=${3:-}
log_dir=${4:-}
[[ "$family" =~ ^(deb|rpm)$ ]] && [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
    && [ -d "$package_dir" ] && [ -n "$log_dir" ] || {
    echo "Usage: $0 deb|rpm MAJOR.MINOR.PATCH PACKAGE_DIR LOG_DIR" >&2
    exit 2
}
[ "$(id -u)" -eq 0 ] || {
    echo "ERROR: smoke-package-lifecycle.sh installs packages and must run as root." >&2
    exit 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
corpus="$repo_root/tests/golden/units/v0.1.0"
mkdir -p -- "$log_dir"

mount_unit=/etc/systemd/system/home-tester-mnt-media.mount
automount_unit=/etc/systemd/system/home-tester-mnt-media.automount
credential=/etc/nasmount/0123456789abcdef0123456789abcdef.cred

# Both families tear managed state down themselves: `apt remove` disarms and
# `apt purge` deletes; RPM has no split, so an erase does both.
seed_managed_state()
{
    install -d -m 0700 /etc/nasmount
    install -m 0600 /dev/null "$credential"
    install -m 0644 "$corpus/credentials.mount" "$mount_unit"
    install -m 0644 "$corpus/credentials.automount" "$automount_unit"
}

require_state_gone()
{
    local what="$1" leftover
    for leftover in "$mount_unit" "$automount_unit" /etc/nasmount; do
        if [ -e "$leftover" ]; then
            echo "ERROR: $what left $leftover behind" >&2
            exit 1
        fi
    done
}

shopt -s nullglob
packages=("$package_dir"/*."$family")
[ "${#packages[@]}" -eq 1 ] || {
    echo "ERROR: expected exactly one .$family in $package_dir, found ${#packages[@]}" >&2
    exit 1
}

if [ "$family" = deb ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        "${packages[0]}"
    dpkg-query -L nasmount > "$log_dir/owned-paths.txt"
    bash "$repo_root/packaging/smoke-installed-package.sh" deb "$version" \
        | tee "$log_dir/inspection.txt"
    seed_managed_state

    DEBIAN_FRONTEND=noninteractive apt-get remove -y nasmount
    ! dpkg-query -s nasmount 2>/dev/null \
        | grep -q '^Status: install ok installed$'
    # remove is not purge: state must survive for a later purge.
    [ -f "$mount_unit" ]
    [ -f "$credential" ]

    DEBIAN_FRONTEND=noninteractive apt-get purge -y nasmount
    require_state_gone purge
    if dpkg-query -W -f='${db:Status-Status}\n' nasmount 2>/dev/null | grep -q .; then
        echo 'ERROR: purge left the package in dpkg state' >&2
        exit 1
    fi
else
    dnf install -y --allowerasing --setopt=install_weak_deps=False "${packages[0]}"
    rpm -ql nasmount > "$log_dir/owned-paths.txt"
    bash "$repo_root/packaging/smoke-installed-package.sh" rpm "$version" \
        | tee "$log_dir/inspection.txt"
    seed_managed_state

    rpm -qa --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort \
        > "$log_dir/rpm-packages-before-removal.txt"
    dnf remove -y --no-autoremove nasmount
    ! rpm -q nasmount >/dev/null 2>&1
    rpm -qa --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort \
        > "$log_dir/rpm-packages-after-removal.txt"
    # --no-autoremove must leave every dependency in place. Build the expected
    # after-set by removing exactly the nasmount line and compare byte for
    # byte. A `diff | grep -v` check cannot work here: every content line diff
    # emits starts with '<' or '>', so the filter eats them all and the test
    # passes no matter what changed.
    [ "$(grep -c '^nasmount-' "$log_dir/rpm-packages-before-removal.txt")" -eq 1 ]
    grep -v '^nasmount-' "$log_dir/rpm-packages-before-removal.txt" \
        > "$log_dir/rpm-packages-expected-after.txt"
    cmp "$log_dir/rpm-packages-expected-after.txt" \
        "$log_dir/rpm-packages-after-removal.txt" || {
        echo 'ERROR: Fedora removal changed packages other than nasmount' >&2
        diff "$log_dir/rpm-packages-expected-after.txt" \
            "$log_dir/rpm-packages-after-removal.txt" >&2 || true
        exit 1
    }
    require_state_gone erase
fi
