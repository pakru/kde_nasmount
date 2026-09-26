# AGENTS.md — kde_nasmount

A KDE/Plasma 6 tool that mounts CIFS shares by generating **static systemd
`.mount` / `.automount` unit pairs** in `/etc/systemd/system`. Two front ends
(a System Settings KCM and a Dolphin service-menu dialog) drive one privileged
KAuth helper. Qt 6 / KF6, C++20, CMake.

Read [`README.md`](README.md) for what the tool does. **The sources are the
design record**: every header and every non-obvious rule carries its own
reasoning in comments, and this file carries the cross-cutting rules.

**Never cite a design document or plan** (`design §6.2`, `plan §4.1`,
`autofill plan`) in code, tests, scripts, or this file. Planning documents
live in `docs/plans_and_designs/`, which is gitignored and never published, so
a citation points at nothing for anyone reading the repository. When a rule's
reason is not local, write the reason itself next to the rule.

## Build, test, install

```bash
make                  # configure + build into build/ (RelWithDebInfo, /usr prefix)
make test             # build, then ctest --output-on-failure
ctest --test-dir build --output-on-failure   # tests only
build/bin/<name>_test                        # one test binary directly
make install          # runs ./install.sh — NOT with sudo
make uninstall        # runs ./uninstall.sh
make deb              # .deb in dist/deb/, built in the pinned Ubuntu 26.04 image
make rpm              # .rpm in dist/rpm/, built in the pinned Fedora 44 image
make packages         # both
make clean            # rm -rf build/
make dist-clean       # rm -rf dist/
```

- **Never run `install.sh` as root** — it refuses. It builds as you and
  elevates only for `cmake --install`.
- The prefix is **`/usr`**, not `/usr/local`: D-Bus only scans
  `/usr/share/dbus-1/system-services` and polkit only
  `/usr/share/polkit-1/actions`. A `/usr/local` install builds and then
  silently fails to authenticate.
- Installing is disruptive (writes system D-Bus/polkit files and enables the
  boot service). Don't install unless asked.
