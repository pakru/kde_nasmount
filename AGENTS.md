# AGENTS.md - kde_nasmount

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
make pot              # regenerate po/kcm_nasmount.pot, merge it into every language
make install          # runs ./install.sh - NOT with sudo
make uninstall        # runs ./uninstall.sh
make deb              # .deb in dist/deb/, built in the pinned Ubuntu 26.04 image
make rpm              # .rpm in dist/rpm/, built in the pinned Fedora 44 image
make packages         # both
make clean            # rm -rf build/
make dist-clean       # rm -rf dist/
```

- **Never run `install.sh` as root** - it refuses. It builds as you and
  elevates only for `cmake --install`.
- The prefix is **`/usr`**, not `/usr/local`: D-Bus only scans
  `/usr/share/dbus-1/system-services` and polkit only
  `/usr/share/polkit-1/actions`. A `/usr/local` install builds and then
  silently fails to authenticate.
- Installing is disruptive (writes system D-Bus/polkit files and enables the
  boot service). Don't install unless asked.
- `uninstall.sh` reads `build/install_manifest.txt` and refuses to run without
  it, so **uninstall only works from the build tree that installed** - a clean
  checkout, or one where `make clean` has run, cannot uninstall until
  `install.sh` regenerates the manifest. The manifest content is deterministic
  (validated against the hardcoded allowlist in
  [`cleanupvalidation.cpp`](src/session/cleanupvalidation.cpp#L37), which also
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
CI**, then lints the result - so a local failure is a failure CI would have
had. It needs podman or docker (`NASMOUNT_CONTAINER_ENGINE=` overrides the
choice); on the matching distribution, call the build script directly instead.
The working tree is copied in, uncommitted changes included, and the source is
mounted read-only so a build can never write back into it.

The image digests are pinned in `build-in-container.sh`, and repeated in
every container job of `ci.yml` and `release.yml`. `packaging_metadata_test.sh`
compares the helper only against the *first* pin of each distribution in
`ci.yml`, because a local build against a different image proves nothing about
CI - the other `ci.yml` copies and all of `release.yml` are unchecked, so bump
every copy together. Each target clears only its own `dist/<family>/`
subdirectory, since the build scripts require an empty output directory.

Build dependencies are **six hand-maintained lists** that must agree:
`packaging/debian/control`, the spec's `BuildRequires`, the DEB and RPM lists
in `build-in-container.sh`, every Ubuntu/Fedora build block in `ci.yml`, and
`release.yml`. A missing entry fails only in the distribution it was forgotten
in. `packaging_metadata_test.sh` checks those lists only for the following:
the KIO and QML-runtime dependencies the autofill feature added
(`libkf6kio-dev`/`kf6-kio-devel`, `qml6-module-qtquick-dialogs`), `gettext`,
and the KI18n floor; extend it when you add another. `gettext` is named
although KI18n's CMake config already pulls it in, because the build itself now
runs `msgfmt` (compiling the catalogs) and the suite runs `xgettext` and
`msgcat` (the translations gate), and a tool that arrives only through someone
else's dependency vanishes in whichever distribution stops providing it. The
test requires it as a whole list entry, so `gettext-base` or `gettext-devel`
cannot stand in, in every list and every `ci.yml`/`release.yml` build block.
The KI18n floor (6.8, the first with `KLocalizedQmlContext`) is the version in
`find_package(KF6I18n ...)`; the test reads it from `CMakeLists.txt` and
requires `debian/control` and the spec to say the same. The password service
itself is a weak dependency in both families (`Recommends:`), never
`Requires:` - a host without it must still install and mount with a typed
credential. So is `kio-extras`, which ships the smb KIO worker behind the Add
dialog's Share "Browse…": without it Browse reports an unknown protocol and a
typed address still works. `packaging_metadata_test.sh` asserts both are
`Recommends:` and neither is a hard dependency.

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
`nasmount-fedora44-x86_64-<version>.rpm`. README's one-line download commands and the
generated Release-note install commands depend on those exact names.

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
   it has no equivalent - and DNF5 downgrades freely on an exact version spec,
   which a local `.rpm` is. The RPM therefore has **no downgrade protection at
   any layer**. Don't add a `%pre` version check; it cannot work. If a
   compatibility gate is ever needed, gate on an on-disk format marker under
   `/etc/nasmount`, which works identically in both families.
4. The generated unit body and the marker schema are a **frozen on-disk
   format**. A share's unit pair survives the upgrade, so new binaries must
   still accept bytes old ones wrote - and because generation and validation
   share the same fixed-value functions, changing one changes both, leaving the
   rest of the suite green while every existing share becomes `Tampered`.
   [`goldenunits_test.cpp`](tests/goldenunits_test.cpp) is the only gate that
   catches this. If it fails, revert the change or design a migration and raise
   `MIN_UPGRADABLE_VERSION` - **never regenerate
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
   marker field:` - `Tampered`, and so unarmed at the next boot.
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
owner-scoped cleanup while KAuth/polkit are installed, then invokes apt/dnf -
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

