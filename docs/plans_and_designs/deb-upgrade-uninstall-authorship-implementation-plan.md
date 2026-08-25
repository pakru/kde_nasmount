# DEB upgrade, uninstall, and authorship — implementation plan

Three deliverables, in dependency order:

1. **Upgradable `.deb`.** `apt install ./nasmount-amd64-<new>.deb` over an
   installed nasmount must succeed and must preserve every managed share,
   credential, and runtime record.
2. **A correct uninstall contract.** One supported removal path with honest
   semantics, explicit `remove` vs `purge` behaviour, and messages that name
   the blocking state and the exact command that clears it.
3. **Correct authorship and copyright.** The published packages currently
   attribute the work to a person who does not exist.

The Fedora counterpart is
[the RPM plan](rpm-upgrade-uninstall-authorship-implementation-plan.md). It
shares this plan's golden-corpus test, `nasmount-uninstall` fixes, and
non-packaging authorship items — implement those once, not twice.

Scope decisions taken before writing this plan:

| Decision | Choice |
|---|---|
| Identity in package metadata | `Pavel Krutikhin <krutikhin92@gmail.com>` |
| `nasmount-boot.service` on upgrade | Restart after upgrade (debhelper default), after proving re-arming is safe |
| RPM parity | **Out of scope.** `.deb` only; the RPM keeps refusing upgrades and the README must say so |

Everything below refers to files under [`packaging/debian/`](../../packaging/debian/)
unless another path is given.

---

## 1. Why upgrades are blocked today, and what that implies

### 1.1 The single blocker

[`nasmount.preinst`](../../packaging/debian/nasmount.preinst) fails the
transaction outright:

```sh
    upgrade)
        echo "nasmount does not yet support in-place package upgrades." >&2
        echo "Run nasmount-uninstall, then install the new package." >&2
        exit 1
        ;;
```

Nothing else in the DEB refuses an upgrade.
[`nasmount.prerm.in`](../../packaging/debian/nasmount.prerm.in) runs
`nasmount-package-guard` only for `$1 = remove`, and
[`nasmount.postrm`](../../packaging/debian/nasmount.postrm) is a bare
`#DEBHELPER#`.

### 1.2 The consequence that makes this cheap

Debian Policy §6.5 orders an upgrade as: **old** `prerm upgrade <new>` →
**new** `preinst upgrade <old>` → unpack → **old** `postrm upgrade <new>` →
**new** `postinst configure <old>`.

The script that refuses is the **new** package's `preinst`. The already-installed
package contributes only its `prerm upgrade` and `postrm upgrade`, and neither
blocks in any published release. Verified against every shipped tag:

```console
$ for t in v0.1.0 v0.1.1 v0.1.2; do git show "$t:packaging/debian/nasmount.prerm.in"; done
# all three: guard runs under `remove)` only
```

**Therefore no bridge release is required.** The first package that ships a
permissive `preinst` is directly installable over 0.1.0, 0.1.1, and 0.1.2. This
must be stated in the plan of record because the natural assumption — that the
*installed* package's scripts decide — is wrong here and would otherwise
produce an unnecessary two-step release.

### 1.3 On-disk compatibility is currently free, and must be kept that way

An upgrade preserves state that this project's own binaries must still accept:

| State | Location | Owner | Survives upgrade? |
|---|---|---|---|
| Unit pairs (`.mount` / `.automount`) | `/etc/systemd/system` | helper, at runtime | yes — not dpkg-owned |
| Credentials | `/etc/nasmount` | helper, root 0700 | yes |
| Automount instance ids | `/run/nasmount-ids` | `nasmount-boot` | yes (tmpfs, cleared only at reboot) |
| Per-user share list | `~/.config/nasmountrc` | dialog / KCM | yes |
| Program files, unit file, manifests | `/usr` | dpkg | replaced |

`Verify::inspectDefinition()` re-derives every property of an existing share
from the unit marker and validates the unit body against the same fixed-value
functions that generated it. **Any change to marker keys or generated unit body
turns every pre-existing share into `Tampered` after an upgrade** — silently
unarming the user's shares at the next boot.

Today the risk is zero and unproven: `git diff v0.1.0 HEAD -- src/core` is
empty, so all released versions generate byte-identical units. §7.3 adds the
gate that keeps it that way. `UnitValue::parseMarker()` additionally rejects
*any* unrecognised `# X-Nasmount-…` line, so adding a marker key is a
**breaking** change in both directions, not an additive one.