- `uninstall.sh` reads `build/install_manifest.txt` and refuses to run without
  it, so **uninstall only works from the build tree that installed** — a clean
  checkout, or one where `make clean` has run, cannot uninstall until
  `install.sh` regenerates the manifest. The manifest content is deterministic
  (validated against the hardcoded allowlist in
  [`cleanupvalidation.cpp`](src/session/cleanupvalidation.cpp#L27), which also
  requires it to be complete), so regenerating it is safe.
- `build/` is the only build directory the tooling knows about; `.gitignore`
  also covers `build-*/` for ad-hoc trees.

## Native packages and release CI

The supported binary targets are Ubuntu/Kubuntu 26.04 amd64 (`.deb`) and
Fedora KDE 44 x86_64 (`.rpm`). KDE neon and a generic DEB/RPM compatibility
claim are explicitly out of scope. Build packages only inside their matching
target containers:

```bash
make deb                                    # or: make rpm / make packages
./packaging/build-deb.sh EMPTY_OUTPUT_DIR   # Ubuntu 26.04, non-root
./packaging/build-rpm.sh EMPTY_OUTPUT_DIR   # Fedora 44, non-root
./packaging/verify-artifact-set.sh PACKAGE_DIR EMPTY_METADATA_DIR
```

`make deb`/`make rpm` exist because the two build scripts refuse to run
anywhere but their target distribution, which almost no workstation is. They
delegate to [`build-in-container.sh`](packaging/build-in-container.sh), which
runs the **same pinned image digests, dependency lists and build scripts as
CI**, then lints the result — so a local failure is a failure CI would have
had. It needs podman or docker (`NASMOUNT_CONTAINER_ENGINE=` overrides the
choice); on the matching distribution, call the build script directly instead.
The working tree is copied in, uncommitted changes included, and the source is
mounted read-only so a build can never write back into it.

The image digests are pinned in `build-in-container.sh`, and repeated in
every container job of `ci.yml` and `release.yml`. `packaging_metadata_test.sh`
compares the helper only against the *first* pin of each distribution in
`ci.yml`, because a local build against a different image proves nothing about
CI — the other `ci.yml` copies and all of `release.yml` are unchecked, so bump
every copy together. Each target clears only its own `dist/<family>/`
subdirectory, since the build scripts require an empty output directory.

Build dependencies are **six hand-maintained lists** that must agree:
`packaging/debian/control`, the spec's `BuildRequires`, the DEB and RPM lists
in `build-in-container.sh`, every Ubuntu/Fedora build block in `ci.yml`, and
`release.yml`. A missing entry fails only in the distribution it was forgotten
in. `packaging_metadata_test.sh` checks those lists only for the KIO and
QML-runtime dependencies the autofill feature added (`libkf6kio-dev`/
`kf6-kio-devel`, `qml6-module-qtquick-dialogs`); extend it when you add
another. The password service itself is a weak dependency in both families
(`Recommends:`), never `Requires:` — a host without it must still install and
mount with a typed credential.

The package entry points use `dpkg-buildpackage`/debhelper and `rpmbuild`/RPM
macros, which call CMake directly with `NASMOUNT_PACKAGE_FAMILY=deb|rpm`.
**Never use `make install` to build a package**: it calls the interactive,
privileged source installer and mutates the running system.

The regular workflow has exactly `validate_packaging`, `build_deb`,
`build_rpm`, `smoke_packages`, `upgrade_deb`, `upgrade_rpm`,
`verify_artifact_set`, and `ci_success`. The
release workflow independently rebuilds the tag through `validate_release`,
`build_deb_release`, `build_rpm_release`, `smoke_release_packages`,
`verify_release_set`, and `attest_and_publish`; only its last job can publish.
The publication job renames the verified native build outputs to the
still-versioned user-facing assets `nasmount-amd64-<version>.deb` and
`nasmount-fedora44-x86_64-<version>.rpm`. README and generated Release-note
one-line download commands depend on those exact names.

`VERSION` is the sole place where a maintainer enters an application version.
After committing the version bump, pushing `master`, and obtaining a successful
regular CI run, use `./packaging/tag-release.sh` (or `--sign` with a configured
Git signing key). The helper verifies that the tree is clean and HEAD equals
`origin/master`, rejects existing local/remote tags, and creates the matching
local annotated `v<version>` tag. It intentionally does not push; review the
tag and run the exact push command it prints. Never move or reuse a release
tag.

### Upgrades

Both families upgrade in place. Five rules govern an upgrade, and
[`packaging_metadata_test.sh`](tests/packaging_metadata_test.sh) and
[`package_scripts_test.sh`](tests/package_scripts_test.sh) encode them:

1. An upgrade **never** refuses and **never** tears anything down; it replaces
   program files in place under live state. No maintainer script runs
   `nasmount-package-guard` at all (see below). `nasmount.prerm.in` reaches
   its teardown from `remove)` only; the RPM gates `%preun`/`%postun` on
   `"$1" -eq 0`. **RPM runs the old package's
   `%preun` and `%postun` with `$1=1` during an upgrade**, so an ungated
   teardown there would unmount and delete every share on every update. That
   gate is the single highest-consequence line in the spec and is tested.
2. An upgrade **never** removes, rewrites, or migrates `/etc/nasmount`,
   `/run/nasmount*`, the generated units in `/etc/systemd/system`, or any
   user's `nasmountrc`.
3. **Only the DEB can gate versions.** Upgrades are supported from
   `MIN_UPGRADABLE_VERSION` in
   [`nasmount.preinst.in`](packaging/debian/nasmount.preinst.in) upward, and
   downgrades are refused there; that file is a template because debhelper does
   not substitute a version into `preinst` and the check needs one. RPM's
   `%pre` receives only an instance count, never the version being replaced, so
   it has no equivalent — and DNF5 downgrades freely on an exact version spec,
   which a local `.rpm` is. The RPM therefore has **no downgrade protection at
   any layer**. Don't add a `%pre` version check; it cannot work. If a
   compatibility gate is ever needed, gate on an on-disk format marker under
   `/etc/nasmount`, which works identically in both families.
4. The generated unit body and the marker schema are a **frozen on-disk
   format**. A share's unit pair survives the upgrade, so new binaries must
   still accept bytes old ones wrote — and because generation and validation
   share the same fixed-value functions, changing one changes both, leaving the
   rest of the suite green while every existing share becomes `Tampered`.
   [`goldenunits_test.cpp`](tests/goldenunits_test.cpp) is the only gate that
   catches this. If it fails, revert the change or design a migration and raise
   `MIN_UPGRADABLE_VERSION` — **never regenerate
   [`tests/golden/units/`](tests/golden/units/)** to make it pass. A release
   that deliberately adds a new shape (as 0.1.4 did) *adds* a
   `v<version>/` directory and registers it in `Corpora` in the test; existing
   directories are never edited or deleted.

   A marker field may be **optional-on-read only if its absence reproduces the
   exact pre-existing byte output.** That is the rule that let
   `X-Nasmount-Access` be added in 0.1.4 without moving
   `MIN_UPGRADABLE_VERSION`: it is omitted on write for the default
   (read-write) value, so every unit pair 0.1.0–0.1.3 wrote still parses,
   still validates, and still regenerates byte-for-byte. A plain new
   *required* field would instead have failed every one of them with `missing
   marker field:` — `Tampered`, and so unarmed at the next boot.
5. Adding, renaming, or removing a KAuth action in
   [`io.github.pakru.nasmount.actions`](io.github.pakru.nasmount.actions) is an
   **upgrade-compatibility change**: a running old front end will call the new
   helper across the upgrade.

`debian/rules` restarts `nasmount-boot.service` after an upgrade explicitly
(`dh_installsystemd --restart-after-upgrade`). The RPM needs no equivalent:
`%systemd_postun_with_restart` marks the unit `Markers=+needs-restart` and
systemd's own `%transfiletriggerpostun` runs `reload-or-restart --marked` at
the end of the transaction. Note that trigger fires *before* ordinary
`%postun`, and that `--marked` enqueues a plain restart, so it starts a unit an
administrator had stopped. Restarting is safe because every
branch of `Root::Arming::evaluateArmPrecheck()` returns before touching the
mount point, the credential, or systemd, with `startedByUs` still false. A
share that is *currently mounted* takes the `Blocked` branch, so
`a live mount already occupies the path` in the journal after an upgrade is
expected, not a fault.

Native removal has a strict order. `nasmount-uninstall` runs authenticated
owner-scoped cleanup while KAuth/polkit are installed, then invokes apt/dnf —
`apt-get purge` on Debian, so no config-files residue remains, and
`dnf remove --no-autoremove` on Fedora. Every check that can fail without
changing anything runs **before** that cleanup; validating `sudo` or the
package manager afterwards would mean a host missing either one loses its
shares and then fails. `nasmount-cleanup` distinguishes a confirmed refusal
(exit 2, nothing dispatched) from an indeterminate outcome (exit 3, state may
be unchanged, partly removed, or fully removed), because the privileged purge
is not atomic across shares and a lost KAuth reply cannot be told apart from a
purge that ran. Never collapse those into one "cleanup failed" message.

**Neither family refuses direct package-manager removal.** Both tear managed
state down themselves. This is a deliberate maintainer decision, not drift:

- **DEB** splits it. `prerm remove` disarms every managed share (automount
  halves first, then the mount halves, which unmounts live CIFS mounts);
  `postrm purge` additionally deletes the unit files, `/etc/nasmount`, and
  `/run/nasmount*`.
- **RPM** has no remove/purge split, so an erase must do both: `%preun`
  disarms, `%postun` deletes. Stopping at the DEB's `remove` behaviour would
  strand root-owned credentials with nothing installed that could remove them.

`nasmount-package-guard` still ships and still classifies host state — the CI
upgrade jobs use it to prove state survived — but it is **diagnostic only**. It
must not reappear in any maintainer script, where it would refuse the removal
this design performs; both test files assert its absence.

Two consequences of the DEB path worth stating outright: `apt remove` destroys
managed shares with no authentication and no owner check, and it cannot clear
any user's `~/.config/nasmountrc` because it has no idea which account owns a
share. `nasmount-uninstall` remains the complete, owner-scoped path.

Managed units are identified **only** by the exact marker line
`# X-Nasmount-Managed=1`, never by filename — a user chooses the filename via
their mount point. The *name* is then checked against the alphabet
`systemd-escape` can emit (alphanumerics, `:`, `_`, `.`, `-`, and the backslash
of a `\xNN` escape) and anything else is skipped with a warning. That check is
load-bearing, not cosmetic: **systemctl expands `*`, `?` and `[...]` in a unit
argument as a pattern over every loaded unit**, so a marked file named
`*.mount` would turn `systemctl stop` into "stop every mount unit on the
system" — and `--` does not help, since it ends option parsing, not globbing.

State directories are removed **non-recursively** by
`nasmount_remove_state_dir()`, which refuses a symlink, refuses a directory
whose device number differs from its parent's (a bind or nested mount), unlinks
only regular files, and lets the final `rmdir` fail if anything unexpected
remains. `rm -rf` is banned in both maintainer scripts and the spec, and
`package_scripts_test.sh` asserts its absence: it would delete straight through
a mount into another filesystem. Deletion failures set a flag and are reported;
a package manager must never announce a successful purge over state that is
still on disk. That shell matcher decides what gets unmounted and deleted,
so it is the highest-consequence code in the packaging: it is duplicated
byte-identically in `prerm` and `postrm` (postrm runs after every shipped file
is gone and cannot source a helper), and `package_scripts_test.sh` diffs the
two copies and runs the real function against adversarial fixtures — symlinks,
a marker with trailing text, an indented marker, a non-unit filename. The spec
carries two more copies of the same function; the test extracts them, expands
`%%` the way rpm would, and requires all four to be identical. **Write a
literal `%` as `%%` inside a spec scriptlet** — rpm macro-expands scriptlet
bodies, so a bare `printf '%s\n'` is handed to the macro expander.

The guard links `kde_nasmount-core` only; never give it mutation or
`kde_nasmount-root` access, and never launch KAuth from a package-manager script.
Every Fedora removal path must pass `--no-autoremove`: DNF can continue
removing unused dependencies after a failed RPM `%preun`, leaving nasmount
installed without Qt. The Fedora smoke gate snapshots the complete RPM set
around a refused removal to enforce this rule.

Changing the installed file set requires synchronized updates to CMake, the
generated cleanup manifest, `Session::validateInstallManifest()`, source
`uninstall.sh`, DEB/RPM file metadata, and cleanup/package inspection tests.
Debian's KAuth helper directory, Debian multiarch libexec, and Fedora
`lib64/libexec` layouts are intentionally distinct.
Before publishing a draft, run the privileged release checklist on fresh
Kubuntu 26.04 and Fedora KDE 44 VMs; GitHub container smoke tests do not replace
KAuth, polkit, D-Bus, CIFS, service enablement, and reboot checks.

## Project name vs product name

The project is called `kde_nasmount`; the thing it installs is called
`nasmount`. That is deliberate, not an unfinished rename:

| Identity | Value | Where it appears |
|----------|-------|------------------|
| **Project / repository** | `kde_nasmount` | GitHub repo, clone URL, CMake `project()`, the three static library targets, prose |
| **Product / shipped artifact** | `nasmount` | package name, binaries, unit names, KAuth id, on-disk state, config |

The rule: anything a user's machine can observe keeps the name `nasmount`;
anything that exists only in the source tree is `kde_nasmount`. Do not
"finish" the rename by carrying it across that line — every identifier below
is load-bearing on an installed host, and renaming one destroys existing
shares rather than failing loudly:

- `# X-Nasmount-Managed=1` and every `X-Nasmount-*` marker key — frozen
  on-disk format (upgrade rule 4). Renaming makes every existing share
  `Tampered`, and so unarmed at the next boot;
  [`goldenunits_test.cpp`](tests/goldenunits_test.cpp) is the only gate that
  catches it.
- `/etc/nasmount`, `/run/nasmount`, `/run/nasmount-ids`. Upgrade rule 2
  forbids migrating them, and `/etc/nasmount/<id>.cred` is baked into the
  `Options=` line of every golden unit — frozen by the same gate as the
  marker, not merely by policy.
- `~/.config/nasmountrc`.
- `nasmount-boot.service` and `90-nasmount.preset`.
- The KAuth/D-Bus/polkit id `io.github.pakru.nasmount` and its action names;
  renaming them is an upgrade-compatibility change (upgrade rule 5) and buys
  nothing.
- Package names (`nasmount` in both families), the six installed binaries,
  `kcm_nasmount.so`, `kcm_nasmount.desktop`, `nasmount.desktop`,
  `share/nasmount/`, `share/doc/nasmount/`, and the release asset names
  `nasmount-amd64-<version>.deb` / `nasmount-fedora44-x86_64-<version>.rpm`.

Consequently [`cleanupvalidation.cpp`](src/session/cleanupvalidation.cpp)'s
allowlist, `packaging/cleanup-manifest.in`, `uninstall.sh`, both package file
lists, and the artifact filename *values* in `packaging/lib.sh` must never
change for a naming reason. The build/CI variable prefix is still
`NASMOUNT_*` (`NASMOUNT_PACKAGE_FAMILY`, `NASMOUNT_CONTAINER_ENGINE`): it is
source-tree-only, so it could carry the project name, but renaming it has no
external effect and crosses four syntaxes, so it was deliberately left alone.

Release provenance attestations are bound to the repository name. Tags
v0.1.0–v0.1.4 reference `pakru/kde_mount`; anything published after the
rename references `pakru/kde_nasmount`. The discontinuity is expected, not
tampering.

## Architecture and the linkage invariant

Three static libraries, and which binaries may link them is a **security
boundary**, not a style preference:

| Library | Contents | Linked into |
|---------|----------|-------------|
| `kde_nasmount-core` | validation, unit-value encoding, read-only state model (`src/core`) | everything, helper included |
| `kde_nasmount-session` | KConfig store, per-user lock, KAuth call wrapper, async operation controller, display model, `smb://` address conversion (`src/session`) | dialog, KCM, cleanup — **never the helper** |
| `kde_nasmount-root` | durable fd-based filesystem ops, root lock, systemd execution, credential/runtime stores (`src/root`) | `nasmount-helper`, `nasmount-boot` **only** |

- Everything is **STATIC** on purpose: the privileged helper must not depend on
  a `.so` an unprivileged user could replace. Don't convert these to shared.
- `kde_nasmount_assert_no_root_link()` in [`CMakeLists.txt`](CMakeLists.txt#L271)
  fails the configure step if `kde_nasmount-root` ever reaches
  `kde_nasmount-session`, the dialog, the cleanup tool, the package guard, or
  the KCM. Structural placement is the real defence; that check catches
  accidents. Add any new unprivileged target to that list.
- The helper ([`src/helper/helper.cpp`](src/helper/helper.cpp)) is deliberately
  thin: caller validation, typed argument decoding, root-lock acquisition,
  dispatch into `kde_nasmount-root`, reply conversion. **Do not add filesystem or
  systemd mutation there** — it belongs in `src/root`.
- Everything in a helper argument map is untrusted. Caller identity comes only
  from `KAuth::HelperSupport::callerUid()`, never from the arguments. Validation
  done in the dialog or KCM is UX feedback and is re-done in the helper.

What gets installed:

| Artifact | Runs as | Role |
|----------|---------|------|
| `nasmount-helper` | root, D-Bus activated by KAuth | the only mutator reachable from a user |
| `nasmount-boot` | root, `nasmount-boot.service` | arms every valid share at boot; authorizes nothing |
| `nasmount-dialog` | user | Dolphin service menu (QML); also the credential-lookup child, below |
| `kcm_nasmount.so` | user | System Settings module, QML in [`src/kcm/ui/`](src/kcm/ui/) |
| `nasmount-cleanup` | user | authenticated, owner-scoped purge via the `purge` action |
| `nasmount-uninstall` | user | shell script ([`packaging/nasmount-uninstall.sh`](packaging/nasmount-uninstall.sh)): cleanup, then apt/dnf |
| `nasmount-package-guard` | on demand (CI upgrade jobs) | libexec, read-only state classifier; links `kde_nasmount-core` only |

The KAuth actions are `definesystem`, `undefinesystem`, `purge` (all
`auth_admin`) and `inventory` (read-only, `Policy=yes`).

Both front ends render the same form,
[`src/kcm/ui/ShareForm.qml`](src/kcm/ui/ShareForm.qml) — the KCM picks it up
by directory glob, `nasmount-dialog` embeds it via
[`src/dialog/dialog.qrc`](src/dialog/dialog.qrc). It must stay host-agnostic:
no `kcm`/`backend` reference inside it, everything injected as a property
(`actions`, `fixedUnc`, …). A single host reference there silently makes it
usable by one front end only, and the two front ends drift apart. QML
reaches C++ by name at runtime, so a C++ rename compiles cleanly and breaks on
the first click; [`qml_invokable_gate.sh`](tests/qml_invokable_gate.sh)
requires every `actions.<name>(` to be a `Q_INVOKABLE` on
`Session::MountActions` and every `kcm.`/`backend.` name to exist on its host.

ShareForm also has a **read-only mode**, which is the KCM's Details view of a
saved share
([`ShareDetailsDialog.qml`](src/kcm/ui/ShareDetailsDialog.qml)). It is
presentation, not an edit path: a read-only form cannot submit and is sealed
against credential suggestions, and the password is never shown because
nothing returns it. **ShareForm must never import Kirigami**:
`shareform_qml_test` loads it in both native package builds, whose containers
have no Kirigami, while a workstation does, so the import would pass every
local test. `removed_api_gates.sh` enforces this. The KCM's own page
(`main.qml`) does import Kirigami, which is therefore a **runtime-only**
dependency in both families (`qml6-module-org-kde-kirigami` / `kf6-kirigami`),
and in none of the six build lists; if a test ever loads `main.qml`, it
becomes a build dependency in all six.

The KCM list's per-row meaning (state text, severity, access, the single
removal a row offers and why none) is decided in pure functions beside
`classifyRow()` in [`mountmodel.h`](src/session/mountmodel.h), which
`mountmodel_test` covers: `presentRow()` and `rowRemoval()`. QML only renders
those values. Access and authentication are shown only for a validated
marker, and are blank otherwise, never a defaulted read-write.

The KCM's **Open** asks the file manager over D-Bus
(`org.freedesktop.FileManager1`) rather than using Qt's own URL opening. Under
Plasma, Qt hands the URL to KIO's `OpenUrlJob` inside System Settings, which
examines the path, and examining an automount point triggers the mount, so
the GUI thread would block until it completed or timed out.
`removed_api_gates.sh` bans Qt's URL-opening calls in `src/kcm`.

### `smb://` addresses

Users see and type `smb://host/share`. `//host/share` is still accepted as
input. `smb://` is **only a spelling for display and input**: the unit's
`What=` and `Description=`, the golden units, Store's `unc`, and the
`definesystem` helper's `unc` argument all stay `//host/share`, because
`mount.cifs` requires that form, the unit body is frozen (upgrade rule 4), and
old and new front ends and helpers must interoperate (upgrade rule 5).
`MountActions::addShare()` converts before the helper call *and* the Store
commit: Store's UNC is re-validated as `//` by the drift comparison.

The conversion is
[`Session::ShareAddress`](src/session/shareaddress.h), in the **session
library, never core**. Core is linked into the helper, so keeping the parser
out of every library the helper links guarantees, structurally, that the
helper can never be handed an `smb://` value and accept it.
`removed_api_gates.sh` enforces that placement, and also stops the
service-menu dialog from growing its own parser again.
`displayUrl()` keeps `%`, `#` and `?` percent-encoded, as Dolphin does, so an
address copied from the list pastes back into Add unchanged;
`shareaddress_test` holds that round trip.

### Credential autofill (service-menu dialog only)

The dialog pre-fills the credential fields from KDE's password service (README
"Credential autofill"). It is an *import* into the normal Define,
not a second credential source, and these rules keep it that way:

- `KF6::KIOCore` (for `KPasswdServerClient`) links into `nasmount-dialog`
  **only** — never core, session, the KCM, or anything privileged.
- The lookup runs in a short-lived child process: `nasmount-dialog` re-invoked
  with a private flag, speaking a versioned, size-capped protocol over pipes
  ([`credentiallookup.h`](src/dialog/credentiallookup.h),
  [`credentiallookupworker.cpp`](src/dialog/credentiallookupworker.cpp)). Not a
  thread: `KPasswdServerClient` blocks in a nested event loop with no deadline
  and may show a wallet prompt, and only a process can be abandoned. It is a
  mode of the existing binary so the installed file set is unchanged —
  splitting it out is an installed-file-set change (the synchronized-update rule
  under "Native packages").
- Only `checkAuthInfo()` is ever called. Never `queryAuthInfo()` or
  `addAuthInfo()`: nasmount never asks the password service to store or prompt.
- A suggestion is **all-or-nothing and one-shot**: applied as one tuple,
  sealed the moment the user edits any credential field
  (`ShareForm.credentialsSealed`), never written to Store, a file, or a log. An
  imported password must never pair with a typed username;
  `shareform_qml_test` loads the real `ShareForm.qml` to hold that.
- When the `smb://` URL names a user, a candidate for another account is
  refused (`acceptCandidate()`), not substituted.
