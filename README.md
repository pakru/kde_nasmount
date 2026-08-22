# kde_mount - easy network mounts without CLI hustle

Make your SMB network shares on your NAS or any other remote host easly connected to your Linux host.

Requirements:
 - Kubuntu 26.04+ or Fedora 44+, **or**
 - KDE Plasma 6.0+ and Linux kernel 6.8+ on any other distro
 
## Downloads and Install

Download the latest package for your system with one command.

Kubuntu 26.04 LTS amd64:

```bash
wget https://github.com/pakru/kde_mount/releases/latest/download/nasmount-amd64-0.1.0.deb
```

Fedora KDE 44 x86_64:

```bash
wget https://github.com/pakru/kde_mount/releases/latest/download/nasmount-fedora44-x86_64-0.1.0.rpm
```

Then install the downloaded package:

```bash
sudo dpkg -i ./nasmount-amd64-0.1.0.deb                       # Kubuntu
sudo dnf install ./nasmount-fedora44-x86_64-0.1.0.rpm         # Fedora
```

![Mount as Network drive in dolphin](docs/img/img3.png)

![Mount as Network Drive dialog](docs/img/img1.png)

![Network Mounts settings page](docs/img/img2.png)


It mounts CIFS/SMB shares as real kernel mounts at **any path you choose**, using
generated static systemd `.mount`/`.automount` pairs. A share mounts **on demand**
— the first time a process opens its path — and the idle timeout releases it
while the trigger stays armed. A share's credential is a root-owned file that
survives reboot, and the trigger is armed at boot before anyone logs in — so a
saved share is simply there again after a restart, with nothing to re-enter and
nothing to arm by hand.

Two ways in, one backend — and now literally one dialog: **System Settings →
Network Mounts** to list, add and remove shares with live state for each, or
the **Dolphin service menu** (right-click an `smb://` share → **Mount as
Network Drive…**). Both render the same `ShareForm.qml`.


## Source-build requirements

A **Plasma 6+ / KF6+** desktop and **Linux 6.8+** (for `STATX_MNT_ID_UNIQUE`).

| Need | Debian/Ubuntu package |
|------|-----------------------|
| CMake ≥ 3.20, C++20 compiler | `cmake`, `g++` |
| Qt 6 Core / Widgets / Concurrent / Quick / QuickControls2 | `qt6-base-dev`, `qt6-declarative-dev` |
| Extra CMake Modules | `extra-cmake-modules` |
| KF6 Auth / I18n / WidgetsAddons / Config / CoreAddons / KCMUtils | `libkf6auth-dev`, `libkf6i18n-dev`, `libkf6widgetsaddons-dev`, `libkf6config-dev`, `libkf6coreaddons-dev`, `kf6-kcmutils-dev` |
| `mount.cifs` at runtime | `cifs-utils` |


## Build and install from source

```bash
make install           # or: ./install.sh — same thing
```

**Do not run this with sudo.** The build runs as you; only `cmake --install`
elevates, and the script refuses to start as root.

The prefix is **`/usr`**, not `/usr/local`: D-Bus only scans
`/usr/share/dbus-1/system-services` and polkit only
`/usr/share/polkit-1/actions`, so a `/usr/local` install would build fine and
then silently fail to authenticate.

This is a **clean install only** — there is no migration from an older version.
If one is installed, run `./uninstall.sh` first.

`make` builds into `build/` without installing; `make clean` removes it. The
shell scripts exist because they gate on the tests passing, refresh Dolphin's
and System Settings' caches, and enable the boot coordinator.

## Versioning and release artifacts

[`VERSION`](VERSION) is the single source of truth for the release version and
uses `MAJOR.MINOR.PATCH` format. CMake reads it as `PROJECT_VERSION`; the
command-line programs' `--version` output and the KCM plugin metadata are
generated from that value. A release bump therefore changes only `VERSION`.

Release tags use the matching `v<version>` form (for example, version `0.3.0`
uses tag `v0.3.0`). The release workflow rejects a tag whose value does not
match `VERSION`, rebuilds both native packages from that tag, smoke-installs
and removes them on their target distributions, verifies the two-package set,
generates checksums and a JSON release manifest, attests both packages, and
creates a draft GitHub Release.

For a release, change `VERSION`, commit it, push, and wait for regular
CI to pass. Then create the matching tag without entering the version again:

```bash
./packaging/tag-release.sh          # or add --sign when Git tag signing is configured
```

The helper requires a clean `master` whose current commit is already the
`origin/master` tip, validates `VERSION` and `packaging/RELEASE`, and refuses
an existing local or remote tag. It creates an annotated tag locally and
prints the exact `git push origin refs/tags/v<version>` command; it never pushes
on its own. Review the tag, run the printed command, and the tag push triggers
the release workflow.

The regular CI workflow has these required jobs:

1. `validate_packaging` — rootless source, shell, workflow, and package-metadata gates;
2. `build_deb` — unprivileged Ubuntu 26.04 build, all CTests, Lintian, and payload evidence;
3. `build_rpm` — unprivileged Fedora 44 build, all CTests, rpmlint, and payload evidence;
4. `smoke_packages` — clean-container install, package guard, and removal for both targets;
5. `verify_artifact_set` — exact payload/layout checks plus checksums and release metadata;
6. `ci_success` — one branch-protection result requiring every prior job.

Package creation deliberately uses `dpkg-buildpackage`/debhelper and
`rpmbuild`/Fedora RPM macros. The repository `make install` target is only for
interactive source installation and is never invoked to assemble a package.

Adding or removing a share requires **administrator authentication**
(`auth_admin`): it writes a persistent root-owned credential under `/etc` and a
unit that mounts before anyone signs in, which is the same authority as editing
`/etc` by hand. That prompt appears once per add or remove — never at boot, and
never while using a mounted share. Listing state is read-only and unauthenticated.

## Tests

```bash
make test              # or: ctest --test-dir build --output-on-failure
```

Fifteen test binaries plus shell, metadata, and AppStream gates; `install.sh`
runs every one and refuses to install if any fail. Both native-package builds
run the complete 21-test CTest suite. Privileged accept paths and real reboot /
no-login behaviour must still be validated in disposable target VMs.

## Uninstall

For a native package, run this as the desktop user who owns the shares:

```bash
nasmount-uninstall
```

An authenticated full purge: every managed share, its credentials and runtime
records, `~/.config/nasmountrc`, then the software itself. Mount points are retained; tampered state or an unsafe live mount is
refused, leaving everything installed for retry.

The command remains installed after the download is deleted. It authenticates
and purges managed state while the KAuth helper and polkit policy still exist,
then invokes `apt-get remove` or `dnf remove --no-autoremove` through `sudo`.
Direct package-manager removal is allowed only when the read-only package guard
proves that no nasmount-managed units, credentials, or runtime state remain;
otherwise it stops and tells you to use `nasmount-uninstall`. On Fedora, never
use plain `dnf remove nasmount`: DNF may continue auto-removing dependencies
after an RPM pre-removal guard refuses. Use `nasmount-uninstall`, or
`dnf remove --no-autoremove nasmount` only when the guard reports empty state.

For an installation made from source, use:

```bash
./uninstall.sh
```

It reads `build/install_manifest.txt` and refuses to run without it, so
**source uninstall works only from the build tree that installed** — after
`make clean`, or in a fresh checkout, re-run `./install.sh` first.
