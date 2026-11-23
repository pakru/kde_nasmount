#!/bin/bash
# Verify the two native release packages and generate checksums/manifest.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/packaging/lib.sh"
read_release_contract "$repo_root"

package_arg=${1:-}
output_arg=${2:-}
[ -d "$package_arg" ] && [ -n "$output_arg" ] || {
    echo "Usage: $0 PACKAGE_DIRECTORY EMPTY_OUTPUT_DIRECTORY" >&2
    exit 2
}
package_dir="$(cd "$package_arg" && pwd)"
mkdir -p "$output_arg"
output_dir="$(cd "$output_arg" && pwd)"
if find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
    echo "ERROR: output directory must be empty: $output_dir" >&2
    exit 1
fi

mapfile -t package_files < <(find "$package_dir" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) -printf '%f\n' | sort)
[ "${#package_files[@]}" -eq 2 ]
[ "${package_files[0]}" = "$NASMOUNT_RPM" ] || [ "${package_files[0]}" = "$NASMOUNT_DEB" ]
[ -f "$package_dir/$NASMOUNT_DEB" ]
[ -f "$package_dir/$NASMOUNT_RPM" ]

[ "$(dpkg-deb -f "$package_dir/$NASMOUNT_DEB" Package)" = nasmount ]
[ "$(dpkg-deb -f "$package_dir/$NASMOUNT_DEB" Version)" = "$NASMOUNT_VERSION-$NASMOUNT_RELEASE" ]
[ "$(dpkg-deb -f "$package_dir/$NASMOUNT_DEB" Architecture)" = amd64 ]
[ "$(rpm -qp --qf '%{NAME}' "$package_dir/$NASMOUNT_RPM")" = nasmount ]
[ "$(rpm -qp --qf '%{VERSION}' "$package_dir/$NASMOUNT_RPM")" = "$NASMOUNT_VERSION" ]
[ "$(rpm -qp --qf '%{RELEASE}' "$package_dir/$NASMOUNT_RPM")" = "$NASMOUNT_RELEASE.fc45" ]
[ "$(rpm -qp --qf '%{ARCH}' "$package_dir/$NASMOUNT_RPM")" = x86_64 ]

work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT
mkdir -p "$work_dir/deb" "$work_dir/rpm"
dpkg-deb -x "$package_dir/$NASMOUNT_DEB" "$work_dir/deb"
(cd "$work_dir/rpm" && rpm2cpio "$package_dir/$NASMOUNT_RPM" | cpio -idm --quiet)
"$repo_root/packaging/inspect-package-tree.sh" "$work_dir/deb" deb
"$repo_root/packaging/inspect-package-tree.sh" "$work_dir/rpm" rpm

(cd "$package_dir" && sha256sum "$NASMOUNT_DEB" "$NASMOUNT_RPM") \
    > "$output_dir/$NASMOUNT_CHECKSUMS"
commit=$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || true)
commit=${commit:-${GITHUB_SHA:-uncommitted}}
deb_sha=$(sha256sum "$package_dir/$NASMOUNT_DEB" | awk '{print $1}')
rpm_sha=$(sha256sum "$package_dir/$NASMOUNT_RPM" | awk '{print $1}')
jq -n \
    --arg version "$NASMOUNT_VERSION" \
    --arg release "$NASMOUNT_RELEASE" \
    --arg commit "$commit" \
    --arg deb "$NASMOUNT_DEB" --arg deb_sha256 "$deb_sha" \
    --arg deb_release_asset "$NASMOUNT_DEB_RELEASE_ASSET" \
    --arg rpm "$NASMOUNT_RPM" --arg rpm_sha256 "$rpm_sha" \
    --arg rpm_release_asset "$NASMOUNT_RPM_RELEASE_ASSET" \
    '{version:$version, packaging_release:$release, commit:$commit,
      artifacts:[
        {build_file:$deb, release_asset:$deb_release_asset,
         target:"ubuntu-26.04-amd64", sha256:$deb_sha256},
        {build_file:$rpm, release_asset:$rpm_release_asset,
         target:"fedora-45-x86_64", sha256:$rpm_sha256}
      ]}' > "$output_dir/$NASMOUNT_RELEASE_MANIFEST"

printf '%s\n%s\n' "$output_dir/$NASMOUNT_CHECKSUMS" \
    "$output_dir/$NASMOUNT_RELEASE_MANIFEST"