`nasmount-package-guard` still ships and still classifies host state - the CI
upgrade jobs use it to prove state survived - but it is **diagnostic only**. It
must not reappear in any maintainer script, where it would refuse the removal
this design performs; both test files assert its absence.

Two consequences of the DEB path worth stating outright: `apt remove` destroys
managed shares with no authentication and no owner check, and it cannot clear
any user's `~/.config/nasmountrc` because it has no idea which account owns a
share. `nasmount-uninstall` remains the complete, owner-scoped path.

Managed units are identified **only** by the exact marker line
`# X-Nasmount-Managed=1`, never by filename - a user chooses the filename via
their mount point. The *name* is then checked against the alphabet
`systemd-escape` can emit (alphanumerics, `:`, `_`, `.`, `-`, and the backslash
of a `\xNN` escape) and anything else is skipped with a warning. That check is
load-bearing, not cosmetic: **systemctl expands `*`, `?` and `[...]` in a unit
argument as a pattern over every loaded unit**, so a marked file named
`*.mount` would turn `systemctl stop` into "stop every mount unit on the
system" - and `--` does not help, since it ends option parsing, not globbing.

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
two copies and runs the real function against adversarial fixtures - symlinks,
a marker with trailing text, an indented marker, a non-unit filename. The spec
carries two more copies of the same function; the test extracts them, expands
`%%` the way rpm would, and requires all four to be identical. **Write a
literal `%` as `%%` inside a spec scriptlet** - rpm macro-expands scriptlet
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

One family of installed files is not named by hand: the translation catalogs,
`share/locale/<lang>/LC_MESSAGES/kcm_nasmount.mo`. It is the single family whose
size varies, one per language under `po/`, possibly none. CMake generates their
manifest lines at configure time from the `po/` tree, not from `build/locale`,
which is empty before a build and can hold a deleted language's leftovers. Both
allowlists that decide what a privileged uninstall deletes,
`validateInstallManifest()` and `uninstall.sh`, admit them by one strict
pattern: this project's catalog name, inside a directory spelled `ll`, `ll_CC`
or `ll@variant`, which nothing a path could traverse with can match. A catalog
counts into the exact-size check but never toward the required components, so a
manifest cannot trade a missing component for one, and zero catalogs is valid.
The language alphabet is spelled in four places (CMake's configure-time check,
twice in `cleanupvalidation.cpp`, and `uninstall.sh`);
[`translations_gate.sh`](tests/translations_gate.sh) fails when one drifts.
`uninstall.sh` matches in the C locale (`is_allowed_path()`), because a bracket
range follows the locale's collation otherwise, admitting `é` under
`en_US.UTF-8` and refusing a plain `i` under Turkish, and the same manifest
would be accepted or refused depending on who runs the uninstall; the gate runs
that allowlist for real under the locales known to differ that the host has.
The DEB lists nothing (`nasmount.install` is `usr/*`); the RPM collects the
catalogs with `%find_lang kcm_nasmount` into `kcm_nasmount.lang` and
`%files -f`, which marks each one `%lang(xx)`. A listed catalog may be absent on
disk (dpkg `path-exclude`, rpm `%_install_langs`), so nothing that reads the
manifest may treat that as an error: the validator reads only its text.

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
"finish" the rename by carrying it across that line - every identifier below
is load-bearing on an installed host, and renaming one destroys existing
shares rather than failing loudly:

