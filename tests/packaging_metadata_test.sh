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
for script in nasmount.postinst nasmount.postrm; do
    sh -n "$repo_root/packaging/debian/$script"
done
# @VERSION@/@DEB_HOST_MULTIARCH@ are ordinary words to `sh -n`, so the
# templates parse without substitution.
for template in nasmount.preinst.in nasmount.prerm.in; do
    sh -n "$repo_root/packaging/debian/$template"
done

# --- upgrade contract (deb plan §2) ------------------------------------------
preinst="$repo_root/packaging/debian/nasmount.preinst.in"
[ -f "$preinst" ]
if [ -e "$repo_root/packaging/debian/nasmount.preinst" ]; then
    echo "ERROR: a non-templated nasmount.preinst would shadow the generated one" >&2
    exit 1
fi
grep -Fq 'dpkg --compare-versions' "$preinst"
grep -Fq 'MIN_UPGRADABLE_VERSION' "$preinst"
if grep -Fq 'does not yet support in-place package upgrades' "$preinst"; then
    echo "ERROR: the DEB still refuses in-place upgrades" >&2
    exit 1
fi
grep -Fq 'nasmount.preinst.in' "$repo_root/packaging/build-deb.sh"

# Rule §2.1: an upgrade must never run the removal guard. `remove)` is the
# only case that may reach it.
if grep -Eq '^\s*upgrade\)' "$repo_root/packaging/debian/nasmount.prerm.in"; then
    echo "ERROR: prerm runs the package guard on upgrade" >&2
    exit 1
fi

postrm="$repo_root/packaging/debian/nasmount.postrm"
grep -Eq '^\s*purge\)' "$postrm"
grep -Fq "postrm called with unknown argument" "$postrm"

grep -Fq 'dh_installsystemd --restart-after-upgrade' "$repo_root/packaging/debian/rules"

# --- authorship (deb plan §6) ------------------------------------------------
for metadata in debian/control debian/copyright debian/changelog.in; do
    grep -Fq 'Pavel Krutikhin <krutikhin92@gmail.com>' "$repo_root/packaging/$metadata"
done
if grep -R -n -E 'Krupets|pakru@users\.noreply\.github\.com' "$repo_root/packaging" \
    "$repo_root/src" "$repo_root/README.md"; then
    echo "ERROR: stale or malformed maintainer identity remains" >&2
    exit 1
fi

grep -Fq 'Rules-Requires-Root: no' "$repo_root/packaging/debian/control"
grep -Fq 'build-deb.sh must run as an unprivileged build user' "$repo_root/packaging/build-deb.sh"
grep -Fq 'build-rpm.sh must run as an unprivileged build user' "$repo_root/packaging/build-rpm.sh"
grep -Fq 'DNASMOUNT_PACKAGE_FAMILY=deb' "$repo_root/packaging/debian/rules"
grep -Fq 'DNASMOUNT_PACKAGE_FAMILY=rpm' "$repo_root/packaging/rpm/nasmount.spec.in"
# Both families now tear managed shares down themselves rather than refusing
# removal. The guard binary still ships and is still a useful diagnostic, but
# it must not appear in any maintainer scriptlet; package_scripts_test.sh
# checks the RPM scriptlet bodies, this checks the DEB prerm.
grep -Fq '@CHANGELOG_DATE@' "$repo_root/packaging/rpm/nasmount.spec.in"
grep -Fq 'LC_ALL=C TZ=UTC0' "$repo_root/packaging/build-rpm.sh"
grep -Fq '@CHANGELOG_DATE@' "$repo_root/packaging/build-rpm.sh"
# The DEB no longer refuses removal: prerm tears managed shares down itself
# and postrm purges their state. The guard binary stays installed and is still
# authoritative for the RPM's %preun (asserted above); it must not reappear in
# the DEB prerm, where it would refuse the removal this design now performs.
if grep -Fq 'nasmount-package-guard' "$repo_root/packaging/debian/nasmount.prerm.in"; then
    echo "ERROR: the DEB prerm refuses removal again; teardown is its job now" >&2
    exit 1
fi
grep -Fq 'nasmount_disable_units' "$repo_root/packaging/debian/nasmount.prerm.in"
grep -Fq 'cp "packages/$NASMOUNT_DEB" "release/$NASMOUNT_DEB_RELEASE_ASSET"' \
    "$repo_root/.github/workflows/release.yml"
