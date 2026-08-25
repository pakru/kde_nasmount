# RPM upgrade, uninstall, and authorship — implementation plan

The Fedora 44 counterpart to
[the DEB plan](deb-upgrade-uninstall-authorship-implementation-plan.md), with
the same three deliverables:

1. **Upgradable `.rpm`.** `sudo dnf install ./nasmount-fedora44-x86_64-<new>.rpm`
   over an installed nasmount must succeed and must preserve every managed
   share, credential, and runtime record.
2. **A correct uninstall contract**, with honest failure reporting.
3. **Correct authorship and copyright**, replacing the non-existent
   "Pavel Krupets".

**Read the DEB plan first.** The two are not symmetric. RPM's scriptlet
ordering is different, its systemd integration does more work for us, and its
version-gating capability is strictly weaker. Sections below say explicitly
where the DEB reasoning transfers and where it does not.

Shared identity decision: **`Pavel Krutikhin <krutikhin92@gmail.com>`**.

---

## 1. Why upgrades are blocked today

### 1.1 The single blocker

[`nasmount.spec.in`](../../packaging/rpm/nasmount.spec.in) `%pre`:

```spec
%pre
if [ "$1" -gt 1 ]; then
    echo "nasmount does not yet support in-place package upgrades." >&2
    echo "Run nasmount-uninstall, then install the new package." >&2
    exit 1
fi
```

`$1` is the number of instances of the package that will exist after the
transaction: `1` on first install, `2` during an upgrade. A non-zero `%pre`
aborts the package install, so DNF fails the whole transaction.

Nothing else refuses. `%preun` guards the package guard with `[ "$1" -eq 0 ]`,
which is false (`$1 = 1`) during an upgrade, so the guard correctly does not
run.

### 1.2 RPM scriptlet order, and why no bridge release is needed

RPM's upgrade order is **not** dpkg's, and the difference matters when
reasoning about which package's scripts decide the outcome:

| Step | Scriptlet | `$1` | From which package |
|---|---|---|---|
| 1 | `%pretrans` | 2 | **new** |
| 2 | `%pre` | 2 | **new** ← the blocker |
| 3 | *new files installed* | | |
| 4 | `%post` | 2 | **new** |
| 5 | `%preun` | 1 | old |
| 6 | *old-only files removed* | | |
| 7 | `%postun` | 1 | old |
| 8 | `%posttrans`, then file triggers | | new / systemd |

Note step 4 precedes steps 5–7: **the new package's `%post` runs before the old
package's `%preun` and `%postun`.** This is the classic RPM trap — an old
`%postun` can undo what a new `%post` just did. nasmount is not currently
exposed to it (§3.3), but any future scriptlet must be written with this order
in mind.

The conclusion matches the DEB plan for a different reason: the deciding script
is the **new** package's `%pre`, and the old package contributes only `%preun`
and `%postun`, neither of which blocks in any published release. Verified:

```console
$ for t in v0.1.0 v0.1.1 v0.1.2; do git show "$t:packaging/rpm/nasmount.spec.in"; done
# all three: %preun guarded by [ "$1" -eq 0 ]
```

**No bridge release.** The first RPM that ships a permissive `%pre` installs
directly over 0.1.0, 0.1.1, and 0.1.2.

### 1.3 On-disk compatibility

Identical to the DEB plan §1.3 and **not repeated here**: the unit marker
schema and generated unit body are a frozen on-disk format; `parseMarker()`
rejects unknown `X-Nasmount-*` keys in both directions; `git diff v0.1.0 HEAD --
src/core` is empty, so all released versions are byte-compatible today.

The golden-unit corpus test (DEB plan §7.3) is **family-agnostic and shared**.
It is a prerequisite of this plan, not a duplicate of it. Whichever plan lands
first implements it.

---

## 2. The upgrade contract

Same five rules as the DEB plan §2, plus one RPM-specific addition:

6. **The systemd preset is never re-applied on upgrade.** `%systemd_post` is
   guarded by `[ $1 -eq 1 ]` (§3.3), so an administrator who ran `systemctl
   disable nasmount-boot.service` keeps it **disabled** across upgrades. Do not
   add an unguarded `preset` call. This rule covers *enablement* only: whether
   the unit also stays **stopped** is a separate and less favourable question,
   resolved in §3.3.

---

## 3. Phase A — make the RPM upgradable

### 3.1 The change

Delete the `%pre` section entirely. There is nothing for it to do: fresh
install needs no pre-step, and upgrade must proceed. Replace it with a spec
comment above `%post` recording the contract, so it is not reintroduced:

```spec
# No %pre. Upgrades are supported in place (plan §1.2): the new package's %pre
# is the only scriptlet that can refuse an upgrade, and refusing one would
# strand every user who already has a share. The removal guard belongs in
# %preun, where it is correctly gated on $1 -eq 0.
```

That single deletion is the whole of Phase A. Everything else in this section
documents behaviour that is **already correct** and must be verified rather
than changed — which is the main way this plan differs from the DEB one.

### 3.2 Downgrades: a structural gap, not an oversight

The DEB plan gates downgrades in `preinst` using `$2`, the version being
replaced. **RPM passes no such argument.** `%pre` receives only an instance
count, so a spec cannot compare versions the way a `preinst` can. Querying the
rpmdb from inside a scriptlet is not an acceptable substitute — the database is
mid-transaction and Fedora's packaging guidelines forbid it.

What actually protects users: **on Fedora 44, essentially nothing.**