- `# X-Nasmount-Managed=1` and every `X-Nasmount-*` marker key - frozen
  on-disk format (upgrade rule 4). Renaming makes every existing share
  `Tampered`, and so unarmed at the next boot;
  [`goldenunits_test.cpp`](tests/goldenunits_test.cpp) is the only gate that
  catches it.
- `/etc/nasmount`, `/run/nasmount`, `/run/nasmount-ids`. Upgrade rule 2
  forbids migrating them, and `/etc/nasmount/<id>.cred` is baked into the
  `Options=` line of every golden unit - frozen by the same gate as the
  marker, not merely by policy.
- `~/.config/nasmountrc`.
- `nasmount-boot.service` and `90-nasmount.preset`.
- The KAuth/D-Bus/polkit id `io.github.pakru.nasmount` and its action names;
  renaming them is an upgrade-compatibility change (upgrade rule 5) and buys
  nothing.
- Package names (`nasmount` in both families), the seven installed binaries,
  `kcm_nasmount.so`, `kcm_nasmount.desktop`, `kcm_nasmount.mo` (the one
  translation catalog, named after the KCM's plugin id because that is the
  domain KCMUtils fixes for its QML; see "Translations"), `nasmount.desktop`,
  `nasmount-manager.desktop`,
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
| `kde_nasmount-core` | validation, unit-value encoding, read-only state model, the read-only mount-point preview (`src/core`) | everything, helper included |
| `kde_nasmount-session` | KConfig store, per-user lock, KAuth call wrapper, async operation controller, display model, `smb://` address conversion, the Add form's mount-point checks, the credential-lookup protocol and controller (`src/session`) | dialog, KCM, cleanup - **never the helper** |
| `kde_nasmount-root` | durable fd-based filesystem ops, root lock, systemd execution, credential/runtime stores (`src/root`) | `nasmount-helper`, `nasmount-boot` **only** |

- Everything is **STATIC** on purpose: the privileged helper must not depend on
  a `.so` an unprivileged user could replace. Don't convert these to shared.