grep -Fq 'cp "packages/$NASMOUNT_RPM" "release/$NASMOUNT_RPM_RELEASE_ASSET"' \
    "$repo_root/.github/workflows/release.yml"
grep -Fq 'name: Check out validated tag for publication' \
    "$repo_root/.github/workflows/release.yml"
grep -A5 -F 'name: Check out validated tag for publication' \
    "$repo_root/.github/workflows/release.yml" | grep -Fq 'persist-credentials: false'
grep -Fq -- '--no-autoremove nasmount' "$repo_root/packaging/nasmount-uninstall.sh"
# Every Fedora removal anywhere in the workflows must carry --no-autoremove:
# DNF can keep removing unused dependencies after a failed transaction, leaving
# nasmount installed without Qt. Counting is the point -- a bare `dnf remove`
# added later must not slip past, so compare against the total.
for workflow in "$repo_root/.github/workflows/ci.yml" "$repo_root/.github/workflows/release.yml"; do
    guarded=$(grep -Fc 'dnf remove -y --no-autoremove nasmount' "$workflow" || true)
    total=$(grep -Ec 'dnf remove [^|]*nasmount' "$workflow" || true)
    [ "$guarded" -eq "$total" ] && [ "$guarded" -ge 1 ] || {
        echo "ERROR: $workflow has $total Fedora removals but only $guarded" >&2
        echo "       carry --no-autoremove" >&2
        exit 1
    }
    grep -Fq 'rpm-packages-before-removal.txt' "$workflow"
    grep -Fq 'rpm-packages-after-removal.txt' "$workflow"
    # The snapshot comparison must be a cmp against an expected set. Piping
    # `diff` into `grep -v '^[<>]'` discards every content line, so such a
    # check passes however many dependencies DNF removed.
    grep -Fq 'rpm-packages-expected-after.txt' "$workflow"
    if grep -Eq "diff logs/rpm-packages-before-removal" "$workflow" \
        && grep -Eq "grep -Ev .\^\[0-9<>-\]" "$workflow"; then
        echo "ERROR: $workflow uses a diff|grep snapshot check that cannot fail" >&2
        exit 1
    fi
done

# --- credential autofill build dependencies ----------------------------------
# Six hand-maintained dependency lists have to agree, and a build that is
# missing one of these fails only in the distribution it was forgotten in:
# without the KIO development package nasmount-dialog does not compile at all,
# and without the QML runtime modules shareform_qml_test cannot load the form
# it exists to test (ctest runs inside both package builds). The deb upgrade
# job builds too, so it needs them as well; the release workflow has no
# upgrade job, which is why the counts differ.
deb_kio_lists=(
    "$repo_root/packaging/debian/control"
    "$repo_root/packaging/build-in-container.sh"
)
for list in "${deb_kio_lists[@]}"; do
    grep -Fq 'libkf6kio-dev' "$list" || {
        echo "ERROR: $(basename "$list") is missing libkf6kio-dev" >&2
        exit 1
    }
done
grep -Fq 'kf6-kio-devel' "$repo_root/packaging/rpm/nasmount.spec.in"
grep -Fq 'kf6-kio-devel' "$repo_root/packaging/build-in-container.sh"

# Every apt/dnf dependency block in the workflows that builds the project must
# carry them. Counting the blocks is the point: a job added later without them
# would otherwise fail only once someone reads the log.
deb_blocks_ci=$(grep -Fc 'qt6-base-dev' "$repo_root/.github/workflows/ci.yml")
deb_kio_ci=$(grep -Fc 'libkf6kio-dev' "$repo_root/.github/workflows/ci.yml")
deb_qml_ci=$(grep -Fc 'qml6-module-qtquick-dialogs' "$repo_root/.github/workflows/ci.yml")
[ "$deb_blocks_ci" -eq "$deb_kio_ci" ] && [ "$deb_blocks_ci" -eq "$deb_qml_ci" ] || {
    echo "ERROR: ci.yml has $deb_blocks_ci Ubuntu build blocks but $deb_kio_ci with" >&2
    echo "       libkf6kio-dev and $deb_qml_ci with the QML runtime modules" >&2
    exit 1
}
rpm_blocks_ci=$(grep -Fc 'qt6-qtbase-devel' "$repo_root/.github/workflows/ci.yml")
rpm_kio_ci=$(grep -Fc 'kf6-kio-devel' "$repo_root/.github/workflows/ci.yml")
[ "$rpm_blocks_ci" -eq "$rpm_kio_ci" ] || {
    echo "ERROR: ci.yml has $rpm_blocks_ci Fedora build blocks but only $rpm_kio_ci" >&2
    echo "       with kf6-kio-devel" >&2
    exit 1
}
for dependency in libkf6kio-dev qml6-module-qtquick-dialogs kf6-kio-devel; do
    grep -Fq "$dependency" "$repo_root/.github/workflows/release.yml" || {
        echo "ERROR: release.yml is missing $dependency" >&2
        exit 1
    }