- It takes no lock — it mutates nothing — and the helper re-validates the
  result like any typed credential. The KCM's Add form stays manual.
- `NASMOUNT_DEBUG_LOOKUP=1` prints why a lookup missed: reasons only, never a
  username, password, or length.

## Conventions

- Every header, QML file and standalone `.cpp` (`main.cpp`, `helper.cpp`)
  opens with a block comment: what the unit is, then
  `SPDX-License-Identifier: GPL-3.0-or-later`, then the *reasoning* that a
  reader would otherwise have to reconstruct. A `.cpp` that implements a header
  opens with the SPDX line alone — its unit description lives in the header.
  Headers carry the API contracts as `/** ... */` doc comments; `.cpp` files
  carry implementation reasoning beside the code. Match this density — it is
  unusually high and it is intentional.
- KDE/Qt style: 4 spaces, brace on its own line for functions and attached for
  control flow, `const QString &` parameters, `QStringLiteral` for literals,
  namespaces `UnitSpec` / `UnitValue` / `Verify` / `Session` / `Root::*`. There
  is no `.clang-format`; follow the surrounding file.
- Errors are reported through `bool` returns plus a `QString *error`
  out-parameter, not exceptions. There is **no logging framework** (no
  `qDebug`/`qWarning` anywhere in `src`) — user-facing output goes to
  `QTextStream(stdout/stderr)` in the standalone binaries only, and nothing
  ever prints credential material.