---

## 2. The upgrade contract

Written into `AGENTS.md` and enforced by §7:

1. An upgrade **never** runs `nasmount-package-guard`. The guard exists to stop
   program files disappearing out from under live state; an upgrade replaces
   them in place.
2. An upgrade **never** removes, rewrites, or migrates `/etc/nasmount`,
   `/run/nasmount*`, `/etc/systemd/system/*.mount|.automount`, or any user's
   `nasmountrc`.
3. Upgrades are supported from **0.1.0-1** upward. Older-to-newer only;
   downgrades are refused (§3.2).
4. The generated unit body and the marker schema are a **frozen on-disk format**.
   Changing either requires a version-gated migration designed as its own
   change, plus a bump of the minimum upgradable version in §3.2.
5. A maintainer script still never launches a graphical cache refresh as root,
   and never invokes KAuth.

---

## 3. Phase A — make the DEB upgradable

### 3.1 Template the `preinst`

`preinst` needs the version being installed in order to reject downgrades, and
debhelper does not substitute one. Reuse the mechanism already used for the
`prerm`: rename to **`nasmount.preinst.in`** and substitute at build time.

Add to [`packaging/build-deb.sh`](../../packaging/build-deb.sh), beside the
existing `nasmount.prerm.in` substitution:

```bash
sed -e "s/@VERSION@/$version/g" -e "s/@RELEASE@/$release/g" \
    "$source_dir/packaging/debian/nasmount.preinst.in" > "$source_dir/debian/nasmount.preinst"
```

and keep `nasmount.preinst` in the `chmod 0755` list (the generated path is the
same name, so no other change is needed there).

### 3.2 New `nasmount.preinst.in`

```sh
#!/bin/sh
# Upgrades are supported in place; downgrades and pre-contract versions are not.
set -e

NEW_VERSION="@VERSION@-@RELEASE@"
# Oldest installed version whose on-disk unit/marker format this package still
# reads unchanged (plan §1.3). Raise this only together with a migration.
MIN_UPGRADABLE_VERSION="0.1.0-1"

case "$1" in
    install|abort-upgrade)
        ;;
    upgrade)
        # $2 is the version being replaced. Equal versions occur on
        # `apt install --reinstall` and must be allowed.
        if dpkg --compare-versions "$2" gt "$NEW_VERSION"; then
            echo "ERROR: nasmount does not support downgrades ($2 -> $NEW_VERSION)." >&2
            echo "Run nasmount-uninstall, then install the older package." >&2
            exit 1
        fi
        if dpkg --compare-versions "$2" lt "$MIN_UPGRADABLE_VERSION"; then
            echo "ERROR: nasmount cannot upgrade in place from $2." >&2
            echo "Run nasmount-uninstall, then install this package." >&2
            exit 1
        fi
        ;;
esac

#DEBHELPER#
exit 0
```

Downgrades are refused rather than allowed because an older binary set has no
knowledge of a newer on-disk format and would classify unknown state as
`Tampered` — the failure mode is a user's shares silently not arming, which is
worse than an explicit refusal with a working escape hatch.

### 3.3 `prerm` and `postrm`

`nasmount.prerm.in` keeps the guard under `remove)` only — that is already
correct and is what makes §1.2 true. Add a comment recording *why* `upgrade` is
deliberately absent, so it is not "helpfully" added later:

```sh
case "$1" in
    remove)
        # Upgrade is intentionally absent: an upgrade replaces program files in
        # place and never orphans live state, so the guard must not run here
        # (plan §2.1). Adding `upgrade)` would make every upgrade fail on any
        # host that has a share.
        /usr/lib/@DEB_HOST_MULTIARCH@/libexec/nasmount-package-guard
        ;;
esac
```

`postrm` gains explicit cases in Phase C (§5.2).

### 3.4 Service handling across the upgrade

`nasmount-boot.service` is `Type=oneshot` / `RemainAfterExit=yes` and only arms
autofs triggers; the `.automount` units it starts are **started, not enabled**,
so nothing but this unit re-arms them at boot.

Adopt debhelper's default `--restart-after-upgrade`, making it explicit in
[`debian/rules`](../../packaging/debian/rules) rather than inherited:

```make
override_dh_installsystemd:
	dh_installsystemd --restart-after-upgrade nasmount-boot.service
```