done

# The password service is a weak dependency in both families, never a hard
# one: a host without it must still install nasmount and still mount shares
# with a hand-typed credential. It carries different weight in each. On
# Fedora it is the only thing that installs the service, since RPM's requires
# are soname-based and pull kf6-kio-core-libs alone. On Ubuntu, Debian's
# libkf6kiocore6 symbols file already adds kio6 to ${shlibs:Depends}, so this
# is a restatement that survives a change to that file.
grep -Eq '^Recommends:.*kio6' "$repo_root/packaging/debian/control"
grep -Eq '^Recommends: +kf6-kio-core' "$repo_root/packaging/rpm/nasmount.spec.in"
if grep -Eq '^Requires: +kf6-kio-core' "$repo_root/packaging/rpm/nasmount.spec.in"; then
    echo "ERROR: the password service must not be a hard RPM requirement" >&2
    exit 1
fi

# `make deb` / `make rpm` must use the exact images CI uses. A local build
# against a different digest proves nothing about the CI result, and the pins
# live in two files that nothing else keeps in step.
container_helper="$repo_root/packaging/build-in-container.sh"
[ -x "$container_helper" ]
for family in DEB:ubuntu RPM:fedora; do
    var=${family%%:*}
    distro=${family##*:}
    helper_pin=$(sed -n -E "s/^readonly NASMOUNT_${var}_IMAGE=(.*)$/\1/p" "$container_helper")
    workflow_pin=$(grep -oE "image: ${distro}:[^[:space:]]+" \
        "$repo_root/.github/workflows/ci.yml" | head -1 | sed 's/^image: //')
    [ -n "$helper_pin" ] && [ -n "$workflow_pin" ] || {
        echo "ERROR: could not read the $var image pin from both files" >&2
        exit 1
    }
    [ "$helper_pin" = "$workflow_pin" ] || {
        echo "ERROR: $var image pin drifted from ci.yml" >&2
        echo "       build-in-container.sh: $helper_pin" >&2
        echo "       ci.yml:                $workflow_pin" >&2
        exit 1
    }
done
grep -Fq 'build-in-container.sh deb' "$repo_root/Makefile"
grep -Fq 'build-in-container.sh rpm' "$repo_root/Makefile"

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

# Listing a job in expected_ci proves it exists, not that it gates anything:
# ci_success carries its own hand-maintained needs/env/loop. All three must
# name upgrade_deb or an upgrade failure leaves the required check green.
ci_success_block=$(sed -n '/^  ci_success:/,$p' "$repo_root/.github/workflows/ci.yml")
for family in DEB RPM; do
    job="upgrade_$(printf '%s' "$family" | tr '[:upper:]' '[:lower:]')"
    grep -Fq "$job" <<<"$ci_success_block"
    grep -Fq "UPGRADE_$family: \${{ needs.$job.result }}" <<<"$ci_success_block"
    grep -Fq "\"\$UPGRADE_$family\"" <<<"$ci_success_block"
done

ci_jobs=$(sed -n '/^jobs:/,$p' "$repo_root/.github/workflows/ci.yml" \
    | sed -n -E 's/^  ([a-z_]+):$/\1/p' | sort)
expected_ci=$(printf '%s\n' build_deb build_rpm ci_success smoke_packages upgrade_deb upgrade_rpm validate_packaging verify_artifact_set | sort)
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