- No in-place Edit exists anywhere. Changing a share's UNC, mount point,
  credentials, authentication or mode is Delete then Add. Don't reintroduce an
  edit path.
- There is one lifecycle: every share is defined with a root-owned `/etc`
  credential and armed at boot. There is no Session/System choice, no
  KWallet, no per-share reconnect switch, and no runtime verb
  (connect/arm/disarm/mount-now). A sign-in-scoped mode (credential under
  `/run`, password in KWallet, a per-user supervisor arming after login)
  existed and was removed: it could not arm before someone logged in and
  unlocked a wallet, so it could not survive a reboot unattended, and it
  depended on a wallet this tool cannot guarantee is reachable. Don't
  reintroduce it without answering both. The only residue is the fixed
  `# X-Nasmount-Mode=system` marker line, kept because the format is frozen.
- Properties of an *existing* definition are still always re-derived from the
  validated unit marker via `Verify::inspectDefinition()` — never from the
  Store and never from a caller-supplied flag.
- Every client mutation runs on a worker thread under `Session::UserLock`; every
  privileged mutation runs under `Root::RootLock`. KAuth calls are unbounded
  waits and must never touch the GUI thread.

## Tests

The suite is 25 CTest entries: 19 C++ test binaries, four shell gates
(`removed_api_gates.sh`, `qml_invokable_gate.sh`, `package_scripts_test.sh`,
`packaging_metadata_test.sh`), `version_metadata` (a CMake script checking
`VERSION` against every generated version string), and ECM's `appstreamtest`.
Both native package builds run all of it.