Re-running `nasmount-boot` against existing shares is non-destructive by
construction, and the reason should be cited in the rules comment. Every branch
of [`evaluateArmPrecheck()`](../../src/root/arming.cpp#L98) returns before the
mount point, the credential, or systemd is touched, and each existing share
takes one of three of them — **which one depends on whether the share is
currently mounted, and the order matters**:

| Post-upgrade share state | Precheck result | Effect |
|---|---|---|
| Automount active, **share currently mounted** | `Blocked` — the `MountState::Present` test is evaluated *first*, before the automount/trust test | logged and skipped per-share; nothing touched |
| Automount active, idle, `/run/nasmount-ids` record matches | `AlreadyArmed` | no-op |
| Automount active, id record missing or mismatched | `Blocked` (`refusing to bless it`) | logged and skipped; stays armed |

The `startedByUs` compensation path in
[`armShare()`](../../src/root/arming.cpp#L119) is reachable only for a share
this very invocation started, and `startedByUs` is still `false` on every
precheck return, so a restart can never stop a live automount it did not
create.

The first row is the common case on a desktop with a share in use, and it means
**a post-upgrade journal will legitimately contain `a live mount already
occupies the path` for every mounted share**. §11 step 3 must expect those
lines rather than treat them as a failure.

Two further properties to record:

- debhelper's snippet ends the invoke with `|| true`, so a failing restart
  cannot leave the package half-configured.
- The snippet runs `systemctl --system daemon-reload` first, which is required
  because the upgrade replaced `/usr/lib/systemd/system/nasmount-boot.service`.

This reasoning is a code reading, not a measurement. §8 makes the VM check
mandatory before release.

### 3.5 Front ends and version skew during the upgrade

The KCM `.so` is replaced while System Settings may have it mapped, and a
running `nasmount-dialog` may call a freshly replaced KAuth helper. Both are
tolerable — the actions file and helper action names are unchanged by an
upgrade unless deliberately changed — but the rule belongs in `AGENTS.md`:
**adding, renaming, or removing a KAuth action in
[`io.github.pakru.nasmount.actions`](../../io.github.pakru.nasmount.actions) is
an upgrade-compatibility change**, because a running old front end will call the
new helper.

No maintainer script may refresh KDE caches (existing constraint). The README
gains one line telling users to restart System Settings and Dolphin after an
upgrade; the existing `nasmount.triggers` `interest-noawait` on
`/usr/share/kio/servicemenus` is a no-op because `postinst` has no `triggered`
case — either drop the file or leave it with a comment saying it is inert
(recommended: drop it; it produces a dpkg trigger cycle cost for nothing).

### 3.6 File-set changes across an upgrade

`usr/*` in [`nasmount.install`](../../packaging/debian/nasmount.install) means
dpkg removes files that disappear between versions automatically. The
regenerated `/usr/share/nasmount/cleanup-manifest.txt` and the compiled-in
allowlist in
[`cleanupvalidation.cpp`](../../src/session/cleanupvalidation.cpp) are replaced
as one unit, so they cannot disagree after an upgrade. No action beyond the
existing AGENTS.md synchronisation rule.

---

## 4. Phase A deliverables

| File | Change |
|---|---|
| `packaging/debian/nasmount.preinst` | delete |
| `packaging/debian/nasmount.preinst.in` | new, §3.2 |
| `packaging/debian/nasmount.prerm.in` | comment only, §3.3 |
| `packaging/debian/rules` | `override_dh_installsystemd`, §3.4 |
| `packaging/debian/nasmount.triggers` | delete, §3.5 |
| `packaging/build-deb.sh` | substitute `nasmount.preinst.in`, §3.1 |

---

## 5. Phase B — a correct uninstall contract

### 5.1 What is wrong today

1. The README calls `nasmount-uninstall` "an authenticated full purge … then
   the software itself", but
   [`nasmount-uninstall.sh`](../../packaging/nasmount-uninstall.sh) runs
   `apt-get remove`, which leaves the package in dpkg's `rc` (config-files)
   state. `dpkg -l` keeps listing nasmount after a "full purge".
2. `postrm` handles no cases at all, so `remove`, `purge`, `abort-install`, and
   `disappear` are indistinguishable and undocumented.
3. There is no non-interactive mode, so nothing — including CI — can exercise
   the real uninstall path end to end.
4. On partial failure (cleanup succeeded, `apt-get` failed) the user is left in
   a state the script never names: shares purged, software installed.

> **Superseded on 2026-08-23, after implementation.** §5.2 and §5.4 below
> described a `postrm` that never deletes anything and a `prerm` guard that
> refuses removal outright. The maintainer chose the opposite contract for the
> DEB: `apt remove` now tears managed shares down itself (including unmounting
> live mounts), and `apt purge` additionally deletes their unit files,
> `/etc/nasmount`, and `/run/nasmount*`. The RPM keeps the guard unchanged.
> What shipped is recorded in [`AGENTS.md`](../../AGENTS.md) under "Native
> removal has a strict order"; §5.2 and §5.4 are kept for the reasoning they
> record about why the guard existed, not as a description of current
> behaviour. Two consequences accepted with the decision: `apt remove`
> destroys managed state with no authentication and no owner check, and it
> cannot clear any user's `~/.config/nasmountrc`.
>
> The rest of §5 — preflight before cleanup, honest exit codes, `--yes`,
> `purge` over `remove` — shipped as written.

### 5.2 `postrm` with explicit cases

```sh
#!/bin/sh
set -e

case "$1" in
    remove|upgrade|failed-upgrade|abort-install|abort-upgrade|disappear)
        ;;
    purge)
        # Removal is permitted only from provably empty root state
        # (nasmount-package-guard, run by prerm), so there is nothing left to
        # purge here. Credentials under /etc/nasmount are never deleted by a
        # maintainer script: if any exist at this point the guard was bypassed,
        # and silently destroying root-owned credentials would be worse than
        # leaving them for the administrator.
        if [ -e /etc/nasmount ]; then
            echo "NOTE: /etc/nasmount still exists and was left in place." >&2
            echo "Remove it by hand once you are sure no share needs it." >&2
        fi
        ;;
    *)
        echo "postrm called with unknown argument '$1'" >&2
        exit 1
        ;;
esac

#DEBHELPER#
exit 0
```

The `*)` arm is the standard Debian maintainer-script idiom (Policy §6.1: a
script called with an argument it does not understand must fail loudly rather
than silently succeed), and it is what makes the four benign cases above a
deliberate no-op instead of an accident.

### 5.3 `nasmount-uninstall` changes

In [`packaging/nasmount-uninstall.sh`](../../packaging/nasmount-uninstall.sh):

- `package_manager_remove_arguments deb` becomes `purge -y nasmount`, so the
  command matches the promise the README already makes. The RPM arm keeps
  `remove --no-autoremove nasmount` unchanged — DNF's autoremove hazard
  documented in AGENTS.md still applies, and `--no-autoremove` stays mandatory.
  `-y` is correct because the script has already taken its own explicit
  confirmation; a second prompt for the same decision is noise.
- Add `--yes` / `-y` to skip the interactive confirmation, and `--help`, for
  scripted administrative use. Note what this does **not** buy: the wrapper
  still cannot run in CI. It refuses to run as root, and its first real step is
  an authenticated KAuth call, so exercising it needs a non-root desktop
  session with system D-Bus, polkit, and the helper — i.e. the VM checklist in
  §11, not a container.
- Move every non-mutating preflight **before** `nasmount-cleanup` runs. Today
  `remove_native_package()` validates `/usr/bin/sudo` and the package-manager
  binary *after* cleanup has already purged the user's shares, so a host
  missing `sudo` destroys state and then fails. Manager selection, executable
  checks, and the `sudo` check must all happen while nothing has changed yet.
- Report the terminal state explicitly. Distinct exit codes, printed with the
  matching sentence:

  | Code | Meaning |
  |---|---|
  | 0 | shares purged and package removed |
  | 1 | usage or preflight error — **nothing was attempted**, guaranteed because all such checks now precede cleanup (above) |
  | 2 | cleanup did not confirm success; **state may be unchanged, partly removed, or fully removed**. Package not removed |
  | 3 | **cleanup confirmed, package removal failed** — state is purged, software still installed, rerun to finish |

  Code 2 must **not** claim that nothing was removed, and this is the one place
  the plan must not simplify. Three separate mechanisms make a failed cleanup
  compatible with destroyed state:
  [`Root::Operations::purge()`](../../src/root/operations.cpp#L411) deletes
  shares in a loop, incrementing `removedShares` and returning early on the
  first failure — earlier shares' units, credentials, and `/run` id records are
  already gone; [`nasmount-cleanup`](../../src/cleanup/main.cpp#L67) returns the
  same failure code for `privileged data was purged, but user data cleanup
  failed`, which by its own wording means the purge *succeeded*; and
  `HelperOutcome::Unknown` is documented in
  [`helperinvoke.h`](../../src/session/helperinvoke.h#L30) as "dispatch may have
  happened; the acknowledgement was lost". Only `ConfirmedFailure` proves the
  helper refused before dispatch, and the wrapper cannot see that distinction
  through `nasmount-cleanup`'s single exit code.

  Consequence for the implementation: either widen `nasmount-cleanup` to report
  confirmed-refusal separately from indeterminate failure (preferred, and a
  small change in [`main.cpp`](../../src/cleanup/main.cpp#L62)), or have code 2
  print the honest superset above. Do not print "nothing was removed".
- Keep the ordering invariant (`nasmount-cleanup` strictly before the package
  manager) and the existing test that proves it by line number.

### 5.4 Direct `apt-get remove` — superseded

This section originally read "remains a supported, guarded path: the guard
refuses unless root state is provably empty". That is no longer true for the
DEB; see the note at the head of §5. One point from it still stands and must
not be lost: do **not** give the DEB path a `--no-autoremove` equivalent. That
requirement is Fedora-specific — it exists because DNF continues removing
dependencies after a refused `%preun` — and stating it for apt would be wrong.

The guard binary is still installed on both families and is still authoritative
for the RPM. It must not reappear in the DEB `prerm`, where it would refuse the
removal the new design performs;
[`packaging_metadata_test.sh`](../../tests/packaging_metadata_test.sh) asserts
its absence there.

### 5.5 Phase B deliverables

| File | Change |
|---|---|
| `packaging/debian/nasmount.postrm` | explicit cases, §5.2 |
| `packaging/nasmount-uninstall.sh` | purge, `--yes`, `--help`, **preflight before cleanup**, honest exit codes, §5.3 |
| `src/cleanup/main.cpp` | optional but preferred: distinguish confirmed refusal from indeterminate failure so the wrapper can map exit 2 precisely, §5.3 |

---

## 6. Phase C — authorship and copyright

### 6.1 The facts

Confirmed against `https://api.github.com/users/pakru` and the repository:

| Field | Value |
|---|---|
| Name | Pavel Krutikhin |
| GitHub | `pakru` (id 20311868) |
| Email | `krutikhin92@gmail.com` — matches every commit author in this repo |
| Repository created | 2026-08-15 |
| Licence | GPL-3.0-or-later ([`LICENSE`](../../LICENSE)) |

The packages currently ship `Pavel Krupets <pakru@users.noreply.github.com>`.
The name is wrong, and the address is a malformed GitHub noreply alias (the
`<id>+` prefix is missing), so it does not route.

Copyright year is **2026**, not `2025-2026`: the repository's first commit and
GitHub creation are both 2026-08-15.

### 6.2 Changes

**`packaging/debian/control`**

```
Maintainer: Pavel Krutikhin <krutikhin92@gmail.com>
```

Also add, since the DEP-5 and control files are being touched anyway:

```
Vcs-Browser: https://github.com/pakru/kde_mount
Vcs-Git: https://github.com/pakru/kde_mount.git
```

**`packaging/debian/copyright`** — replace with a complete DEP-5 file. The
current `License:` paragraph is a bare pointer, which is not a valid licence
stanza; the GPL requires the standard grant text plus the
`/usr/share/common-licenses` reference:

```
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: nasmount
Upstream-Contact: Pavel Krutikhin <krutikhin92@gmail.com>
Source: https://github.com/pakru/kde_mount

Files: *
Copyright: 2026 Pavel Krutikhin
License: GPL-3.0-or-later

License: GPL-3.0-or-later
 This program is free software: you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the Free
 Software Foundation, either version 3 of the License, or (at your option)
 any later version.
 .
 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 more details.
 .
 You should have received a copy of the GNU General Public License along
 with this program. If not, see <https://www.gnu.org/licenses/>.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 3 can be found in "/usr/share/common-licenses/GPL-3".
```

**`packaging/debian/changelog.in`** — maintainer line, and an entry that says
what the release does rather than that it was built:

```
nasmount (@VERSION@-@RELEASE@) resolute; urgency=medium

  * Upstream release @VERSION@ for Ubuntu 26.04 LTS.

 -- Pavel Krutikhin <krutikhin92@gmail.com>  @DATE@
```

**`packaging/rpm/nasmount.spec.in`** — the `%changelog` author line only. The
RPM's upgrade behaviour is explicitly untouched in this plan (§9).

**`src/kcm/kcm_nasmount.json`** — the KCM's authorship is user-visible in System
Settings and is currently absent:

```json
"Authors": [{ "Name": "Pavel Krutikhin", "Email": "krutikhin92@gmail.com" }],
"License": "GPL-3.0-or-later",
"Website": "https://github.com/pakru/kde_mount"
```

Note `KPlugin.Version` is injected by CMake and gated by
[`version_metadata_test.cmake`](../../tests/version_metadata_test.cmake) — do
not hand-write a `Version` key here.

**`README.md`** — an `## Author and licence` section naming Pavel Krutikhin,
the repository, and GPL-3.0-or-later.

### 6.3 Optional, separable: source-file copyright headers

Every file under `src/` carries `SPDX-License-Identifier: GPL-3.0-or-later` but
no copyright holder. Adding the companion line

```
 * SPDX-FileCopyrightText: 2026 Pavel Krutikhin
```

directly above it in all ~50 files is mechanical and matches the SPDX
convention the project already follows. Keep it as its **own commit** — it
touches every source file and would otherwise bury the packaging diff.

---

## 7. Tests and CI

This is the largest part of the work, and the part most likely to be
under-estimated: `tests/packaging_metadata_test.sh` compares the CI job list to
an **exact set**, so adding a job without updating that list fails the suite.

### 7.1 Static gates — `tests/packaging_metadata_test.sh`

- The `sh -n` loop currently names `nasmount.preinst` literally. Change to
  `nasmount.preinst.in` (as with `prerm.in`, `@VERSION@` is an ordinary word to
  `sh -n`).
- New assertions:
  - `nasmount.preinst.in` contains `dpkg --compare-versions` and a
    `MIN_UPGRADABLE_VERSION`, and does **not** contain
    `does not yet support in-place package upgrades`.
  - `nasmount.prerm.in` does **not** contain an `upgrade)` case — the direct
    encoding of contract rule §2.1.
  - `nasmount.postrm` contains a `purge)` case and the unknown-argument `*)`
    arm.
  - `debian/rules` contains `dh_installsystemd --restart-after-upgrade`.
  - Identity gates: `control`, `copyright`, and `changelog.in` all contain
    `Pavel Krutikhin <krutikhin92@gmail.com>`, and no file under `packaging/`
    contains `Krupets` or the malformed `pakru@users.noreply.github.com`.
- Update `expected_ci` to include the new job from §7.2. **This alone does not
  make the job blocking** — see §7.5.

### 7.2 A real upgrade job — `upgrade_deb`

Add to [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml), needing
`build_deb`, running in the `ubuntu:26.04` container.

The job must replicate `build_deb`'s environment setup, because
[`build-deb.sh`](../../packaging/build-deb.sh) enforces four preconditions that
a bare container fails: it refuses to run as root (line 8), requires
`ID=ubuntu`/`VERSION_ID=26.04` and amd64 (line 23), and requires an **empty**
output directory (line 33). So: `apt-get install` the same build-dependency
list, `useradd builder`, `chown -R builder`, and invoke through `runuser -u
builder`, with a **separate empty output directory per build**. The job is
therefore not network-free — it installs the full build stack like `build_deb`
does; the point of the `RELEASE` bump is that it needs no *GitHub release
asset*, not that it needs no network.

1. Download the built `.deb` (version *N-1*).
2. Rebuild the same tree with `packaging/RELEASE` bumped to `2`, producing
   *N-2* — a genuine dpkg upgrade with no dependency on a published asset and
   no synthetic version arithmetic on `VERSION`.
3. `apt-get install ./nasmount_<N>-1_amd64.deb`.
4. Seed state the guard would refuse to remove: `install -d -m 0700
   /etc/nasmount` plus a marker-bearing `.mount`/`.automount` pair copied from
   the golden corpus (§7.3) into `/etc/systemd/system`.
5. `apt-get install ./nasmount_<N>-2_amd64.deb` — **must succeed**.
6. Assert: `dpkg-query -W -f='${Version}'` is `<N>-2`; `/etc/nasmount` still
   exists with mode `0700`; both seeded unit files are byte-identical to the
   corpus; and the guard still classifies the host as **managed**. Invoke it by
   its installed path — `/usr/lib/x86_64-linux-gnu/libexec/nasmount-package-guard`,
   derived from `dpkg-architecture -qDEB_HOST_MULTIARCH`, since
   [CMake installs it outside `PATH`](../../CMakeLists.txt#L183). Assert **exit
   status 1**, not merely non-zero: exit 2 is `Unsafe`/`Indeterminate`, which is
   what a corrupted golden corpus would produce, and accepting any non-zero
   status would let that pass as a successful test.

   Note the state seeded in step 4 is deliberately redundant.
   [`PackageState::classify()`](../../src/core/packagestate.cpp#L97) returns
   `Managed` on `/etc/nasmount` alone, so the seeded units are what actually
   exercise marker parsing — which is the half that can regress.
7. Assert the downgrade rule: `dpkg -i` of the *N-1* package now **fails**, and
   the package remains at *N-2*.
8. Clean up seeded state, then `apt-get purge -y nasmount` and assert the
   package is fully gone (`dpkg -l` shows no `rc` entry). This tests the
   `postrm purge` case and dpkg's end state — **not** `nasmount-uninstall`,
   which cannot run here (§5.3). Do not describe this step as covering the
   uninstall wrapper.

A second, optional job may install the newest published GitHub release asset and
upgrade HEAD over it. It is the only test that catches a break against bytes
users actually have, but it depends on network and on a release existing, so it
must be allowed to skip rather than fail when the asset is absent.

### 7.3 Freeze the on-disk format — golden unit corpus

The highest-severity upgrade failure (§1.3) has no test at all. Add:

- `tests/golden/units/v0.1.0/` — a real `.mount` + `.automount` pair as
  generated by 0.1.0, plus the fixture inputs (marker, UNC, mount point,
  uid/gid) that produced them.
- `tests/goldenunits_test.cpp` — a plain `main()` in the existing harness style
  that asserts, for each frozen corpus version:
  1. `UnitValue::parseMarker()` accepts the marker;
  2. `UnitSpec::validateMountUnitBody()` and `validateAutomountUnitBody()`
     accept the bodies;
  3. `buildMountUnitContent()` / `buildAutomountUnitContent()` re-generate the
     corpus files **byte for byte** from the recorded inputs.

Assertion 3 is what actually protects users: it fails the build the moment
generation drifts, at which point the change author must either revert or
design a migration and raise `MIN_UPGRADABLE_VERSION`.

Per AGENTS.md, a new test means editing three places: the test source,
`CMakeLists.txt` (`add_executable` + `target_link_libraries nasmount-core` +
`add_test`), and the explicit test-binary list in
[`install.sh`](../../install.sh#L44).

### 7.4 `tests/package_scripts_test.sh`

- Assert `package_manager_remove_arguments deb` now yields `purge -y nasmount`,
  and that the rpm arm is unchanged.
- Assert `--yes` is accepted and that the cleanup-before-removal line ordering
  check still holds.
- Keep the existing "source installs select no package manager" gates.

### 7.5 Making the new job actually blocking

Adding a job and listing it in `expected_ci` is **not** enough for it to gate
anything. Both workflows gate on explicit, hand-maintained dependency lists:

- [`ci_success`](../../.github/workflows/ci.yml#L228) declares
  `needs: [validate_packaging, build_deb, build_rpm, smoke_packages,
  verify_artifact_set]`, maps each to an `env:` var, and loops over exactly
  those five values. `upgrade_deb` must be added in **all three** places —
  `needs`, `env`, and the loop — or an upgrade failure leaves the required
  check green.
- [`verify_release_set`](../../.github/workflows/release.yml#L231) declares
  `needs: [validate_release, smoke_release_packages]`, and `attest_and_publish`
  gates on it. If the release workflow gets an upgrade job, it must be added to
  that `needs` list, otherwise a broken upgrade can still be published.

Then update `expected_release` to match, so the two exact-set comparisons in
`tests/packaging_metadata_test.sh` stay in sync with reality.

---

## 8. Documentation

- **`README.md`**
  - New `## Upgrade` section: `sudo apt install ./nasmount-amd64-<v>.deb` over
    an existing install; shares, credentials and configuration are preserved;
    downgrades are refused; restart System Settings and Dolphin afterwards to
    pick up the new KCM and service menu.
  - State plainly that **the RPM does not yet support in-place upgrades** and
    that Fedora users must run `nasmount-uninstall` then install (§9).
  - Correct the Uninstall section: `nasmount-uninstall` now *purges* the DEB.
  - New `## Author and licence` section.
  - The example commands still say `0.1.1`; refresh to the current release.
- **`AGENTS.md`**
  - New "Upgrades" paragraph carrying the §2 contract, especially rules 1, 4
    and the KAuth-action rule from §3.5.
  - Fix two stale statements while here: "The repo has **no commits** yet" is
    false, and the design document link points at `docs/credential-modes-design.md`
    whereas the file lives in `docs/plans_and_designs/`.
- Note for a separate cleanup, not this plan: `install.sh` filters CTest with
  `-R '^(appstreamtest|version_metadata)$'`, but no `appstreamtest` target
  exists — the pattern silently matches only `version_metadata`.

---

## 9. Explicitly out of scope

- **RPM upgrade support**, now covered by its own
  [RPM plan](rpm-upgrade-uninstall-authorship-implementation-plan.md). Until
  that lands, `%pre` keeps `if [ "$1" -gt 1 ]` refusing upgrades and the README
  must not imply DEB/RPM parity for upgrades.

  The two are **not symmetric**, so do not assume this plan's mechanisms port.
  RPM's `%pre` receives only an instance count and never the version being
  replaced, so it has no equivalent of §3.2's `MIN_UPGRADABLE_VERSION` gate —
  and, unlike what a first reading suggests, **no native downgrade refusal to
  fall back on either**: DNF5 downgrades on an exact version spec, which a local
  `.rpm` is. Conversely the RPM needs no `--restart-after-upgrade` equivalent:
  `%systemd_postun_with_restart` in the already-shipped 0.1.2 spec marks the
  unit and systemd's own file triggers restart it. See the RPM plan §3.2 and
  §3.3.

  If §3.2's version gate ever has to become a real compatibility gate, the RPM
  plan proposes a better mechanism for **both** families: gate on an on-disk
  format-version marker under `/etc/nasmount` rather than on the package
  version. Prefer that over extending this plan's `dpkg --compare-versions`
  approach.
- Multi-user cleanup. `nasmount-uninstall` stays owner-scoped.
- Any change to the marker schema, unit body, or KAuth action set.
- Conffiles. The package ships nothing under `/etc`, and §5.2 depends on that
  remaining true.

---

## 10. Sequencing

| Step | Content | Gate |
|---|---|---|
| 1 | §7.3 golden corpus + test | Green on the unchanged tree — proves the baseline before anything moves |
| 2 | §6 authorship and copyright | `lintian --fail-on error`; identity gates in §7.1 |
| 3 | §3/§4 upgrade support | §7.1 static gates; §7.2 `upgrade_deb` job |
| 4 | §5 uninstall contract | §7.4; the purge assertions in §7.2 step 8 |
| 5 | §8 documentation | Review |
| 6 | §6.3 source headers | Separate commit, mechanical |

Step 1 first is deliberate: the corpus must be captured from a tree that is
known-compatible with shipped releases. Capturing it after packaging changes
would freeze whatever the tree happens to do at that point rather than what
users already have on disk.

Release as **0.1.3** with `packaging/RELEASE` reset to `1`. Because §1.2 holds,
0.1.3 installs directly over 0.1.0, 0.1.1, and 0.1.2 with no intermediate step.

## 11. Privileged validation before publishing

Container CI cannot exercise KAuth, polkit, D-Bus, CIFS, or reboot. On a fresh
Kubuntu 26.04 VM, before the release is published:

1. Install 0.1.2 from the published asset. Add two shares — one credentials,
   one guest — through the KCM and through the Dolphin service menu.
2. Reboot; confirm both arm before login and mount on first access.
3. `sudo apt install ./nasmount-amd64-0.1.3.deb`. Confirm the upgrade succeeds,
   both shares remain mounted or armed, `/etc/nasmount` is intact, and
   `systemctl status nasmount-boot` shows the post-upgrade restart succeeding.
   In the journal, `a live mount already occupies the path` is **expected** for
   any share that was mounted at upgrade time (§3.4); what must be absent is
   `refusing to bless it` for an idle share, and any share that stops resolving
   afterwards. This is the step that turns §3.4's code reading into evidence.
4. Access both shares; add a third with the upgraded binaries.
5. Reboot again; confirm all three arm.
6. Confirm `apt-get remove nasmount` is refused and names the blocking path.
7. Run `nasmount-uninstall`; confirm exit 0, all shares and credentials gone,
   mount-point directories retained, and `dpkg -l nasmount` shows nothing —
   not an `rc` entry.
8. Repeat step 3 on a host with **no** shares, to cover the empty-state upgrade.
