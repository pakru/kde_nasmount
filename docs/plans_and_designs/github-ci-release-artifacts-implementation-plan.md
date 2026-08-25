# GitHub CI and native DEB/RPM packages — implementation plan

This plan implements [GitHub issue #1](https://github.com/pakru/kde_mount/issues/1),
“Introduce github CI build with install script from artifacts assets.” The
primary deliverables are native packages for Ubuntu-family DEB systems and
Fedora-family RPM systems. The release does not publish prebuilt `.tar.gz`
binary bundles or use a custom root-copying installer.

## 1. Priority targets

The initial binary-package matrix, as of 2026-08-17, is:

| Priority | Package | Build baseline | Intended desktop | Architecture |
|---|---|---|---|---|
| 1 | `.deb` | Ubuntu 26.04 LTS | Kubuntu and compatible Ubuntu-based Plasma 6/KF6 systems | amd64 |
| 1 | `.rpm` | Fedora 44 | Fedora KDE Plasma Desktop | x86_64 |

Ubuntu 26.04 is the first Ubuntu baseline because its normal repositories
provide the project's Qt 6.5+ and KF6 build stack. Stock Ubuntu 24.04 provides
Qt 6.4.2, below the `find_package(Qt6 6.5)` minimum; CI must not add KDE neon or
another third-party repository and then label that result a native Ubuntu 24.04
package.

Only the latest stable Fedora release is supported. Before every release,
replace the Fedora target if a newer stable release exists, as one change
containing its build image, package dependencies, expected file layout, CI
entry, and disposable-VM result. Supporting the previous Fedora release is not
part of the initial contract.

KDE neon is not a priority or initial package target. Debian, RHEL/EPEL,
Rocky/AlmaLinux, openSUSE, and other DEB/RPM distributions require their own
package metadata and validation before being advertised as supported.

## 2. Release assets and user flow

`VERSION` remains the sole upstream software version. Add
`packaging/RELEASE`, containing a positive integer initially set to `1`, for a
packaging-only revision that does not change the application version.
Add `packaging/tag-release.sh` so maintainers enter an application version only
in `VERSION`. The helper validates a clean `master` at the published
`origin/master` tip, rejects an existing matching local or remote tag, and
creates the local annotated `v<version>` tag. It prints but never executes the
tag push, leaving one explicit review point before the release workflow starts.

For `VERSION=0.1.0` and `packaging/RELEASE=1`, the release contains:

```text
nasmount-amd64-0.1.0.deb
nasmount-fedora44-x86_64-0.1.0.rpm
nasmount-0.1.0-SHA256SUMS
nasmount-0.1.0-release-manifest.json
```

Debug-symbol packages may be retained as short-lived CI artifacts but are not
required in the first public Release asset set. GitHub's source archive for the
matching tag supplies the corresponding source.

The build jobs produce native conventional filenames internally, but the
publication job copies the verified bytes to these clearer, still-versioned
Release asset names. Package metadata retains the full native version and
packaging revision.

The Ubuntu-family download command is one literal line:

```bash
wget https://github.com/pakru/kde_mount/releases/latest/download/nasmount-amd64-0.1.0.deb
```

The Fedora-family equivalent is:

```bash
wget https://github.com/pakru/kde_mount/releases/latest/download/nasmount-fedora44-x86_64-0.1.0.rpm
```

Users then install the downloaded `.deb` with `apt` or `.rpm` with `dnf`.

`apt` and `dnf` resolve and install the declared runtime dependencies. Users do
not run the source-tree `install.sh`, install compiler/development packages, or
copy files into `/usr` themselves.

The supported full uninstall command is:

```bash
nasmount-uninstall
```

It performs the existing owner-scoped authenticated cleanup first, then asks
the native package manager to remove `nasmount`. Direct `apt remove nasmount`
or `dnf remove --no-autoremove nasmount` is allowed only when no managed
nasmount system state exists. Plain Fedora `dnf remove nasmount` is unsupported:
DNF may continue auto-removing unused dependencies after a failed RPM `%preun`.
Package removal must fail closed rather than orphan units, credentials, runtime
records, or an installed application whose runtime dependencies were removed.

## 3. Package and security boundaries

Native packaging must preserve these codebase invariants:

1. Every package installs to `/usr`, never `/usr/local`.
2. `nasmount-core`, `nasmount-session`, and `nasmount-root` remain static with
   the existing linkage boundary. No package adds an application `.so`.
3. Builds and tests run as a non-root build user. Root is used only by `apt`,
   `dnf`, `dpkg`, or `rpm` while installing/removing the completed package in a
   disposable smoke environment or end-user system.
4. The package consumes the CMake install graph. Debian/RPM metadata must not
   manually reimplement KAuth, D-Bus, polkit, systemd, KDE libexec, or KCM
   destinations.
5. The helper stays thin and all privileged mutation remains in `src/root`.
   Package maintainer scripts never call the helper protocol directly.
6. Package scripts are noninteractive and never launch KAuth or a graphical
   prompt. The user-facing `nasmount-uninstall` command owns authenticated
   cleanup.
7. `nasmount-cleanup` runs while the helper, system D-Bus service, and polkit
   policy are still installed. Program-file removal starts only after cleanup
   reports confirmed success.
8. Ubuntu's KAuth helper path, Debian multiarch libexec paths, and Fedora
   lib64/libexec paths remain distinct and are taken from each target's
   CMake/KDEInstallDirs result.
9. The package manager owns program-file removal. No maintainer script reads a
   user-writable list and passes it to `rm` as root.
10. Real KAuth, polkit, system D-Bus, service enablement, CIFS, reboot, and
    no-login behavior remain disposable-VM release tests.

## 4. Installed package contents

Both package families contain the current CMake-installed application plus:

- `/usr/bin/nasmount-uninstall` — unprivileged authenticated cleanup followed
  by native package removal;
- a read-only `nasmount-package-guard` in the configured KDE libexec directory;
- `/usr/share/nasmount/cleanup-manifest.txt` — a root-owned, package-owned
  complete manifest accepted by `Session::validateInstallManifest()`;
- `/usr/share/nasmount/package-family` — exactly `deb` or `rpm` in a native
  package, installed from package metadata rather than accepted from a caller;
- the license and package documentation in the target's conventional
  documentation directory;
- on Fedora, any systemd preset file required by the approved service policy.

The cleanup manifest includes its own installed path, the uninstaller, the
package guard, and every application/package payload file. The package build
generates it from the target's staged CMake install manifest plus a finite
reviewed list of packaging additions, so Debian and Fedora receive complete but
different manifests. The compiled validator accepts either complete layout and
rejects a manifest that mixes paths from both.

Package inspection tests compare the package manager's actual regular-file
list to the expected CMake stage plus packaging additions. Unexpected files,
missing files, `/usr/local`, build-tree paths, tests, source files, or a mixed
Debian/Fedora layout fail the build.

Mutable share state remains outside package ownership:

- `/etc/systemd/system/*.mount` and `*.automount` definitions;
- `/etc/nasmount` credentials;
- `/run/nasmount` and `/run/nasmount-ids` runtime state;
- each user's `~/.config/nasmountrc`.

Neither `.deb` nor `.rpm` claims those paths as package payload.

Add a CMake package-family setting whose default is `source`; native package
builds explicitly select `deb` or `rpm`. A source install continues to use the
checkout's `uninstall.sh`. If the installed `nasmount-uninstall` sees the
`source` family, it prints that instruction and does not guess a package-manager
command. This preserves the existing source-install lifecycle while making the
native package behavior deterministic.

## 5. Safe package installation, upgrade, and removal

### 5.1 Installation and service setup

The package declares ELF runtime dependencies through native dependency
generation and explicitly declares non-ELF requirements such as `cifs-utils`,
systemd/polkit integration, Qt Quick QML modules, Qt Quick Controls, Qt Quick
Dialogs, and the KF6 KCMUtils QML module.

Debian packaging uses debhelper's systemd integration. Fedora packaging uses
the Fedora systemd RPM macros and an explicit preset decision. The resulting
package contract is the same on both systems:

1. package installation is noninteractive and idempotent;
2. `systemctl daemon-reload` is performed through distro-native helpers;
3. `nasmount-boot.service` is enabled for boot and started when systemd is the
   running init system;
4. installation in a container/chroot without systemd PID 1 succeeds without
   pretending the service was runtime-tested;
5. user-session KDE cache refresh is not attempted as root. The README tells
   the installing user to restart Dolphin/System Settings or run
   `kbuildsycoca6 --noincremental` in their desktop session.

### 5.2 Initial package-upgrade policy

The repository currently documents clean install only and no migration from an
older installed version. The first native packages preserve that rule:

- Debian `preinst upgrade` rejects an in-place upgrade before new payload files
  are unpacked;
- the RPM transaction gate rejects an installed older `nasmount` version before
  replacement begins;
- the message instructs the user to run `nasmount-uninstall`, then install the
  new package;
- fresh installation and package-manager retry/error-recovery calls remain
  idempotent.

Do not use this issue to infer that on-disk definitions are migration-safe.
Supporting native in-place package upgrades requires a separate reviewed
compatibility decision and tests.

### 5.3 Read-only package-removal guard

Add a small `nasmount-package-guard` executable that links `nasmount-core` and
Qt Core only. It is read-only and must never link `nasmount-root` or contain a
filesystem/systemd mutation path.

Put package-removal state classification in a testable core API. It scans every
top-level `*.mount` and `*.automount` candidate under `/etc/systemd/system` and
uses the shared `UnitValue::hasMarker()` / `parseMarker()` contract. Any valid
marker proves managed state exists; a malformed managed-looking marker is
unsafe. This is deliberately enough to block removal without interpreting or
trusting the rest of a possibly tampered unit body. It also checks for
`/etc/nasmount`, `/run/nasmount`, and `/run/nasmount-ids`; unreadable or
unclassifiable candidates fail closed.

The guard returns success only for provably empty managed state. `prerm remove`
and RPM final-erasure `%preun` call it while the executable is still installed:

- empty state: continue with normal service stop/disable and package removal;
- managed, malformed, tampered, mixed-owner, or indeterminate state: print a
  concise instruction to run `nasmount-uninstall` as the owning desktop user
  and exit nonzero before package files disappear.

The guard never removes or repairs anything. Forced package-manager options
that explicitly bypass maintainer scripts are outside the supported path.

### 5.4 `nasmount-uninstall`

The installed uninstaller:

1. refuses UID 0;
2. resolves the fixed, root-owned cleanup manifest and package-family metadata;
3. invokes `/usr/bin/nasmount-cleanup --manifest
   /usr/share/nasmount/cleanup-manifest.txt` as the current user;
4. stops if the KAuth result is failed or unknown;
5. after confirmed purge and caller config removal, executes the matching
   package-manager removal command (`sudo apt remove nasmount` or
   `sudo dnf remove --no-autoremove nasmount`);
6. relies on the package pre-removal guard to independently confirm that no
   managed root state remains;
7. refreshes the caller's KDE cache after successful package removal when the
   command remains available, otherwise prints the restart instruction.

`Root::Operations::purge()` already rejects tampered state and managed shares
belonging to another user. Preserve that all-or-nothing ownership rule. A
single user cannot use package uninstall to delete another user's shares.

If authenticated purge succeeds but native package removal fails, the safe
result is an installed but empty application. The user can retry `apt remove`
or `dnf remove --no-autoremove`; do not attempt rollback by recreating deleted
shares.

Direct native package removal when the guard reports empty state removes only
package files. It does not traverse home directories to delete arbitrary users'
configuration. The full `nasmount-uninstall` path removes the invoking user's
configuration as it does today.

### 5.5 Build entry points

Keep `make` and `make test` as the developer-facing source build and test
commands. Native package jobs use `dpkg-buildpackage` and `rpmbuild`; their
debhelper and RPM macros invoke CMake with package-specific build directories,
`DESTDIR` staging, install paths, and `NASMOUNT_PACKAGE_FAMILY` values.

Do not run `make install` in package jobs. The repository Makefile delegates it
to the interactive source `install.sh`, which requests elevation and mutates
the running system. Do not add a second Makefile packaging implementation that
duplicates debhelper or RPM behavior. Package CI may run `make test` as an
additional source-build gate, but the package build's own test phase remains
authoritative because it tests the exact package configuration.

## 6. Debian package implementation

Add native Debian metadata under `packaging/debian/`:

```text
packaging/debian/control
packaging/debian/rules
packaging/debian/source/format
packaging/debian/changelog.in
packaging/debian/nasmount.install
packaging/debian/nasmount.preinst
packaging/debian/nasmount.postinst
packaging/debian/nasmount.prerm.in
packaging/debian/nasmount.postrm
packaging/debian/nasmount.triggers
```

Build in a temporary source-package tree so the repository root does not need a
generated `debian/changelog`. Generate the changelog from `VERSION`,
`packaging/RELEASE`, the tagged commit date, and release notes; never hand-edit
another application version.

Use debhelper's CMake sequence with `/usr`, `RelWithDebInfo`, and
`-DNASMOUNT_PACKAGE_FAMILY=deb`. The package build must run the complete CTest
suite through `dh_auto_test`, not only package smoke tests. Generate
`${shlibs:Depends}`/`${misc:Depends}` and add explicit runtime/QML dependencies
that ELF scanning cannot discover.

Validate with at least:

- `dpkg-buildpackage -b -us -uc` as non-root;
- `lintian` with reviewed, documented overrides only;
- `dpkg-deb --info` and `dpkg-deb --contents`;
- a clean-container `apt install ./nasmount_...deb` and removal cycle.

Do not embed a private APT repository, KDE neon repository, or package signing
key. The first GitHub Release uses checksums and GitHub provenance; an APT
repository and Release-file signing are separate distribution work.

## 7. Fedora RPM implementation

Add native Fedora metadata under `packaging/rpm/`:

```text
packaging/rpm/nasmount.spec.in
packaging/rpm/90-nasmount.preset
```

Generate the spec version/release from `VERSION` and `packaging/RELEASE` while
retaining the target's `%{?dist}` suffix. Use Fedora's `%cmake` with
`-DNASMOUNT_PACKAGE_FAMILY=rpm`, followed by `%cmake_build`, `%ctest`,
`%cmake_install`, and the systemd scriptlet macros. The `%files` section is
explicit and must match the staged CMake/package additions.

Use automatic RPM ELF provides/requires plus explicit non-ELF runtime/QML
requirements. Build and test the RPM on Fedora 44 only. When Fedora 45 becomes
stable, update the target, dependency metadata, container digest, artifact
name, and VM qualification together instead of retaining an unqualified
previous-release package.

Validate with at least:

- `rpmbuild -ba` as a non-root build user;
- `rpmlint` with reviewed, documented exceptions only;
- `rpm -qpi`, `rpm -qpl`, and dependency inspection on the resulting file;
- a clean-container `dnf install ./nasmount-...rpm` and removal cycle.

The first GitHub Release packages may be unsigned while protected by published
SHA-256 values and GitHub provenance. Native RPM signing and a DNF repository
require a separately managed release key and are not improvised with a CI
secret in this issue.

## 8. CI workflows and exact jobs

### 8.1 `.github/workflows/ci.yml`

Triggers: pull requests, default-branch pushes, and manual dispatch. Default
permission is `contents: read`; no CI job receives repository secrets or write
permission.

```text
validate_packaging
├── build_deb [ubuntu-26.04-amd64]
└── build_rpm [fedora-44-x86_64]
                    │
              smoke_packages [both targets]
                    │
             verify_artifact_set
                    │
                 ci_success
```

| Job ID | Matrix/environment | Needs | Responsibilities | Output |
|---|---|---|---|---|
| `validate_packaging` | GitHub `ubuntu-24.04` host | none | Validate `VERSION` and `packaging/RELEASE`; run shell syntax/source/QML/removed-API gates; validate Debian control/spec templates and maintainer-script argument tables; ensure all Actions are SHA-pinned. | Validation logs |
| `build_deb` | `ubuntu-26.04-amd64` in official `ubuntu:26.04` container pinned by digest | `validate_packaging` | Install build dependencies, drop to a dedicated non-root user, run `dpkg-buildpackage`, full tests, Lintian, package metadata/file-list/dependency gates, and upload the DEB plus logs. | One `.deb`, build metadata, test/lint logs |
| `build_rpm` | `fedora-44-x86_64` in an official Fedora 44 container pinned by digest | `validate_packaging` | Install build dependencies, drop to a dedicated non-root user, run `rpmbuild`, `%check`, rpmlint, package metadata/file-list/dependency gates, and upload the RPM plus logs. | One `.rpm`, build metadata, test/lint logs |
| `smoke_packages` | Two-entry Ubuntu 26.04/Fedora 44 matrix in fresh matching containers | `build_deb`, `build_rpm` | Download only the matching package; install through `apt` or `dnf`; verify dependency resolution, versions, owned paths/modes, cleanup manifest, package guard, service unit/preset metadata, and no unresolved ELF/QML dependency; remove with provably empty managed state and check for package-file leftovers. | Per-target install/remove report |
| `verify_artifact_set` | GitHub `ubuntu-24.04` host | `smoke_packages` | Require exactly one DEB and one RPM; inspect without executing; require common application version/commit, correct package release and Fedora distro suffix, unique filenames, no extra payload, and matching package digests; generate candidate SHA256SUMS/manifest. | Verified two-package set |
| `ci_success` | GitHub `ubuntu-24.04` host | all previous jobs | With `if: always()`, fail if any required job/matrix entry failed, was cancelled, or was unexpectedly skipped. Print artifact links/digests. This is the required branch-protection check. | Pass/fail summary |

Do not share compiled outputs between DEB and RPM jobs. Caches, if added later,
are target/container/dependency keyed and never cache final packages.

### 8.2 `.github/workflows/release.yml`

Triggers: `v*` tag pushes and manual dispatch with an existing tag input. It
rebuilds the exact tag rather than promoting packages from another workflow.

```text
validate_release
├── build_deb_release
└── build_rpm_release [fedora-44]
                    │
           smoke_release_packages [both targets]
                    │
             verify_release_set
                    │
            attest_and_publish
```

| Job ID | Needs | Responsibilities | Permission/output |
|---|---|---|---|
| `validate_release` | none | Require `vMAJOR.MINOR.PATCH`; compare tag to `VERSION`; validate positive package release; fetch full history; reject existing conflicting assets; freeze the supported target matrix. | `contents: read`; normalized tag/version/release/commit |
| `build_deb_release` | `validate_release` | Repeat the clean DEB build, full tests, lint, and inspection from the tag. | Final DEB |
| `build_rpm_release` | `validate_release` | Repeat the clean Fedora 44 RPM build, full tests, lint, and inspection from the tag. | Final RPM |
| `smoke_release_packages` | both build jobs | Install and remove each final package in a fresh matching container using the native package manager. | Two smoke reports |
| `verify_release_set` | smoke job | Require exactly the two main packages; generate `nasmount-<version>-SHA256SUMS` and a JSON manifest with filenames, package metadata, target, commit, and digest. | Read-only release set |
| `attest_and_publish` | verify job | Check out the validated tag without persisted credentials so `gh release create --verify-tag` has Git context; generate GitHub provenance for each versioned package; create a draft GitHub Release with `gh`; upload packages, checksums, and manifest with exact one-line latest-download and install/uninstall notes. | Protected `release` environment; `contents: write`, `id-token: write`, `attestations: write` |

Use per-tag concurrency with cancellation disabled. Pin Actions to full commit
SHAs with version comments, use the preinstalled `gh` CLI instead of a
third-party release action, and add `.github/dependabot.yml` for reviewed
Actions updates.

## 9. Automated tests

Add rootless/local tests for:

- package-state classification: empty, valid managed state, malformed marker,
  tampered/collision, runtime-only state, and indeterminate read failure;
- the package guard performing no mutation and linking no `nasmount-root`;
- complete Debian and Fedora cleanup manifests and mixed-layout rejection;
- package-family metadata validation and uninstaller command selection;
- unknown/failed KAuth result never invoking `apt` or `dnf`;
- confirmed purge invoking only the expected fixed package name;
- preinstall upgrade/refusal and maintainer-script retry argument matrices;
- pre-removal guard ordering before service/package-file removal;
- package file-list parity with CMake plus finite package additions;
- dependency metadata including `cifs-utils` and QML imports;
- no `/usr/local`, build RPATH, unresolved ELF dependency, source/test file, or
  user/root mutable share state in either package;
- package filename, version, release, Fedora dist suffix, tag, and `VERSION`
  agreement;
- exact one-DEB/one-RPM release-set validation.

Every package build runs the existing complete CTest suite. Adding a C++ test
binary still requires updating `tests/`, CMake, and `install.sh`'s explicit
test-binary gate.

## 10. Disposable VM release matrix

Before publishing the first draft Release, test its downloaded packages on:

| Package | VM |
|---|---|
| DEB | Fresh Kubuntu 26.04 LTS amd64 |
| RPM | Fresh Fedora 44 KDE Plasma Desktop x86_64 |

For every VM:

1. Verify SHA-256 and GitHub provenance.
2. Install using `apt install ./...deb` or `dnf install ./...rpm` with no build
   dependencies present.
3. Confirm dependency resolution uses only the distribution's enabled official
   repositories.
4. Confirm package-owned paths, modes, owners, package guard, manifest, system
   D-Bus/polkit files, KCM, and service metadata.
5. Confirm `nasmount-boot.service` is enabled and active.
6. Confirm KAuth/polkit Add and Delete prompts and no passwordless mutation.
7. Confirm System Settings and Dolphin discover the installed integration.
8. Add a disposable SMB share, exercise automount/idle release, reboot without
   login, and verify boot arming.
9. With a managed share present, confirm direct `apt remove` or
   `dnf remove --no-autoremove` refuses without changing the complete installed
   package set or leaving unresolved guard libraries.
10. Run `nasmount-uninstall`; confirm authenticated purge precedes native
    package removal and mount-point directories remain.
11. Confirm direct package removal succeeds when managed state is empty.
12. Confirm an attempted in-place package upgrade is rejected before payload
    replacement and the existing installation still works.

Publish the draft only after both VM results are recorded. Never run these
privileged accept paths on a maintainer workstation or GitHub-hosted runner.

## 11. Documentation changes

Update `README.md` to:

- lead with native Ubuntu/Kubuntu DEB and Fedora RPM downloads;
- show literal one-line `wget` commands using `/releases/latest/download/`,
  followed by `apt install ./...deb` and `dnf install ./...rpm`;
- explain checksums/provenance, runtime dependencies, clean reinstall, and
  `nasmount-uninstall`;
- explain why direct native removal is guarded while managed shares exist;
- state the exact supported releases and that KDE neon/generic DEB/RPM are not
  implied;
- distinguish public Release packages from short-lived CI packages;
- keep source `make install` for developers and unsupported systems.

Update `AGENTS.md` with the package build commands, exact CI/release job names,
maintainer-script safety rules, package-state guard boundary, file-list/manifest
synchronization rule, and two-VM gate.

## 12. Implementation sequence

Each phase leaves `make test` passing and should be one reviewable commit unless
the DEB and RPM metadata naturally require separate commits.

### Phase 1 — add read-only removal guard and package-aware uninstall

Files:

- core package-state API under `src/core/` (new)
- `src/packageguard/main.cpp` (new)
- `packaging/nasmount-uninstall.sh` (new)
- `CMakeLists.txt`
- `src/session/cleanupvalidation.cpp`
- `tests/cleanupvalidation_test.cpp`
- new package-state/uninstaller tests
- `install.sh`

Implement empty-state proof, malformed-marker detection, package guard,
complete per-layout cleanup manifests, and the authenticated-cleanup-then-native
removal flow. Enforce that the guard cannot link `nasmount-root`.

### Phase 2 — add Ubuntu DEB packaging

Files:

- `packaging/RELEASE` (new)
- `packaging/debian/*` (new)
- Debian package tests/gates

Implement dependency metadata, full package build/tests, service integration,
upgrade refusal, pre-removal guard, Lintian, package inspection, and clean
container install/remove.

### Phase 3 — add Fedora RPM packaging

Files:

- `packaging/rpm/*` (new)
- Fedora package tests/gates

Implement the Fedora 44 spec, requirements, systemd macros/preset, upgrade
refusal, `%preun` guard, rpmlint, package inspection, and clean-container
install/remove.

### Phase 4 — implement the six CI jobs

Files:

- `.github/workflows/ci.yml` (new)
- `.github/dependabot.yml` (new)

Implement `validate_packaging`, `build_deb`, `build_rpm`, `smoke_packages`,
`verify_artifact_set`, and `ci_success` exactly as specified in §8.1.

### Phase 5 — implement the six release jobs

Files:

- `.github/workflows/release.yml` (new)
- `packaging/tag-release.sh` (new)
- release-set tests/gates

Implement `validate_release`, `build_deb_release`, `build_rpm_release`,
`smoke_release_packages`, `verify_release_set`, and `attest_and_publish`.
Add rootless functional tests for dirty-state refusal, annotated tag creation,
local/remote duplicate refusal, and the no-implicit-push contract.

### Phase 6 — qualify and document the first packages

Files:

- `README.md`
- `AGENTS.md`
- this plan only for justified contract corrections found during implementation

Run the two-VM release matrix against draft assets and publish only after both
targets pass.

## 13. Completion criteria

Issue #1 is complete when:

- CI produces one installable Ubuntu 26.04 DEB and one Fedora 44 RPM;
- both native package builds run the full test suite and package lint;
- `ci_success` reflects every build, matrix, smoke, and verification result;
- a valid tag produces exactly two versioned packages, checksums, a release
  manifest, and provenance for both published package filenames;
- `apt`/`dnf` install dependencies and package files without source/build tools;
- package payloads exactly match the per-distribution CMake layout plus finite
  reviewed packaging additions;
- direct removal refuses safely while managed state exists;
- `nasmount-uninstall` authenticates, purges caller-owned state, then removes
  the package, without allowing one user to purge another user's shares;
- direct removal succeeds with empty state and leaves no package-owned files;
- in-place upgrades fail before payload replacement under the current clean
  install policy;
- KAuth, polkit, D-Bus, KCM, Dolphin, CIFS automount, idle release, boot arming,
  guarded removal, and full uninstall pass on both disposable VMs;
- README prioritizes native DEB/RPM packages and does not present KDE neon or a
  generic Linux build as supported.

## 14. Explicitly out of scope

- KDE neon as a priority or initial package target;
- Ubuntu 24.04 packages built from third-party KDE repositories;
- Debian, RHEL/EPEL, Rocky, AlmaLinux, openSUSE, or generic DEB/RPM support;
- APT/DNF repositories and repository metadata signing;
- native package in-place upgrades/migrations;
- ARM64 or other architectures;
- automatic updates;
- privileged integration tests on GitHub-hosted runners;
- changes to share Edit/Delete/Add lifecycle, credentials, or helper mutation
  authorization.

Those are separate issues after the initial Ubuntu/Fedora packages are proven.