- `kde_nasmount_assert_no_root_link()` in [`CMakeLists.txt`](CMakeLists.txt#L378)
  fails the configure step if `kde_nasmount-root` ever reaches
  `kde_nasmount-session`, the dialog, the cleanup tool, the package guard, or
  the KCM. Structural placement is the real defence; that check catches
  accidents. Add any new unprivileged target to that list. Two more checks sit
  beside it and guard the translation boundary:
  [`kde_nasmount_assert_no_i18n_link()`](CMakeLists.txt#L404) and
  [`kde_nasmount_assert_translation_domain()`](CMakeLists.txt#L441); see
  "Translations".
- The helper ([`src/helper/helper.cpp`](src/helper/helper.cpp)) is deliberately
  thin: caller validation, typed argument decoding, root-lock acquisition,
  dispatch into `kde_nasmount-root`, reply conversion. **Do not add filesystem or
  systemd mutation there** - it belongs in `src/root`.
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
| `nasmount` | user | shell script ([`packaging/nasmount.sh`](packaging/nasmount.sh)): `exec kcmshell6 kcm_nasmount`, with the menu entry `nasmount-manager.desktop` |
| `nasmount-uninstall` | user | shell script ([`packaging/nasmount-uninstall.sh`](packaging/nasmount-uninstall.sh)): cleanup, then apt/dnf |
| `nasmount-package-guard` | on demand (CI upgrade jobs) | libexec, read-only state classifier; links `kde_nasmount-core` only |

The KAuth actions are `definesystem`, `undefinesystem`, `purge` (all
`auth_admin`) and `inventory` (read-only, `Policy=yes`).

Both front ends render the same form,
[`src/kcm/ui/ShareForm.qml`](src/kcm/ui/ShareForm.qml) - the KCM picks it up
by directory glob, `nasmount-dialog` embeds it via
[`src/dialog/dialog.qrc`](src/dialog/dialog.qrc). It must stay host-agnostic:
no `kcm`/`backend` reference inside it, everything injected as a property
(`actions`, `fixedUnc`, …). A single host reference there silently makes it
usable by one front end only, and the two front ends drift apart. QML
reaches C++ by name at runtime, so a C++ rename compiles cleanly and breaks on
the first click; [`qml_invokable_gate.sh`](tests/qml_invokable_gate.sh)
requires every `actions.<name>(` to be a `Q_INVOKABLE` on
`Session::MountActions` and every `kcm.`/`backend.` name to exist on its host.

### Validation and the two Browse buttons

ShareForm **holds no validation rules of its own.** Each field's verdict is a
string from `actions` - empty means fine - computed by the functions the submit
path and the helper use, so a value the form accepts is a value `addShare()`
accepts. That is why `Session::ShareAddress::shareInputProblem()` is
`resolveShareInput()`'s own refusal and `Session::mountPointProblem()` applies
the helper's `UnitSpec::validateMountpoint()` to the same
`canonicalMountPoint()` the submit sends; `shareaddress_test` and
`mountactions_test` hold that parity. A problem always gates Add but is only
*shown* once its field has been revealed (a typing pause, leaving the field, a
Browse pick, a host pre-fill), so the pre-filled `smb://` is not scolded.

Two speeds, and the split is a rule:

- **Synchronous** checks are lexical: no process, no filesystem access. They
  sit in bindings and run on every keystroke.
- **Anything that touches the system** - naming unit files with
  `systemd-escape`, looking in `/etc/systemd/system`, examining the folder -
  runs in `MountActions::checkMountPoint()` on a private one-thread pool,
  single-flight with the newest waiting request winning, and answers through
  `mountPointChecked(raw, problem)`, which a form applies only if the field
  still holds `raw`. The folder is examined by
  `UnitSpec::previewMountpointProblem()`, which **must never trigger an
  automount** (statx with `AT_NO_AUTOMOUNT`, descending only by `O_PATH`
  into components shown not to be a mount or a trigger): the path may sit on
  another share's automount point, and looking at it from System Settings
  would mount that share and stall. It lives beside the helper's walk it
  mirrors, and `unitspec_test` runs both against the same fixtures.
- A check that has not answered, or cannot tell, never blocks Add; the helper
  is the authority. Every form on one `actions` hears every answer, so a
  read-only form ignores them.

`~` in the mount point is expanded to the caller's passwd home (not `$HOME`) by
`canonicalMountPoint()`, so the check and the submit mean the same path; it
only gives meaning to input the helper always refused as relative.

The Share **Browse…** is the platform `FolderDialog` opened at `smb://`. Under
Plasma that is KDE's KIO-backed dialog, so the KCM links nothing for it. A pick
goes through `ShareAddress::browsedShareInput()` and lands in the field as if
typed; a user in the picked URL fills Username by the rule typing uses and is
kept out of the address. The mount-point Browse converts with
`QUrl::toLocalFile()`, never string surgery: `toString()` keeps `%` and `#`
encoded, so a folder named `100%` would otherwise become `100%25`.

The Dolphin window's height follows its content for the same reason the form
explains problems inline: a fixed height cuts the buttons off when a message
appears.

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

### Credential autofill

Both front ends pre-fill the credential fields from KDE's password service: the
service-menu dialog when it opens, and the KCM's Add dialog after a Share
Browse pick (never while the address is typed: a wallet prompt should follow
something the user just did with SMB, not a keystroke). It is an *import* into
the normal Define, not a second credential source, and these rules keep it
that way:

- `KF6::KIOCore` (for `KPasswdServerClient`) links into `nasmount-dialog`
  **only** - never core, session, the KCM, or anything privileged.
- The lookup runs in a short-lived child process: `nasmount-dialog` re-invoked
  with a private flag, speaking a versioned, size-capped protocol over pipes.
  The parent half - protocol, `acceptCandidate()` policy, `Controller`,
  `ProcessTransport` - is in the session library
  ([`credentiallookup.h`](src/session/credentiallookup.h)) so both front ends
  share it; only the child
  ([`credentiallookupworker.cpp`](src/dialog/credentiallookupworker.cpp))
  calls the password service. The service menu runs the child as itself; the
  KCM runs it by an **absolute path fixed at build time**
  (`NASMOUNT_DIALOG_PROGRAM`), never a `PATH` lookup, and a program that will
  not start is an ordinary miss. Not a thread: `KPasswdServerClient` blocks in
  a nested event loop with no deadline and may show a wallet prompt, and only
  a process can be abandoned. It is a mode of the existing binary so the
  installed file set is unchanged - splitting it out is an installed-file-set
  change (the synchronized-update rule under "Native packages").
- Only `checkAuthInfo()` is ever called. Never `queryAuthInfo()` or
  `addAuthInfo()`: nasmount never asks the password service to store or prompt.
- A suggestion is **all-or-nothing and one-shot**: applied as one tuple,
  sealed the moment the user edits any credential field
  (`ShareForm.credentialsSealed`), never written to Store, a file, or a log. An
  imported password must never pair with a typed username;
  `shareform_qml_test` loads the real `ShareForm.qml` to hold that.
- When the `smb://` URL names a user, a candidate for another account is
  refused (`acceptCandidate()`), not substituted.
- Where the share can change (the KCM types it) an imported login is **bound
  to the share it was found for**. A suggestion is refused unless the Share
  field still holds the address it was requested for, and an applied one is
  withdrawn if the address moves to another server or share
  (`lookupTargetOf()` keys them; a subfolder of the same share keeps it).
  Without this a password found for one machine could be submitted to another
  after a retype. Only flags and a key are kept to decide it, never the
  values; what the user typed since is theirs.
- It takes no lock - it mutates nothing - and the helper re-validates the
  result like any typed credential.
- `NASMOUNT_DEBUG_LOOKUP=1` prints why a lookup missed: reasons only, never a
  username, password, or length. It is the one place library code writes to
  stderr (`Session::CredentialLookup::debugReport()`), because the KCM shares
  it with the service menu; it is opt-in, and in System Settings it goes to
  that process's stderr.

### Translations

Only the two GUI front ends translate: the KCM and the dialog, with the
session library they share, plus the metadata files. The privileged side, the
diagnostics and the command-line tools do not. Russian (`ru`) is the one
shipped language; the checklist for the next is in
[`CONTRIBUTING.md`](CONTRIBUTING.md#translating), and
[`translations_gate.sh`](tests/translations_gate.sh) enforces it.

- **One catalog, `kcm_nasmount`**, for every translated string:
  `po/<lang>/kcm_nasmount.po`, installed as
  `share/locale/<lang>/LC_MESSAGES/kcm_nasmount.mo`. KCMUtils sets its own
  engine's QML translation domain to the plugin id (`kcm_nasmount`) and this
  project cannot change that, while `ShareForm.qml` is loaded by two hosts
  (the KCM and `nasmount-dialog`). The only domain the shared form can use
  under both is the plugin id, so it uses plain `i18n()` and stays
  host-agnostic; two catalogs would make it name one (`i18nd`) and so belong to
  one host. The name is defined once, as `NASMOUNT_TRANSLATION_DOMAIN` in
  `CMakeLists.txt`, and reaches C++ as `TRANSLATION_DOMAIN`. Don't spell the
  literal in C++; QML never names a domain. The file name is observable on a
  user's machine, so it is frozen once shipped (see "Project name vs product
  name").
- **`TRANSLATION_DOMAIN` on every translating target**:
  `kde_nasmount-session`, `nasmount-dialog` and `kcm_nasmount`. Without the
  macro `i18n()` uses the *application's* catalog, which inside System Settings
  is System Settings' own, so nothing fails and the strings just stay English.
  `kde_nasmount_assert_translation_domain()` fails configure for a target that
  lacks it. A translation unit compiled straight into a test needs it too
  (`smburl_test` builds the dialog's `smburl.cpp`). The dialog's `main()` also
  makes the catalog its application domain and calls
  `KLocalization::setupLocalizedContext()` on its engine *before* `load()`:
  without that `i18n()` is not defined there and a binding in `ShareForm.qml`
  throws as soon as it evaluates. `shareform_qml_test` and `i18n_test` give
  their bare engines the same context for the same reason.
- **Core, root, helper, boot and the package guard never translate.** The
  helper runs as root with no knowledge of the caller's language and core is
  linked into it, so whatever they produce (`UnitSpec` validation errors,
  `Verify` details, the helper's replies) is English inside the translated UI.
  That is accepted, not an oversight; translating it needs codes passed across
  the KAuth boundary, not a call to `i18n()` in code the helper links.
  `kde_nasmount_assert_no_i18n_link()` fails configure if KI18n, or the session
  library that carries it, reaches those targets, directly or through an
  INTERFACE-only link; `removed_api_gates.sh` catches a call that needs no new
  link. Several more stay English on purpose, though they sit in a library that
  otherwise translates: the `NASMOUNT_DEBUG_LOOKUP` report and the credential
  lookup protocol's parse errors (the reason a lookup missed reaches nothing
  the user sees), `nasmount-cleanup`'s own output, the install-manifest
  validator's errors (`cleanupvalidation.cpp`) and `purgeApplicationData()`'s
  errors (only that tool calls either). Wrapping one would pull command-line
  text into the catalog at the next `make pot`. `nasmount-cleanup` shares
  `UserLock` and the helper-call fallback with the GUIs, so a couple of its
  error lines can appear translated; that is a side effect, not a feature to
  extend.
- **`ShareForm.qml` uses plain `i18n()`/`i18nc()`, never `i18nd*`, and never
  `import org.kde.ki18n`.** The prefixed `KI18n.i18n()` form needs that QML
  module importable wherever the form loads, which includes both package
  containers because `shareform_qml_test` loads the real form there. It would
  put a QML runtime module in all six build lists and raise the KDE Frameworks
  floor to 6.21, for qmllint visibility only. It is the Kirigami rule's reason
  again: whatever the form imports must exist in the package containers.
- **Writing a string.**
  1. Whole sentences with placeholders, never pieces joined with `+`:
     `i18n("Remove the network mount at %1?", mountPoint)`. Translators must be
     able to reorder words. In QML a message is one string literal, never two
     joined with `+` inside the call: the gate's backstop reads line by line and
     can flag the continuation; a long line is fine.
  2. Give a short or ambiguous string a context with `i18nc()`, using KDE's
     semantic markers: `@action:button`, `@title:window`, `@label:textbox`,
     `@option:radio`, `@item:inlistbox` (a state or access shown in the list),
     `@title:group`, `@info:tooltip`, `@info:placeholder`, `@info:status`.
     Sentences take plain `i18n()`. One context per *meaning*: two calls with
     the same context and text are one catalog entry, which is intended when
     the meaning matches (a Foreign share and a Mounted one both read
     "Mounted").
  3. Identifiers are never translated: paths, `smb://` examples, unit names,
     command lines, D-Bus names, `nasmount`, option values. Inside a sentence
     they go in as a placeholder so a translator cannot alter them. KI18n
     formats an integer argument for the user's locale (digit grouping), so
     pass an id-like number such as a uid or port as `QString::number(n)`, and
     use `i18np()` for a real count.
  4. A translator note is `// i18n: ...` directly above the call, in C++ and
     QML; the extractor collects it (`--add-comments=i18n`). Nothing may stand
     between the note and the call: xgettext drops a note that other code
     separates from it, so put the call in its own statement.
  5. Changing English source text orphans every translation of it. Fix wording
     deliberately and in batches, never in passing.
  6. Never compare displayed text for logic. Branch on an enum, role or key,
     and translate at the edge.
- **QML is extracted as JavaScript** (`xgettext -L JavaScript`), never as C++:
  the C++ scanner stops at the first literal of `i18n("a " + "b")` and records
  `"a "` as the whole message, silently.
- **`make pot` and the freshness gate.** `po/extract-messages.sh` rewrites the
  committed `po/kcm_nasmount.pot` from `src/session`, `src/kcm` and
  `src/dialog`, then `msgmerge`s every language. `translations_gate.sh`
  compares the committed template with a fresh extraction message by message
  (context, `msgid`, plural, translator comment and flags), ignoring only
  source references, wrapping, order and the header, so an edit that only
  shifts line numbers demands nothing, while a string added, or a `// i18n:`
  note reworded, without `make pot` fails the suite. A catalog lagging behind the
  template is normal: KI18n falls back to English for any message it lacks.
  The extractor also flags every message that names a `%1`..`%9` placeholder
  `kde-format`, which xgettext does for C++ (`--kde`) but never for
  JavaScript, because `msgfmt --check` verifies a translation's placeholders
  only for a flagged message. `ki18n_install()` runs `msgfmt` without
  `--check`, so the gate does; otherwise a Russian string that dropped a `%1`
  would compile, install and ship.
- **What the gate checks.** Six things, each reported on its own: the `po/`
  layout and language alphabet; `msgfmt --check` on every catalog and the
  `kde-format` flag on the template; template freshness; a backstop for QML
  literals never wrapped (a heuristic: a capitalized literal on a line with no
  translation call. Where it fires on data rather than text, change the code so
  the value is not spelled that way; don't loosen the pattern); every
  language's metadata being complete; and the four spellings of the language
  alphabet agreeing. Configure refuses the same layout mistakes early: a `.po`
  other than `po/<lang>/kcm_nasmount.po`, a `.pmap` or a `scripts/` directory
  (`ki18n_install()` would install them with nothing listing them), and a
  language directory the allowlists would refuse.
- **Metadata translations are inline**, as `Name[ru]=` keys in the two
  `.desktop` files and `io.github.pakru.nasmount.actions` and as
  `"Name[ru]"` in `kcm_nasmount.json`, edited by hand: nothing in gettext
  handles the `.actions` file or the KCM JSON, and for a few languages
  machinery costs more than it saves (revisit at about four). The service menu
  has two `Name=` lines and Dolphin shows the second in its context menu, so
  both need a translation. `kauth-policy-gen` writes each action's `Name` as
  the policy's description and its `Description` as the message the
  authentication prompt shows, so an action needs exactly one of each per
  language. It reads the file through `QSettings` and neither fails nor warns:
  **a comma empties the whole value, a semicolon cuts it there, and double
  quotes and backslashes are dropped**, so no `Name` or `Description` may
  contain one of the four. The gate refuses them.
- **Tests run in English.** `CMakeLists.txt` appends `LANGUAGE=en_US` to every
  test, because many assertions compare English text, and on a workstation with
  the catalog installed (`make install`, or a `.mo` copied to
  `~/.local/share/locale`) and a Russian locale they would see it translated.
  The line must stay after the last `add_test()`: it pins only the tests
  defined above it. Catalogs are invisible under the `C` and `C.UTF-8`
  locales, where glibc's gettext ignores `LANGUAGE` altogether, which is all a
  package container has. So the one test that needs a real catalog, `i18n`
  (the shipped Russian catalog from `build/locale`, read by the session
  library and by `ShareForm.qml` under a context with the shared domain), is
  registered only while `po/ru/kcm_nasmount.po` exists (the `pofiles` target
  exists whenever `po/` does, so it cannot say). It pins `LC_ALL=en_US.UTF-8`
  for itself and exits 77, which CTest reports as *Skipped*, when that locale
  is not installed and the messages locale therefore still reads as `C`. It is
  skipped in both package builds, deliberately: running it there would mean
  installing locales in both containers, which is more list churn for a check
  the structural ones already cover in part. What runs everywhere is the domain assertion at configure
  time, `shareform_qml_test` loading the form under a localization context, and
  the gate.
- **Column widths** in the KCM list come from `MountModel.stateLabels` and
  `accessLabels` (CONSTANT properties over `Session::allStateLabels()` and
  `allAccessLabels()`), the complete translated label sets that `presentRow()`
  itself draws from, measured with `FontMetrics`. Measuring two English
  literals would size the columns for the wrong string in any other language.
  A new `DisplayState` is labelled in `stateLabel()` and nowhere else; the list
  picks it up by itself.
- **Stale catalogs.** `ki18n_install(po)` compiles every
  `po/<lang>/kcm_nasmount.po` into `build/locale/<lang>/LC_MESSAGES/` and
  installs the *whole* `build/locale` directory. A `.mo` left there by a
  language since deleted is installed too, so **delete `build/` after removing
  a language** (the package builds start from a clean tree and
  `inspect-package-tree.sh` would catch the mismatch). `KF_SKIP_PO_PROCESSING`
  turns the whole thing off: nothing is compiled, installed or listed.

## Conventions

- Every header, QML file and standalone `.cpp` (`main.cpp`, `helper.cpp`)
  opens with a block comment: what the unit is, then
  `SPDX-License-Identifier: GPL-3.0-or-later`, then the *reasoning* that a
  reader would otherwise have to reconstruct. A `.cpp` that implements a header
  opens with the SPDX line alone - its unit description lives in the header.
  Headers carry the API contracts as `/** ... */` doc comments; `.cpp` files
  carry implementation reasoning beside the code. Match this density - it is
  unusually high and it is intentional.
- KDE/Qt style: 4 spaces, brace on its own line for functions and attached for
  control flow, `const QString &` parameters, `QStringLiteral` for literals,
  namespaces `UnitSpec` / `UnitValue` / `Verify` / `Session` / `Root::*`. There
  is no `.clang-format`; follow the surrounding file.
- Errors are reported through `bool` returns plus a `QString *error`
  out-parameter, not exceptions. There is **no logging framework** (no
  `qDebug`/`qWarning` anywhere in `src`) - user-facing output goes to
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
  validated unit marker via `Verify::inspectDefinition()` - never from the
  Store and never from a caller-supplied flag.
- Every client mutation runs on a worker thread under `Session::UserLock`; every
  privileged mutation runs under `Root::RootLock`. KAuth calls are unbounded
  waits and must never touch the GUI thread.

## Tests

The suite is 27 CTest entries: 20 C++ test binaries, five shell gates
(`removed_api_gates.sh`, `qml_invokable_gate.sh`, `package_scripts_test.sh`,
`packaging_metadata_test.sh`, `translations_gate.sh`), `version_metadata` (a
CMake script checking `VERSION` against every generated version string), and
ECM's `appstreamtest`. Both native package builds run all of it; the `i18n`
test reports *Skipped* there (see "Translations").

`tests/*.cpp` are plain `main()` binaries using a local harness (`static int
passed/failed` plus a `check(label, condition, detail)` helper, `return failed
== 0 ? 0 : 1`) - **not** QTest. Copy the pattern from an existing test. Code
that lives in no library (the dialog's `smburl.cpp`) is tested by compiling
its translation unit straight into the test. The credential lookup's parent
half is in the session library, so `credentiallookup_test` links that without
`KF6::KIOCore`, and nothing needs a wallet or a NAS. Those tests also link
`kde_nasmount-session`, where the `smb://` parser and the domain/user split
they share with the KCM live. Tests that read the source
tree (`goldenunits_test`, `shareform_qml_test`) get the path as a compile
definition rather than a staged copy, which could go stale.

Adding a C++ test means editing two places:

1. `tests/<name>_test.cpp`;
2. `CMakeLists.txt` - `add_executable` + `target_link_libraries` + `add_test`.

A new shell gate needs only its `add_test`. Either kind goes above the block
at the end of `CMakeLists.txt` that pins `LANGUAGE=en_US` for every test (see
"Translations"); a test added below it runs in the caller's language.
`install.sh` runs no tests - it only builds and installs - so run `make test`
before `make install`.

The same hand-maintained-list trap exists in CI. `packaging_metadata_test.sh`
compares both workflows' job names to an **exact set**, and `ci_success`
carries its own `needs`/`env`/loop; a job added to the workflow but not to all
of those runs without gating anything. The test asserts that wiring for
`upgrade_deb` and `upgrade_rpm`.

[`tests/removed_api_gates.sh`](tests/removed_api_gates.sh) is a grep-based gate
that fails the suite if deleted APIs return - the transaction/recovery engine,
tombstones/Forget, automatic partial repair, in-place Edit/Replace, pending-
transaction presentation roles, runtime coupling in the privileged inventory,
and stale transaction-storage prose in `README.md`, `src` and the install
scripts. It also holds five placement rules:
- the `smb://` parser stays out of core, the helper and the root library;
- the service-menu dialog does not regain its own parser;
- translation stays in the front ends: no `i18n` call, `KLocalized*` or `KI18n`
  in core, root, helper, boot or the package guard;
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
  change the shared function - never the two sides separately.
- Every KAuth mutation is `auth_admin` by design, with no passwordless tier:
  nothing is ever invoked unattended, since boot arming runs as root. The
  header of [`io.github.pakru.nasmount.actions`](io.github.pakru.nasmount.actions)
  also explains why `auth_admin_keep` is written as plain `auth_admin`
  (`kauth-policy-gen` cannot emit `_keep`). Adding an action means editing
  that file - and is upgrade rule 5.
- Requires Linux 6.8+ (`STATX_MNT_ID_UNIQUE`), Plasma 6 / KF6, `cifs-utils`.
