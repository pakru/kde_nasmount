#!/bin/bash
# Build the Ubuntu 26.04 amd64 binary package in an isolated source tree.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ "$(id -u)" -ne 0 ] || {
    echo "ERROR: build-deb.sh must run as an unprivileged build user." >&2
    exit 1
}
output_arg=${1:-}
[ -n "$output_arg" ] || {
    echo "Usage: $0 EMPTY_OUTPUT_DIRECTORY" >&2
    exit 2
}

version=$(tr -d '[:space:]' < "$repo_root/VERSION")
release=$(tr -d '[:space:]' < "$repo_root/packaging/RELEASE")
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
[[ "$release" =~ ^[1-9][0-9]*$ ]]
# shellcheck disable=SC1091 -- fixed distribution identity file
source /etc/os-release
[ "${ID:-}" = ubuntu ] && [ "${VERSION_ID:-}" = 26.04 ] || {
    echo "ERROR: the initial DEB target is Ubuntu 26.04 only." >&2
    exit 1
}
[ "$(dpkg-architecture -qDEB_HOST_ARCH)" = amd64 ] || {
    echo "ERROR: the initial DEB target is amd64 only." >&2
    exit 1
}

mkdir -p -- "$output_arg"
output_dir="$(cd "$output_arg" && pwd)"
if find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
    echo "ERROR: output directory must be empty: $output_dir" >&2
    exit 1
fi

work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT
source_dir="$work_dir/nasmount-$version"
mkdir -p -- "$source_dir"
tar --exclude-vcs --exclude='./build' --exclude='./build-*' --exclude='./dist' \
    -cf - -C "$repo_root" . | tar -xf - -C "$source_dir"
cp -a -- "$source_dir/packaging/debian" "$source_dir/debian"

build_epoch=${SOURCE_DATE_EPOCH:-}
if ! [[ "$build_epoch" =~ ^[0-9]+$ ]]; then
    build_epoch=$(git -C "$repo_root" show -s --format=%ct HEAD 2>/dev/null || date +%s)
fi
export SOURCE_DATE_EPOCH=$build_epoch
build_date=$(date --date="@$build_epoch" --rfc-email)
sed -e "s/@VERSION@/$version/g" \
    -e "s/@RELEASE@/$release/g" \
    -e "s/@DATE@/$build_date/g" \
    "$source_dir/packaging/debian/changelog.in" > "$source_dir/debian/changelog"
sed -e "s/@VERSION@/$version/g" -e "s/@RELEASE@/$release/g" \
    "$source_dir/packaging/debian/nasmount.preinst.in" > "$source_dir/debian/nasmount.preinst"
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
sed "s/@DEB_HOST_MULTIARCH@/$multiarch/g" \
    "$source_dir/packaging/debian/nasmount.prerm.in" > "$source_dir/debian/nasmount.prerm"
chmod 0755 "$source_dir/debian/rules" "$source_dir/debian/nasmount.preinst" \
    "$source_dir/debian/nasmount.postinst" "$source_dir/debian/nasmount.prerm" \
    "$source_dir/debian/nasmount.postrm"

(cd "$source_dir" && dpkg-buildpackage -b -us -uc)
artifact="$work_dir/nasmount_${version}-${release}_amd64.deb"
[ -f "$artifact" ] || {
    echo "ERROR: expected package was not produced: $artifact" >&2
    exit 1
}
cp -- "$artifact" "$output_dir/"
printf '%s\n' "$output_dir/$(basename "$artifact")"