- `rpm -U` does refuse an older package without `--oldpackage`.
- **`dnf install ./<older>.rpm` does not.** Fedora ships DNF5, whose `install`
  documentation states that when an exact version is given — which a local
  `.rpm` file is — "DNF will install the desired version, no matter which
  version of the package is already installed… it will automatically try to
  downgrade or upgrade to the given version"
  ([DNF5 install](https://dnf5.readthedocs.io/en/latest/commands/install.8.html)).
  Since the README tells users to install with `dnf install ./<file>.rpm`, the
  documented command is exactly the one that will silently downgrade.
- `dnf downgrade` is a further explicit override.

So the RPM has **no downgrade protection at any layer**, and this is a real
asymmetry with the DEB rather than a cosmetic one.

Consequences to write down rather than paper over:

- There is **no RPM equivalent of `MIN_UPGRADABLE_VERSION`**, and no native
  downgrade refusal to fall back on either. If the on-disk format ever changes,
  the DEB can refuse both a too-old upgrade and a downgrade; the RPM can refuse
  neither.
- The near-term mitigation is that the golden-corpus test (§1.3) stops the
  format from drifting at all, and any deliberate format change must ship a
  migration safe from *every* prior version — a strictly stronger requirement
  than the DEB faces.
- **`%triggerprein` is not the escape hatch.** A versioned trigger condition can
  select the transaction, but the scriptlet's `$1`/`$2` are package-instance
  counts, not version strings, and RPM specifies only `%pre`/`%pretrans` failure
  as preventing installation — trigger failure carries no such guarantee
  ([rpm-scriptlets(7)](https://rpm-software-management.github.io/rpm/man/rpm-scriptlets.7)).
  Do not present it as a hard blocker.
- **If a compatibility gate is ever genuinely needed, gate on on-disk state, not
  on package version.** Write a format-version marker under `/etc/nasmount` and
  have `%pre` (and the DEB `preinst`) read *that file* and refuse when it is
  newer than the incoming package understands. It needs no rpmdb query, works
  identically in both families, and is correct for the thing actually at
  risk — the state format — rather than for a package version that only
  correlates with it. Design it with the migration that needs it, not before.

### 3.3 Service handling — already correct, and worth understanding

Unlike the DEB, **the RPM needs no change to restart `nasmount-boot.service` on
upgrade; it already does.** The mechanism is indirect and worth recording,
because it is easy to "fix" by mistake.

Modern systemd RPM macros do not restart anything themselves. Verified against
**systemd v259.8**, the series Fedora 44 ships — not `main`, which has already
changed the restart verb (below):
[`macros.systemd.in`](https://github.com/systemd/systemd/blob/v259.8/src/rpm/macros.systemd.in)
and
[`systemd-update-helper.in`](https://github.com/systemd/systemd/blob/v259.8/src/rpm/systemd-update-helper.in):

| Spec macro | Guard | What it actually runs |
|---|---|---|
| `%systemd_post` | `$1 -eq 1` | `systemd-update-helper install-system-units` → `systemctl --no-reload preset` — **initial install only** |
| `%systemd_preun` | `$1 -eq 0` | `remove-system-units` → `systemctl disable --now` — **removal only** |
| `%systemd_postun_with_restart` | `$1 -ge 1` | `mark-restart-system-units` → `systemctl set-property <unit> Markers=+needs-restart` — **marks, does not restart** |

The restart itself is performed by file triggers that the **systemd package**
ships
([`triggers.systemd.in`](https://github.com/systemd/systemd/blob/v259.8/src/rpm/triggers.systemd.in)),
fired by any transaction that touched `/usr/lib/systemd/system/`:

```spec
%transfiletriggerpostun -P 1000100 ... → systemd-update-helper system-reload   → systemctl daemon-reload
%transfiletriggerpostun -P 10000   ... → systemd-update-helper system-restart  → systemctl reload-or-restart --marked
```

Higher `-P` runs earlier. Crucially, these are **not** all end-of-transaction:
the `-P 1000100` trigger's own source comment states it runs "after any new unit
files have been installed, **but before `%postun` scripts in packages get
executed**". So the real order is:

```text
daemon-reload  (systemd trigger, -P 1000100)
  → old %postun sets Markers=+needs-restart  (mark-restart-system-units)
    → reload-or-restart --marked  (systemd trigger, -P 10000)
```

The reload comes *first*, so no assumption about markers surviving a
`daemon-reload` is needed — an earlier draft of this plan had the first two
steps reversed and justified the wrong order with a marker-survival claim that
is simply not load-bearing.

Three things follow:

1. **The restart on a 0.1.2 → 0.1.3 upgrade is driven by 0.1.2's already-shipped
   `%postun`.** It works today; the only reason nobody has seen it is that
   `%pre` aborts first.
2. `%post`'s `systemctl start` is correctly guarded by `[ "$1" -eq 1 ]` and must
   stay that way. Do not add an upgrade branch — it would double up with
   `reload-or-restart --marked`.
3. The safety of re-running `nasmount-boot` against existing shares is
   **exactly** the DEB plan §3.4 analysis, which applies unchanged: every branch
   of [`evaluateArmPrecheck()`](../../src/root/arming.cpp#L98) returns before
   touching the mount point, credential, or systemd, and `startedByUs` is
   `false` on every one of them. Refer to that table rather than restating it;
   in particular a **currently mounted** share returns `Blocked` (`a live mount
   already occupies the path`), not `AlreadyArmed`, and those journal lines are
   expected after an upgrade.

#### An upgrade probably restarts a service the administrator stopped

This has no DEB analogue and it is not an edge case that resolves in our favour.
`systemctl --marked` is documented as enqueuing **`restart`** jobs for
`needs-restart` units, and plain `reload-or-restart` "If the units are not
running yet, they will be started" — unlike `try-reload-or-restart`, which "does
nothing if the units are not running"
([systemctl(1), v259](https://www.freedesktop.org/software/systemd/man/259/systemctl.html)).

So on a host where an administrator ran `systemctl disable --now
nasmount-boot.service`, an upgrade is expected to:

- leave it **disabled** — the preset is not re-applied (§2 rule 6); but
- **start it anyway**, re-arming every share the admin had deliberately left
  unarmed.

Two open questions decide whether this needs handling, and both are VM
questions, not reading questions (§8 step 6):

1. Does `systemctl set-property` succeed at all on a stopped unit? `Markers` is
   a runtime property, and if the unit is not loaded the mark may simply fail
   under the macro's `|| :`, in which case the problem disappears.
2. If it is marked and started, is that acceptable? Arming shares on a host
   whose administrator disabled the arming service is a real behaviour change,
   even if a benign one.

If it turns out to need handling, the fix is **not** to fight the trigger:
replace `%systemd_postun_with_restart` with a `%posttrans` that restarts only
when the unit was active before the transaction. Do not decide that until the
VM answers question 1.

### 3.4 Front ends and version skew

Identical to the DEB plan §3.5: the KCM `.so` is replaced under a running
System Settings, a running `nasmount-dialog` may call a freshly replaced helper,
and **changing the KAuth action set in
[`io.github.pakru.nasmount.actions`](../../io.github.pakru.nasmount.actions) is
an upgrade-compatibility change**. No scriptlet may refresh KDE caches or
invoke KAuth. The README instruction to restart System Settings and Dolphin is
shared.

### 3.5 File-set changes across an upgrade

The RPM lists every installed path explicitly in `%files`, so unlike the DEB's
`usr/*` glob, **a file added or removed in CMake and not mirrored in `%files`
fails the build** — a useful property, and one the existing AGENTS.md
synchronisation rule already covers. RPM removes old-only files at step 6 of
§1.2, after the new files are in place, so a renamed file cannot be deleted
after being reinstalled.

Keep `%undefine __brp_linkdupes`: the cleanup manifest requires one independent
regular file per installed path, and hardlinking duplicates would break both the
manifest and per-file replacement on upgrade.

---

## 4. Phase A deliverables

| File | Change |
|---|---|
| `packaging/rpm/nasmount.spec.in` | delete `%pre`; add the contract comment, §3.1 |

**Superseded in part:** the spec also gained the erase-time teardown described
in the note at the head of §5, so this is no longer the entire functional
change. The upgrade half of it still is.

That was intended to be the entire functional change. The disproportion between this table and
the test work in §6 is the point: the risk is not in the edit, it is in proving
that everything §3.3 describes actually behaves as documented.

---

> **Superseded on 2026-08-23, after implementation.** §5 below described an
> RPM that keeps `nasmount-package-guard` in `%preun` and refuses removal while
> managed state exists. The maintainer chose the same relaxed contract the DEB
> received: an RPM erase now disarms every managed share and deletes its unit
> files, `/etc/nasmount`, and `/run/nasmount*` — RPM has no remove/purge split,
> so a single erase does what `apt remove` and `apt purge` do together.
> `%preun`/`%postun` are gated on `"$1" -eq 0`, without which every upgrade
> would unmount and delete every share. What shipped is recorded in
> [`AGENTS.md`](../../AGENTS.md). §5.2's point about `--no-autoremove` still
> stands and is unchanged; its claim that the rpm arm of `nasmount-uninstall`
> stays without `-y` was already corrected in §5.2 itself and shipped as
> `remove -y --no-autoremove nasmount`.
>
> Accepted with the decision: `dnf remove` destroys managed state with no
> authentication and no owner check, and cannot clear any user's
> `~/.config/nasmountrc`.

## 5. Phase B — the uninstall contract

### 5.1 What is and is not shared with the DEB

[`packaging/nasmount-uninstall.sh`](../../packaging/nasmount-uninstall.sh) is
**one shared file** driving both families. Its two defects are family-neutral
and are specified in full in the DEB plan §5.3:

- **Preflight runs after destruction.** `remove_native_package()` validates
  `/usr/bin/sudo` and the package-manager binary *after* `nasmount-cleanup` has
  already purged the user's shares. A Fedora host without `sudo` loses its
  shares and then fails. All preflight must move ahead of cleanup.
- **Exit codes lie.** "Cleanup failed" cannot be reported as "nothing was
  removed": [`Root::Operations::purge()`](../../src/root/operations.cpp#L411)
  deletes shares in a loop and returns early on first failure;
  [`cleanup/main.cpp`](../../src/cleanup/main.cpp#L67) returns the same code for
  `privileged data was purged, but user data cleanup failed`; and
  `HelperOutcome::Unknown` means dispatch may have happened.

**Whichever plan lands first owns those changes.** Do not implement them twice.

### 5.2 What is RPM-specific

- **No purge/`rc` state.** The DEB plan switches `apt-get remove` to
  `apt-get purge` because dpkg otherwise leaves the package in `rc` state. RPM
  has no such state: `dnf remove` is already complete, so the verb stays
  `remove`.
- **But the rpm arm does need `-y`.** It must become
  `remove -y --no-autoremove nasmount`, not stay as-is. The DEB plan adds `-y`
  on its side and a `--yes` wrapper mode; leaving the rpm arm without `-y` means
  that in `--yes` mode DNF still prompts on its own, and in a non-interactive
  context that prompt fails — **after** `nasmount-cleanup` has already destroyed
  the user's shares. That is the exit-3 state (§5.1) reached for no reason. The
  wrapper has already taken its own explicit confirmation, so the second prompt
  buys nothing. This breaks an exact assertion in
  [`package_scripts_test.sh`](../../tests/package_scripts_test.sh#L22), which
  must be updated in the same change (§7.1).
- **`--no-autoremove` remains mandatory**, for the reason AGENTS.md records:
  DNF can continue removing unused dependencies after RPM's `%preun` guard
  refuses the transaction, leaving nasmount installed without Qt. The existing
  smoke gate snapshots the full RPM set around a refused removal to enforce
  this, and `tests/packaging_metadata_test.sh` asserts the exact string count in
  both workflows (§6.1).
- **No `postrm purge` equivalent.** The DEB plan's `%`-equivalent work in its
  §5.2 has no counterpart here. `%postun` stays as it is:
  `%systemd_postun_with_restart` is correctly inert at `$1 -eq 0` (§3.3), and
  nothing else is needed because `%preun`'s guard already proved the root state
  empty.
- **`%preun` stays exactly as written**, including
  `%{_libdir}/libexec/nasmount-package-guard || exit $?`. The explicit
  `|| exit $?` preserves the guard's distinction between exit 1 (`Managed`) and
  exit 2 (`Unsafe`/`Indeterminate`), which a bare invocation under RPM's
  scriptlet shell would flatten.

### 5.3 Phase B deliverables

| File | Change |
|---|---|
| `packaging/nasmount-uninstall.sh` | shared with the DEB plan §5.3 — preflight ordering, honest exit codes, `--yes`, `--help`; **plus** `-y` on the rpm arm, §5.2 |
| `tests/package_scripts_test.sh` | update the exact rpm argument assertion, §7.1 |
| `src/cleanup/main.cpp` | shared, optional: distinguish confirmed refusal from indeterminate failure |
| `packaging/rpm/nasmount.spec.in` | none |

---

## 6. Phase C — authorship and copyright

### 6.1 RPM-specific

The `%changelog` entry has three defects, not one:

```spec
* Sun Aug 16 2026 Pavel Krupets <pakru@users.noreply.github.com> - @VERSION@-@RELEASE@
- Initial Fedora 44 package
```

1. Wrong name, and a malformed GitHub noreply address (missing the `20311868+`
   prefix, so it does not route).
2. **The date is frozen.** Unlike
   [`build-deb.sh`](../../packaging/build-deb.sh), which derives an RFC-email
   date from `SOURCE_DATE_EPOCH` or the HEAD commit,
   [`build-rpm.sh`](../../packaging/build-rpm.sh) substitutes only `@VERSION@`
   and `@RELEASE@`. Every future release will claim 2026-08-16. Add a
   `@CHANGELOG_DATE@` substitution using the same epoch logic as `build-deb.sh`,
   formatted, **with the locale and timezone pinned**, `date --date="@$epoch" '+%a %b %d %Y'`. (`Sun Aug 16 2026` is a
   genuine Sunday, so there is no rpmlint weekday error to fix — only the
   staleness.)

   The pinning is not optional: GNU `date` localises weekday and month names,
   and the local timezone can move an epoch onto the adjacent calendar day.
   Measured on a `ru_RU.UTF-8` host:

   ```console
   $ LC_ALL=ru_RU.UTF-8 date --date=@1755302400 '+%a %b %d %Y'
   Пт авг 15 2025          # RPM rejects this changelog header
   $ LC_ALL=C TZ=UTC0     date --date=@1755302400 '+%a %b %d %Y'
   Sat Aug 16 2025
   ```

   So:

   ```bash
   changelog_date=$(LC_ALL=C TZ=UTC0 date --date="@$build_epoch" '+%a %b %d %Y')
   ```

   `build-deb.sh` needs no equivalent change: `date --rfc-email` was verified to
   produce identical output under `ru_RU.UTF-8` and `C`, because RFC 5322
   mandates English abbreviations.
3. The version is `@VERSION@-@RELEASE@` while `Release:` is
   `@RELEASE@%{?dist}`, so the changelog reads `0.1.2-1` against a package
   release of `1.fc44`. rpmlint reports `incoherent-version-in-changelog`. Use
   `@VERSION@-@RELEASE@%{?dist}`.

Corrected:

```spec
%changelog
* @CHANGELOG_DATE@ Pavel Krutikhin <krutikhin92@gmail.com> - @VERSION@-@RELEASE@%{?dist}
- Upstream release @VERSION@ for Fedora 44
```

Note that CI runs `rpmlint` **without** a failure gate — unlike `lintian
--fail-on error` on the DEB side. Consider whether rpmlint should become
blocking once these are fixed; if not, say so deliberately rather than leaving
it ambiguous.

`URL:` and `License:` in the spec are already correct.

### 6.2 Shared with the DEB plan

`src/kcm/kcm_nasmount.json` (add `Authors`/`License`/`Website`), the
`## Author and licence` README section, and the optional
`SPDX-FileCopyrightText: 2026 Pavel Krutikhin` header pass over `src/` are all
**shared**. See the DEB plan §6.2–§6.3; implement once.

Copyright year is **2026** — the repository was created 2026-08-15.

---

## 7. Tests and CI

### 7.1 Static gates — `tests/packaging_metadata_test.sh`

This file gates both families, and the RPM assertions are more brittle than the
DEB ones. Two will break:

- **Exact-count assertion.** For *both* workflows:

  ```bash
  [ "$(grep -Fc 'dnf remove -y --no-autoremove nasmount' "$workflow")" -eq 2 ]
  ```

  A new `upgrade_rpm` job that removes the package makes the count 3 and fails
  the suite. Update the expected count deliberately — do not relax the check to
  `-ge`, because its purpose is to prove that *every* removal in the workflow
  carries `--no-autoremove`.
- **Negative assertion.** `grep -Fq 'dnf remove -y nasmount'` must find nothing.
  Any new removal line in the upgrade job must carry `--no-autoremove`.
- `expected_ci` / `expected_release` are exact set comparisons; add
  `upgrade_rpm`. **This alone does not make the job blocking** — see §7.4.

New assertions to add:

- `nasmount.spec.in` no longer contains `does not yet support in-place package
  upgrades`, and contains no `%pre` section.
- `%preun` still contains `%{_libdir}/libexec/nasmount-package-guard || exit $?`
  (already asserted — keep it).
- `%post` still guards its `systemctl start` with `[ "$1" -eq 1 ]`.
- In [`package_scripts_test.sh`](../../tests/package_scripts_test.sh#L22),
  `[ "${rpm_remove[*]}" = "remove --no-autoremove nasmount" ]` must become
  `"remove -y --no-autoremove nasmount"` (§5.2). The DEB arm assertion changes
  in the same file for the same reason.
- Identity: the spec contains `Pavel Krutikhin <krutikhin92@gmail.com>`, and
  nothing under `packaging/` contains `Krupets` or
  `pakru@users.noreply.github.com`.
- `build-rpm.sh` substitutes `@CHANGELOG_DATE@`.

### 7.2 A real upgrade job — `upgrade_rpm`

Add to [`ci.yml`](../../.github/workflows/ci.yml), needing `build_rpm`, in the
`fedora:44` container.

The job must replicate `build_rpm`'s setup, because
[`build-rpm.sh`](../../packaging/build-rpm.sh) enforces four preconditions a
bare container fails: it refuses to run as root (line 8), requires
`rpm --eval '%{fedora}'` to be `44` and `uname -m` to be `x86_64` (lines 22–29),
and requires an **empty** output directory (line 33). So: `dnf install` the same
build-dependency list, `useradd builder`, `chown -R builder`, invoke via
`runuser -u builder`, and use a **separate empty output directory per build**.
The job is not network-free; the point of the `RELEASE` bump is that it needs no
published release asset.

1. Download the built `.rpm` (release *1*).
2. Rebuild the same tree with `packaging/RELEASE` bumped to `2`.
3. `dnf install -y ./nasmount-<v>-1.fc44.x86_64.rpm` — the command the README
   documents.
4. Seed state the guard would refuse to remove: `install -d -m 0700
   /etc/nasmount`, plus a marker-bearing `.mount`/`.automount` pair from the
   golden corpus into `/etc/systemd/system`.
5. `dnf install -y ./nasmount-<v>-2.fc44.x86_64.rpm` — **must succeed**.
6. Assert: `rpm -q --qf '%{RELEASE}'` is `2.fc44`; `/etc/nasmount` still exists
   with mode `0700`; both seeded unit files are byte-identical to the corpus;
   and `/usr/lib64/libexec/nasmount-package-guard` exits **1** (`Managed`) — not
   merely non-zero, since exit 2 is `Unsafe`/`Indeterminate` and would mean the
   corpus itself is broken.

   The state seeded in step 4 is deliberately redundant:
   [`PackageState::classify()`](../../src/core/packagestate.cpp#L97) returns
   `Managed` on `/etc/nasmount` alone, so the seeded units are what actually
   exercise marker parsing — the half that can regress.
7. Downgrade: `dnf install -y ./nasmount-<v>-1.fc44.x86_64.rpm`. **Assert that
   it succeeds and that the installed release becomes `1.fc44`** — the opposite
   of the DEB job's step 7. DNF5 downgrades on an exact version spec (§3.2), and
   the test must encode what Fedora actually does rather than what we would
   prefer. Then assert the seeded state in step 4 survived the downgrade too:
   that is the property users depend on, and it is the only one this family can
   still guarantee.

   Assert on the resulting installed version, never on the exit code — DNF can
   report "nothing to do" and exit 0.

   If §3.2's on-disk format gate is ever implemented, this step inverts and
   becomes a refusal test. Until then, a test asserting refusal would fail.
8. Snapshot the full RPM set (`rpm -qa --qf ... | sort`) before and after the
   upgrade and `cmp` them modulo the nasmount line, mirroring the existing
   refused-removal snapshot. This proves an upgrade causes no dependency churn
   — the DNF hazard AGENTS.md documents.
9. Clean up seeded state, then `dnf remove -y --no-autoremove nasmount` and
   assert `rpm -q nasmount` fails. This tests `%preun`/`%postun` and RPM's end
   state — **not** `nasmount-uninstall`, which cannot run in a container (it
   refuses to run as root and its first real step is an authenticated KAuth
   call).

### 7.3 Verifying §3.3 rather than assuming it

The claim "the RPM already restarts on upgrade" is the load-bearing one in this
plan, and container CI cannot check it — `enqueue-marked` requires a running
systemd (`[ -d /run/systemd/system ] || exit 0` short-circuits every helper
verb otherwise). It therefore belongs entirely to the VM checklist (§8), and the
CI job must **not** assert anything about the service being restarted, or it
will pass vacuously.

### 7.4 Making the new job actually blocking

Adding a job and listing it in `expected_ci` gates nothing on its own:

- [`ci_success`](../../.github/workflows/ci.yml#L228) declares
  `needs: [validate_packaging, build_deb, build_rpm, smoke_packages,
  verify_artifact_set]`, maps each to an `env:` var, and loops over exactly
  those five. `upgrade_rpm` must be added in **all three** places — `needs`,
  `env`, and the loop.
- [`verify_release_set`](../../.github/workflows/release.yml#L231) declares
  `needs: [validate_release, smoke_release_packages]`, and `attest_and_publish`
  gates on it. A release-side upgrade job must be added to that `needs` list, or
  a broken upgrade can still be published.

---

## 8. Privileged validation before publishing

Container CI covers none of KAuth, polkit, D-Bus, CIFS, `enqueue-marked`, or
reboot. On a fresh **Fedora KDE 44** VM:

1. Install 0.1.2 from the published asset. Add two shares — one credentials, one
   guest — through the KCM and through the Dolphin service menu.
2. Reboot; confirm both arm before login and mount on first access.
3. `sudo dnf install ./nasmount-fedora44-x86_64-0.1.3.rpm`. Confirm the upgrade
   succeeds and both shares remain mounted or armed.
4. Confirm the §3.3 chain actually fired: `systemctl show nasmount-boot.service
   -p ActiveEnterTimestamp` moved, and the journal shows the post-upgrade run.
   `a live mount already occupies the path` is **expected** for any share that
   was mounted at upgrade time; `refusing to bless it` for an idle share is not.
5. Confirm `systemctl is-enabled nasmount-boot.service` is unchanged, and that
   `rpm -qa | sort` differs from the pre-upgrade snapshot only in the nasmount
   line.
6. **Administrator-state check (§3.3) — an observation, not a pass/fail.**
   `systemctl disable --now nasmount-boot.service`, upgrade again from a
   `RELEASE=2` build, and record three things: whether the unit is still
   disabled (**expected: yes**), whether it is still stopped (**expected: no —
   `reload-or-restart --marked` enqueues a plain restart, which starts an
   inactive unit**), and whether the shares got re-armed as a result.

   Then answer §3.3's open question 1: did `systemctl set-property` succeed on
   the stopped unit at all? Check the journal around the transaction. If it
   silently failed under the macro's `|| :`, the unit stays stopped and there is
   nothing to fix. If it succeeded and the unit was started, decide whether to
   accept that or move to a `%posttrans` that restarts only a previously-active
   unit. Do not treat a started service here as an automatic failure — treat it
   as the answer this step exists to obtain.
7. Access both shares; add a third with the upgraded binaries. Reboot; confirm
   all three arm.
8. Confirm `dnf remove --no-autoremove nasmount` is refused, names the blocking
   path, and leaves the dependency set untouched.
9. Run `nasmount-uninstall`; confirm exit 0, all shares and credentials gone,
   mount-point directories retained, and `rpm -q nasmount` reports not installed.
10. Repeat step 3 on a host with **no** shares, to cover the empty-state upgrade.

---

## 9. Sequencing, and its relationship to the DEB plan

| Step | Content | Gate |
|---|---|---|
| 1 | Golden corpus + test (**shared**, DEB plan §7.3) | Green on the unchanged tree |
| 2 | §6.1 RPM changelog identity, date substitution, dist tag | `rpmlint`; identity gates in §7.1 |
| 3 | §3.1 delete `%pre` | §7.1 static gates; §7.2 `upgrade_rpm` job |
| 4 | §5 shared uninstall fixes | Shared with the DEB plan — implement once |
| 5 | README/AGENTS.md wording | Review |

The DEB and RPM plans share steps 1, 4, and the authorship items in §6.2. They
can land in either order, but **the shared work must not be implemented twice**.
If the DEB plan lands first, this plan reduces to §3.1, §6.1, and §7.

Release both families together as **0.1.3** with `packaging/RELEASE` reset to
`1`. Until this plan lands, the README must state explicitly that the RPM does
not support in-place upgrades while the DEB does — the DEB plan §9 already
requires that sentence, and this plan is what removes it.
