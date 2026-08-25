#!/bin/bash
# Build the Fedora 44 x86_64 RPM in an isolated rpmbuild tree.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ "$(id -u)" -ne 0 ] || {
    echo "ERROR: build-rpm.sh must run as an unprivileged build user." >&2
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
[ "$(rpm --eval '%{fedora}')" = 44 ] || {
    echo "ERROR: the initial RPM target is Fedora 44 only." >&2
    exit 1
}
[ "$(uname -m)" = x86_64 ] || {
    echo "ERROR: the initial RPM target is x86_64 only." >&2
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
top_dir="$work_dir/rpmbuild"
mkdir -p "$top_dir/BUILD" "$top_dir/BUILDROOT" "$top_dir/RPMS" \
    "$top_dir/SOURCES" "$top_dir/SPECS" "$top_dir/SRPMS"
source_dir="$work_dir/nasmount-$version"
mkdir -p -- "$source_dir"
tar --exclude-vcs --exclude='./build' --exclude='./build-*' --exclude='./dist' \
    -cf - -C "$repo_root" . | tar -xf - -C "$source_dir"
tar -czf "$top_dir/SOURCES/nasmount-$version.tar.gz" -C "$work_dir" "nasmount-$version"
build_epoch=${SOURCE_DATE_EPOCH:-}
if ! [[ "$build_epoch" =~ ^[0-9]+$ ]]; then
    build_epoch=$(git -C "$repo_root" show -s --format=%ct HEAD 2>/dev/null || date +%s)
fi
export SOURCE_DATE_EPOCH=$build_epoch
# RPM changelog headers require English weekday/month names, and the local
# timezone can move an epoch onto the adjacent calendar day. Without the pin a
# ru_RU host emits "Пт авг 15 2025", which RPM rejects.
changelog_date=$(LC_ALL=C TZ=UTC0 date --date="@$build_epoch" '+%a %b %d %Y')
sed -e "s/@VERSION@/$version/g" -e "s/@RELEASE@/$release/g" \
    -e "s/@CHANGELOG_DATE@/$changelog_date/g" \
    "$repo_root/packaging/rpm/nasmount.spec.in" > "$top_dir/SPECS/nasmount.spec"

rpmbuild --define "_topdir $top_dir" -ba "$top_dir/SPECS/nasmount.spec"
artifact="$top_dir/RPMS/x86_64/nasmount-${version}-${release}.fc44.x86_64.rpm"
[ -f "$artifact" ] || {
    echo "ERROR: expected package was not produced: $artifact" >&2
    exit 1
}
cp -- "$artifact" "$output_dir/"
printf '%s\n' "$output_dir/$(basename "$artifact")"
