#!/bin/bash
# Rootless release/package metadata gates.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/packaging/lib.sh"
read_release_contract "$repo_root"

[ "$NASMOUNT_DEB" = "nasmount_${NASMOUNT_VERSION}-${NASMOUNT_RELEASE}_amd64.deb" ]
[ "$NASMOUNT_RPM" = "nasmount-${NASMOUNT_VERSION}-${NASMOUNT_RELEASE}.fc44.x86_64.rpm" ]
[ "$NASMOUNT_DEB_RELEASE_ASSET" = "nasmount-amd64-${NASMOUNT_VERSION}.deb" ]
[ "$NASMOUNT_RPM_RELEASE_ASSET" = "nasmount-fedora44-x86_64-${NASMOUNT_VERSION}.rpm" ]

while IFS= read -r script; do
    bash -n "$script"
done < <(find "$repo_root/packaging" "$repo_root/tests" -type f -name '*.sh' -print)
for script in nasmount.preinst nasmount.postinst nasmount.postrm; do
    sh -n "$repo_root/packaging/debian/$script"
done
sh -n "$repo_root/packaging/debian/nasmount.prerm.in"

grep -Fq 'Rules-Requires-Root: no' "$repo_root/packaging/debian/control"
grep -Fq 'build-deb.sh must run as an unprivileged build user' "$repo_root/packaging/build-deb.sh"
grep -Fq 'build-rpm.sh must run as an unprivileged build user' "$repo_root/packaging/build-rpm.sh"
grep -Fq 'DNASMOUNT_PACKAGE_FAMILY=deb' "$repo_root/packaging/debian/rules"
grep -Fq 'DNASMOUNT_PACKAGE_FAMILY=rpm' "$repo_root/packaging/rpm/nasmount.spec.in"
grep -Fq '%{_libdir}/libexec/nasmount-package-guard || exit $?' "$repo_root/packaging/rpm/nasmount.spec.in"
grep -Fq '/usr/lib/@DEB_HOST_MULTIARCH@/libexec/nasmount-package-guard' \
    "$repo_root/packaging/debian/nasmount.prerm.in"
grep -Fq 'cp "packages/$NASMOUNT_DEB" "release/$NASMOUNT_DEB_RELEASE_ASSET"' \
    "$repo_root/.github/workflows/release.yml"
grep -Fq 'cp "packages/$NASMOUNT_RPM" "release/$NASMOUNT_RPM_RELEASE_ASSET"' \
    "$repo_root/.github/workflows/release.yml"
grep -Fq 'name: Check out validated tag for publication' \
    "$repo_root/.github/workflows/release.yml"
grep -A5 -F 'name: Check out validated tag for publication' \
    "$repo_root/.github/workflows/release.yml" | grep -Fq 'persist-credentials: false'
grep -Fq -- '--no-autoremove nasmount' "$repo_root/packaging/nasmount-uninstall.sh"
for workflow in "$repo_root/.github/workflows/ci.yml" "$repo_root/.github/workflows/release.yml"; do
    [ "$(grep -Fc 'dnf remove -y --no-autoremove nasmount' "$workflow")" -eq 2 ]
    grep -Fq 'rpm-packages-before-blocked-removal.txt' "$workflow"
    grep -Fq 'rpm-packages-after-blocked-removal.txt' "$workflow"
    if grep -Fq 'dnf remove -y nasmount' "$workflow"; then
        echo "ERROR: Fedora removal permits dependency autoremove in $workflow" >&2
        exit 1
    fi
done

if grep -R -n -E 'Fedora 43|fedora-43|fc43' "$repo_root/packaging"; then
    echo "ERROR: retired Fedora 43 target remains in packaging" >&2
    exit 1
fi

for workflow in "$repo_root/.github/workflows/ci.yml" "$repo_root/.github/workflows/release.yml"; do
    [ -f "$workflow" ]
    if grep -E 'uses: [^@[:space:]]+@(v[0-9]+|main|master)$' "$workflow"; then
        echo "ERROR: GitHub Action is not pinned to a commit in $workflow" >&2
        exit 1
    fi
    while IFS= read -r use; do
        [[ "$use" =~ @[0-9a-f]{40}$ ]] || {
            echo "ERROR: malformed Action pin in $workflow: $use" >&2
            exit 1
        }
    done < <(sed -n -E 's/^[[:space:]]*uses:[[:space:]]*([^#[:space:]]+).*/\1/p' "$workflow")
done

ci_jobs=$(sed -n '/^jobs:/,$p' "$repo_root/.github/workflows/ci.yml" \
    | sed -n -E 's/^  ([a-z_]+):$/\1/p' | sort)
expected_ci=$(printf '%s\n' build_deb build_rpm ci_success smoke_packages validate_packaging verify_artifact_set | sort)
[ "$ci_jobs" = "$expected_ci" ]
release_jobs=$(sed -n '/^jobs:/,$p' "$repo_root/.github/workflows/release.yml" \
    | sed -n -E 's/^  ([a-z_]+):$/\1/p' | sort)
expected_release=$(printf '%s\n' attest_and_publish build_deb_release build_rpm_release smoke_release_packages validate_release verify_release_set | sort)
[ "$release_jobs" = "$expected_release" ]

if grep -R -n -E 'Fedora 43|fedora-43|fc43' "$repo_root/.github"; then
    echo "ERROR: retired Fedora 43 target remains in workflows" >&2
    exit 1
fi

echo "Packaging metadata gates passed."