`tests/*.cpp` are plain `main()` binaries using a local harness (`static int
passed/failed` plus a `check(label, condition, detail)` helper, `return failed
== 0 ? 0 : 1`) — **not** QTest. Copy the pattern from an existing test. Code
that lives in no library (the dialog's `smburl.cpp`, `credentiallookup.cpp`)
is tested by compiling its translation units straight into the test, without
`KF6::KIOCore`, so nothing needs a wallet or a NAS. Those tests also link
`kde_nasmount-session`, where the `smb://` parser and the domain/user split
they share with the KCM live. Tests that read the source
tree (`goldenunits_test`, `shareform_qml_test`) get the path as a compile
definition rather than a staged copy, which could go stale.

Adding a C++ test means editing **three** places:

1. `tests/<name>_test.cpp`;
2. `CMakeLists.txt` — `add_executable` + `target_link_libraries` + `add_test`;
3. [`install.sh`](install.sh#L45) — the explicit test-binary list, which gates
   installation. It is a hand-maintained list; a new test not added there is
   silently skipped at install time.

A new shell gate likewise needs its `add_test` *and* its own `bash …` line in
`install.sh`.

The same hand-maintained-list trap exists in CI. `packaging_metadata_test.sh`
compares both workflows' job names to an **exact set**, and `ci_success`
carries its own `needs`/`env`/loop; a job added to the workflow but not to all
of those runs without gating anything. The test asserts that wiring for
`upgrade_deb` and `upgrade_rpm`.

[`tests/removed_api_gates.sh`](tests/removed_api_gates.sh) is a grep-based gate
that fails the suite if deleted APIs return — the transaction/recovery engine,
tombstones/Forget, automatic partial repair, in-place Edit/Replace, pending-
transaction presentation roles, runtime coupling in the privileged inventory,
and stale transaction-storage prose in `README.md`, `src` and the install
scripts. It also holds four placement rules:
- the `smb://` parser stays out of core, the helper and the root library;
- the service-menu dialog does not regain its own parser;
- ShareForm never imports Kirigami;
- the KCM never uses Qt's own URL opening.

The gate greps comments too, so word explanations around a forbidden name
rather than spelling it out. If a build fails there, the fix is to stop using
the forbidden identifier, not to loosen the pattern.

Privileged accept paths and real reboot / no-login behaviour are **not** covered
locally and must be validated in a disposable VM.

## Gotchas

- Unit values are encoded by `UnitValue::encodeUnitValue()`; its rules
  (percent-doubling, backslashes and quotes *not* interpreted outside a leading
  quote) were verified empirically against real systemd, not just
  `systemd-analyze verify`. Don't "fix" the encoder from first principles.
- Mount-point authorization is **lexical and never canonicalizing**;
  `openMountpointNoFollow()` walks only the authorized suffix with
  `O_NOFOLLOW`. Authorizing a canonical path and then operating on the lexical
  one is a symlink bypass.
- Generation and validation of unit bodies share the same fixed-value functions,
  so any functional deviation becomes `Tampered`. If you change generation,
  change the shared function — never the two sides separately.
- Every KAuth mutation is `auth_admin` by design, with no passwordless tier:
  nothing is ever invoked unattended, since boot arming runs as root. The
  header of [`io.github.pakru.nasmount.actions`](io.github.pakru.nasmount.actions)
  also explains why `auth_admin_keep` is written as plain `auth_admin`
  (`kauth-policy-gen` cannot emit `_keep`). Adding an action means editing
  that file — and is upgrade rule 5.
- Requires Linux 6.8+ (`STATX_MNT_ID_UNIQUE`), Plasma 6 / KF6, `cifs-utils`.
