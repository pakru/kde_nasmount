#!/bin/bash
# Build one native package inside its matching target container.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# AGENTS.md requires packages to be built only in their matching target
# distribution. Almost no developer workstation *is* Ubuntu 26.04 or Fedora 44,
# so `make deb` / `make rpm` route through this: it runs the same pinned images
# CI uses, installs the same dependency list, and invokes the same
# build-deb.sh / build-rpm.sh as an unprivileged user. A local build therefore
# fails for the same reasons CI would, before the push.
#
# The working tree is copied in, not `git archive`d: uncommitted work is
# normally exactly what you want to test here.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Keep these digests identical to the ones in .github/workflows/ci.yml.
# tests/packaging_metadata_test.sh fails the suite if they drift, because a
# local build against a different image proves nothing about CI.
readonly NASMOUNT_DEB_IMAGE=ubuntu:26.04@sha256:7b202b0e2e0028c6250f5fcf41d04df492d145a1654c6995a6553f0c1f6f1960
readonly NASMOUNT_RPM_IMAGE=fedora:44@sha256:89f61a124414261868224666aa7fb8df1b78397a53623774bdfb105d1612b48b

family=${1:-}
output_arg=${2:-}
[[ "$family" =~ ^(deb|rpm)$ ]] && [ -n "$output_arg" ] || {
    echo "Usage: $0 deb|rpm OUTPUT_DIRECTORY" >&2
    exit 2
}

engine=${NASMOUNT_CONTAINER_ENGINE:-}
if [ -z "$engine" ]; then
    for candidate in podman docker; do
        if command -v "$candidate" >/dev/null 2>&1; then
            engine=$candidate
            break
        fi
    done
fi
[ -n "$engine" ] || {
    echo "ERROR: no container engine found; install podman or docker." >&2
    echo "On the matching distribution you can instead run:" >&2
    echo "  ./packaging/build-$family.sh EMPTY_OUTPUT_DIRECTORY" >&2
    exit 1
}
"$engine" info >/dev/null 2>&1 || {
    echo "ERROR: '$engine' is installed but its daemon is not reachable." >&2
    exit 1
}

mkdir -p -- "$output_arg"
output_dir="$(cd "$output_arg" && pwd)"

case "$family" in
    deb)
        image=$NASMOUNT_DEB_IMAGE
        install_dependencies='export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  appstream binutils build-essential ca-certificates cmake debhelper devscripts \
  extra-cmake-modules file git jq libkf6auth-dev libkf6config-dev \
  libkf6coreaddons-dev libkf6i18n-dev libkf6kcmutils-dev libkf6kio-dev \
  libkf6widgetsaddons-dev lintian qml6-module-qtquick qml6-module-qtquick-controls \
  qml6-module-qtquick-dialogs qml6-module-qtquick-layouts \
  qt6-base-dev qt6-declarative-dev systemd'
        lint='lintian --fail-on error /out/*.deb'
        ;;
    rpm)
        image=$NASMOUNT_RPM_IMAGE
        install_dependencies='dnf install -y -q --allowerasing --setopt=install_weak_deps=False \
  appstream binutils cmake cpio diffutils extra-cmake-modules file findutils \
  gcc-c++ git gzip jq kf6-kauth-devel kf6-kcmutils-devel kf6-kconfig-devel \
  kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kio-devel kf6-kwidgetsaddons-devel \
  qt6-qtbase-devel qt6-qtdeclarative qt6-qtdeclarative-devel rpm-build rpmlint \
  systemd systemd-rpm-macros tar
useradd --create-home --shell /bin/bash builder 2>/dev/null || :'
        lint='rpmlint /out/*.rpm || echo "NOTE: rpmlint reported findings (not fatal, as in CI)."'
        ;;
esac

# SOURCE_DATE_EPOCH is forwarded only when it is actually set. Passing it
# through as an empty string is worse than not passing it: dpkg parses the
# variable if it is present at all and fails with `a2i(""): Operation
# canceled`, and build-deb.sh's own "not a number, derive from HEAD" fallback
# never gets a chance to run.
# A failed run must not leave a partial tar behind for `ls` to report as an
# artifact, or for the next run to trip over.
artifact_tar="$output_dir/.artifacts.tar"
trap 'rm -f -- "$artifact_tar"' EXIT

engine_env=()
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    engine_env=(-e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH")
fi

# The build output is streamed back over stdout as a tar rather than written
# through a bind-mounted directory. A bind mount would tie the result to a host
# inode that can be replaced underneath the running container -- after which
# writes land on an unlinked directory and silently never appear -- and it
# would leave artifacts owned by the container's build account, needing a
# chown back. Streaming has neither problem: nothing but the source is shared,
# and the tar is unpacked by the invoking user.
#
# The source mount stays read-only, so a build can never write into the tree.
# Progress goes to stderr inside the container; fd 3 carries the payload.
"$engine" run --rm \
    -v "$repo_root":/src:ro \
    "${engine_env[@]}" \
    "$image" \
    bash -euo pipefail -c "
exec 3>&1 1>&2

echo '=== installing $family build dependencies ==='
$install_dependencies
id builder >/dev/null 2>&1 || useradd --create-home --shell /bin/bash builder

echo '=== copying the working tree ==='
mkdir -p /work /out
cp -a /src/. /work/
rm -rf /work/build /work/build-* /work/.git /work/dist
chown -R builder:builder /work /out

echo '=== building as an unprivileged user ==='
runuser -u builder -- /work/packaging/build-$family.sh /out

echo '=== linting ==='
$lint

echo '=== streaming artifacts out ==='
tar -cf - -C /out . >&3
" > "$artifact_tar"

tar -xf "$artifact_tar" -C "$output_dir"
rm -f -- "$artifact_tar"

printf '\n%s package written to %s:\n' "$family" "$output_dir"
ls -1 "$output_dir"
