#!/bin/bash
# Shared, side-effect-free release naming helpers.
# SPDX-License-Identifier: GPL-3.0-or-later

read_release_contract()
{
    local repo_root=$1
    NASMOUNT_VERSION=$(tr -d '[:space:]' < "$repo_root/VERSION")
    NASMOUNT_RELEASE=$(tr -d '[:space:]' < "$repo_root/packaging/RELEASE")
    [[ "$NASMOUNT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || return 1
    [[ "$NASMOUNT_RELEASE" =~ ^[1-9][0-9]*$ ]] || return 1
    NASMOUNT_DEB="nasmount_${NASMOUNT_VERSION}-${NASMOUNT_RELEASE}_amd64.deb"
    NASMOUNT_RPM="nasmount-${NASMOUNT_VERSION}-${NASMOUNT_RELEASE}.fc45.x86_64.rpm"
    NASMOUNT_CHECKSUMS="nasmount-${NASMOUNT_VERSION}-SHA256SUMS"
    NASMOUNT_RELEASE_MANIFEST="nasmount-${NASMOUNT_VERSION}-release-manifest.json"
    NASMOUNT_DEB_RELEASE_ASSET="nasmount-amd64-${NASMOUNT_VERSION}.deb"
    NASMOUNT_RPM_RELEASE_ASSET="nasmount-fedora45-x86_64-${NASMOUNT_VERSION}.rpm"
    export NASMOUNT_VERSION NASMOUNT_RELEASE NASMOUNT_DEB NASMOUNT_RPM
    export NASMOUNT_CHECKSUMS NASMOUNT_RELEASE_MANIFEST
    export NASMOUNT_DEB_RELEASE_ASSET NASMOUNT_RPM_RELEASE_ASSET
}
