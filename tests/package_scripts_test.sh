#!/bin/bash
# Rootless static/functional checks for native package shell entry points.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
uninstaller="$repo_root/packaging/nasmount-uninstall.sh"
release_tagger="$repo_root/packaging/tag-release.sh"

bash -n "$uninstaller"
bash -n "$release_tagger"
# Sourcing exposes only pure command selection; main is guarded and therefore
# cannot request authorization during CTest.
source "$uninstaller"

[ "$(package_manager_binary deb)" = /usr/bin/apt-get ]
[ "$(package_manager_binary rpm)" = /usr/bin/dnf ]
mapfile -t deb_remove < <(package_manager_remove_arguments deb)
mapfile -t rpm_remove < <(package_manager_remove_arguments rpm)
# purge, not remove: `remove` leaves the package in dpkg's config-files state,
# so `dpkg -l` keeps listing nasmount after an advertised full purge.
[ "${deb_remove[*]}" = "purge -y nasmount" ]
[ "${rpm_remove[*]}" = "remove -y --no-autoremove nasmount" ]
if package_manager_binary source >/dev/null 2>&1; then
    echo "ERROR: source installs must not select a package manager" >&2
    exit 1
fi
if package_manager_remove_arguments source >/dev/null 2>&1; then
    echo "ERROR: source installs must not select native removal arguments" >&2
    exit 1
fi

grep -Fq '"$NASMOUNT_CLEANUP" --manifest "$NASMOUNT_MANIFEST"' "$uninstaller"
cleanup_line=$(grep -nF '"$NASMOUNT_CLEANUP" --manifest "$NASMOUNT_MANIFEST"' "$uninstaller" | cut -d: -f1 || true)
remove_line=$(grep -nF 'if ! remove_native_package; then' "$uninstaller" | tail -1 | cut -d: -f1 || true)
[ "$cleanup_line" -lt "$remove_line" ] || {
    echo "ERROR: native package removal appears before authenticated cleanup" >&2
    exit 1
}

# Every check that can fail without changing anything must run before cleanup.
# Validating sudo or the package manager after the purge means a host missing
# either one loses its shares and then fails.
preflight_line=$(grep -nF 'validate_removal_environment "$family"' "$uninstaller" | tail -1 | cut -d: -f1 || true)
[ -n "$preflight_line" ] && [ "$preflight_line" -lt "$cleanup_line" ] || {
    echo "ERROR: removal preflight does not precede authenticated cleanup" >&2
    exit 1
}
for guard in '[ -x /usr/bin/sudo ]' '[ -x "$NASMOUNT_MANAGER" ]'; do
    guard_line=$(grep -nF "$guard" "$uninstaller" | tail -1 | cut -d: -f1 || true)
    [ -n "$guard_line" ] && [ "$guard_line" -lt "$cleanup_line" ] || {
        echo "ERROR: environment guard runs after cleanup: $guard" >&2
        exit 1
    }
done

# Non-interactive mode exists, and the wrapper reports the three distinct
# post-cleanup states rather than one undifferentiated failure.
grep -Fq -- '-y|--yes) assume_yes=1' "$uninstaller"
grep -Fq 'cleanup_status" -eq 2' "$uninstaller"
grep -Fq 'return 3' "$uninstaller"

# An unconfirmed cleanup must not be reported as "nothing was removed": the
# privileged purge is not atomic across shares, and a lost KAuth reply cannot
# be distinguished from a purge that ran. Only nasmount-cleanup's exit 2
# (refused before dispatch) proves that, and only that branch may say so.
grep -Fq 'may be unchanged' "$uninstaller" || {
    echo "ERROR: uninstaller does not report the indeterminate cleanup outcome" >&2
    exit 1
}
# The claim must sit strictly inside the `-eq 2` then-branch, i.e. after the
# test and before its `else`. Bounding it any more loosely would let the claim
# drift into the indeterminate branch and still pass.
refusal_line=$(grep -nF 'cleanup_status" -eq 2' "$uninstaller" | cut -d: -f1 || true)
else_line=$(awk -v start="$refusal_line" 'NR > start && $1 == "else" { print NR; exit }' "$uninstaller")
[ -n "$refusal_line" ] && [ -n "$else_line" ] || {
    echo "ERROR: cannot locate the confirmed-refusal branch in the uninstaller" >&2
    exit 1
}
# Only claims made *after* cleanup has run are at risk of being false: before
# it, nothing has been touched and the statement is trivially true (the sudo
# preflight relies on that).
while IFS=: read -r line _; do
    [ "$line" -lt "$cleanup_line" ] && continue
    [ "$line" -gt "$refusal_line" ] && [ "$line" -lt "$else_line" ] || {
        echo "ERROR: 'nothing was removed' at line $line is after cleanup and" >&2
        echo "       outside the confirmed-refusal branch ($refusal_line..$else_line)" >&2
        exit 1
    }
done < <(grep -n -i 'nothing was removed' "$uninstaller")

# --- managed-unit matcher -----------------------------------------------------
# prerm/postrm decide what to unmount and delete from a shell matcher, so the
# matcher is the highest-consequence code in the packaging. Exercise the real
# function text against adversarial fixtures, with its directory redirected at
# a temporary root. Both scripts must carry byte-identical copies: postrm runs
# after every shipped file is gone and cannot source a shared helper.
prerm_template="$repo_root/packaging/debian/nasmount.prerm.in"
postrm_script="$repo_root/packaging/debian/nasmount.postrm"
sh -n "$postrm_script"

# Whitespace-tolerant: the DEB scripts define the matcher at column 0, the
# spec nests it inside an `if`, and the two must still compare equal.
extract_matcher()
{
    sed -n '/^[[:space:]]*nasmount_managed_units()/,/^[[:space:]]*}$/p' "$1"
}
if ! diff <(extract_matcher "$prerm_template") <(extract_matcher "$postrm_script") >/dev/null; then
    echo "ERROR: the managed-unit matcher differs between prerm and postrm" >&2
    exit 1
fi
[ -n "$(extract_matcher "$prerm_template")" ]

matcher_root="$(mktemp -d)"
matcher_lib="$matcher_root/matcher.sh"
# Declared before the trap so an early exit here cannot trip `set -u` inside it.
tag_test_root=""
trap 'rm -rf -- "$matcher_root" ${tag_test_root:+"$tag_test_root"}' EXIT
units_dir="$matcher_root/units"
mkdir -p "$units_dir"
extract_matcher "$prerm_template" | sed "s#/etc/systemd/system#$units_dir#g" > "$matcher_lib"
# shellcheck disable=SC1090 -- generated from the shipped maintainer script
. "$matcher_lib"

marker='# X-Nasmount-Managed=1'
printf '%s\n[Mount]\n' "$marker"       > "$units_dir/home-t-mnt-a.mount"
printf '%s\n[Automount]\n' "$marker"   > "$units_dir/home-t-mnt-a.automount"
printf '[Mount]\nWhat=//x/y\n'         > "$units_dir/foreign.mount"
printf '%s extra\n' "$marker"           > "$units_dir/trailing-text.mount"
printf '  %s\n' "$marker"               > "$units_dir/indented.mount"
printf '%s\n' "$marker"                 > "$units_dir/notaunit.service"
ln -s "$units_dir/home-t-mnt-a.mount"      "$units_dir/symlinked.mount"
# systemctl expands '*', '?' and '[...]' in a unit argument against every
# loaded unit, so a marked file named '*.mount' would turn `systemctl stop`
# into "stop every mount unit on the system". A newline would be split by
# `read` into two unit names. Neither may reach the teardown.
printf '%s\n' "$marker"                 > "$units_dir/*.mount"
printf '%s\n' "$marker"                 > "$units_dir/?.mount"
printf '%s\n' "$marker"                 > "$units_dir/[abc].mount"
printf '%s\n' "$marker"                 > "$units_dir/two words.mount"
# A name that is legitimately systemd-escaped must still be accepted.
printf '%s\n[Mount]\n' "$marker"        > "$units_dir/home-t-my\\x2dshare.mount"

matched=$(nasmount_managed_units 2>/dev/null | sort | tr '\n' ' ')
expected="home-t-mnt-a.automount home-t-mnt-a.mount home-t-my\\x2dshare.mount "
[ "$matched" = "$expected" ] || {
    echo "ERROR: matcher selected [$matched], expected [$expected]" >&2
    echo "       a false positive here means deleting someone else's unit file" >&2
    exit 1
}

# An empty unit directory must yield nothing and must not error on the
# unmatched glob.
empty_dir="$matcher_root/empty"
mkdir -p "$empty_dir"
extract_matcher "$prerm_template" | sed "s#/etc/systemd/system#$empty_dir#g" \
    > "$matcher_root/matcher-empty.sh"
(
    # shellcheck disable=SC1090
    . "$matcher_root/matcher-empty.sh"
    empty_output=$(nasmount_managed_units)
    [ -z "$empty_output" ] || {
        echo "ERROR: matcher produced output for an empty unit directory" >&2
        exit 1
    }
)

# --- RPM scriptlets -----------------------------------------------------------
# The spec carries its own copies of the matcher and teardown. RPM expands
# macros inside scriptlet bodies, so a literal % must be written %% -- an
# unescaped printf '%s' would be handed to the macro expander.
spec="$repo_root/packaging/rpm/nasmount.spec.in"

if grep -Eq '^%pre$' "$spec"; then
    echo "ERROR: the RPM %pre section is back; it refuses in-place upgrades" >&2
    exit 1
fi
if grep -Fq 'does not yet support in-place package upgrades' "$spec"; then
    echo "ERROR: the RPM still refuses in-place upgrades" >&2
    exit 1
fi
# Note on `|| true` in the line-number lookups below: this script runs under
# `set -o pipefail`, so a grep that finds nothing fails the whole command
# substitution and kills the script before the assertion that would explain
# why. Collapsing a no-match to an empty string keeps the diagnostics reachable.
extract_scriptlet()
{
    awk -v section="$1" '
        $0 == section { capture = 1; next }
        capture && /^%/ { exit }
        capture { print }
    ' "$spec"
}

for section in %preun %postun; do
    body=$(extract_scriptlet "$section")
    [ -n "$body" ] || {
        echo "ERROR: $section is empty in the spec" >&2
        exit 1
    }
    # The guard binary still ships (it is listed in %files and is still a
    # useful diagnostic), but it must not come back in a *scriptlet*, where it
    # would refuse the removal this design now performs.
    if printf '%s\n' "$body" | grep -Fq 'nasmount-package-guard'; then
        echo "ERROR: $section refuses removal again; teardown is its job now" >&2
        exit 1
    fi
    # RPM runs the OLD package's %preun/%postun with $1=1 during an upgrade.
    # An ungated teardown would unmount every share on every update -- the
    # single highest-consequence mistake available in this file.
    gate_line=$(printf '%s\n' "$body" | grep -n '"\$1" -eq 0' | head -1 | cut -d: -f1 || true)
    first_use=$(printf '%s\n' "$body" | grep -n 'nasmount_managed_units' | head -1 | cut -d: -f1 || true)
    [ -n "$gate_line" ] && [ -n "$first_use" ] && [ "$gate_line" -lt "$first_use" ] || {
        echo "ERROR: $section touches managed units without an \$1 -eq 0 gate" >&2
        exit 1
    }
    # Expand %% the way rpm would, then prove the result is valid shell.
    printf '%s\n' "$body" | sed 's/%%/%/g' > "$matcher_root/rpm$section.sh"
    sh -n "$matcher_root/rpm$section.sh" || {
        echo "ERROR: $section is not valid shell after macro expansion" >&2
        exit 1
    }
    # Check the SPEC text, not the expansion: an unescaped %s survives the
    # %%->% substitution unchanged and would look correct afterwards, while
    # rpm itself would hand it to the macro expander.
    if printf '%s\n' "$body" | grep -Fq "printf '%s\\n'"; then
        echo "ERROR: $section has an unescaped % in a scriptlet; write %%" >&2
        exit 1
    fi
    printf '%s\n' "$body" | grep -Fq "printf '%%s\\n'" || {
        echo "ERROR: $section is missing the escaped printf format" >&2
        exit 1
    }
done

# All four matcher copies -- two DEB scripts, two spec scriptlets -- must be
# the same function, modulo the indentation the spec nests them at. Each spec
# scriptlet carries its own copy, so extract them per section rather than from
# the whole file.
normalise() { sed 's/^[[:space:]]*//'; }
deb_matcher=$(extract_matcher "$prerm_template" | normalise)
[ -n "$deb_matcher" ]

for section in %preun %postun; do
    expanded="$matcher_root/rpm$section.sh"
    section_matcher=$(extract_matcher "$expanded" | normalise)
    [ -n "$section_matcher" ] || {
        echo "ERROR: no matcher found in $section" >&2
        exit 1
    }
    if [ "$section_matcher" != "$deb_matcher" ]; then
        echo "ERROR: the $section matcher has drifted from the DEB one" >&2
        diff <(printf '%s\n' "$deb_matcher") <(printf '%s\n' "$section_matcher") >&2 || true
        exit 1
    fi

    # Run it against the same adversarial fixtures as the DEB copy.
    printf '%s\n' "$section_matcher" | sed "s#/etc/systemd/system#$units_dir#g" \
        > "$matcher_root/matcher$section.sh"
    (
        # shellcheck disable=SC1090
        . "$matcher_root/matcher$section.sh"
        section_matched=$(nasmount_managed_units 2>/dev/null | sort | tr '\n' ' ')
        [ "$section_matched" = "$expected" ] || {
            echo "ERROR: $section matcher selected [$section_matched]" >&2
            exit 1
        }
    )
done

# The fail-closed state remover is duplicated the same way and must not drift.
extract_remover()
{
    sed -n '/^[[:space:]]*nasmount_remove_state_dir()/,/^[[:space:]]*}$/p' "$1"
}
deb_remover=$(extract_remover "$postrm_script" | normalise)
[ -n "$deb_remover" ] || {
    echo "ERROR: no state remover found in postrm" >&2
    exit 1
}
rpm_remover=$(extract_remover "$matcher_root/rpm%postun.sh" | normalise)
if [ "$deb_remover" != "$rpm_remover" ]; then
    echo "ERROR: the state remover has drifted between DEB and RPM" >&2
    diff <(printf '%s\n' "$deb_remover") <(printf '%s\n' "$rpm_remover") >&2 || true
    exit 1
fi
# It must not be a recursive delete: rm -rf follows a bind mount or a nested
# mount straight out of our own tree.
for script in "$postrm_script" "$spec"; do
    if grep -Eq '^[^#]*rm -rf' "$script"; then
        echo "ERROR: $script uses a recursive delete on state directories" >&2
        exit 1
    fi
done

# --- removal contract ---------------------------------------------------------
# Teardown must never run on upgrade: it unmounts live shares.
if grep -Eq '^\s*upgrade\)' "$prerm_template"; then
    echo "ERROR: prerm tears down managed units on upgrade" >&2
    exit 1
fi
# The *call*, not the definition: `grep nasmount_disable_units` alone also
# matches the function's own `nasmount_disable_units()` line, so deleting the
# call from the remove) branch would go unnoticed.
prerm_case_line=$(grep -n '^    remove)' "$prerm_template" | cut -d: -f1 || true)
prerm_esac_line=$(grep -n '^esac' "$prerm_template" | head -1 | cut -d: -f1 || true)
prerm_call_line=$(grep -n '^[[:space:]]*nasmount_disable_units$' "$prerm_template" | cut -d: -f1 || true)
[ -n "$prerm_case_line" ] && [ -n "$prerm_esac_line" ] && [ -n "$prerm_call_line" ] \
    && [ "$prerm_call_line" -gt "$prerm_case_line" ] \
    && [ "$prerm_call_line" -lt "$prerm_esac_line" ] || {
    echo "ERROR: prerm does not invoke nasmount_disable_units from remove)" >&2
    exit 1
}
# Automount halves are disarmed before mount halves are stopped, so nothing
# can re-trigger the path mid-unmount.
# Match invocations, not prose: the scripts' own comments mention these
# commands, and grepping for the bare words picks a comment line instead.
for script in "$prerm_template" "$postrm_script"; do
    automount_line=$(grep -n '^[^#]*systemctl disable --now --' "$script" | head -1 | cut -d: -f1 || true)
    mount_stop_line=$(grep -n '^[^#]*systemctl stop --' "$script" | head -1 | cut -d: -f1 || true)
    [ -n "$automount_line" ] && [ -n "$mount_stop_line" ] \
        && [ "$automount_line" -lt "$mount_stop_line" ] || {
        echo "ERROR: $script stops mount halves before disarming automounts" >&2
        exit 1
    }
done
# Purge removes unit files, credentials and runtime records; remove does not.
grep -Fq 'nasmount_purge_state' "$postrm_script"
for root in /etc/nasmount /run/nasmount /run/nasmount-ids; do
    grep -Fq "$root" "$postrm_script"
done

# Exercise the release helper against a local bare origin. No network access,
# credentials, tag push, or modification of the source repository is involved.
tag_test_root="$(mktemp -d)"
tag_test_repo="$tag_test_root/repository"
tag_test_origin="$tag_test_root/origin.git"
git init --bare --quiet "$tag_test_origin"
git init --quiet --initial-branch=master "$tag_test_repo"
mkdir "$tag_test_repo/packaging"
cp "$release_tagger" "$tag_test_repo/packaging/tag-release.sh"
cp "$repo_root/VERSION" "$tag_test_repo/VERSION"
cp "$repo_root/packaging/RELEASE" "$tag_test_repo/packaging/RELEASE"
git -C "$tag_test_repo" config user.name 'nasmount release test'
git -C "$tag_test_repo" config user.email 'release-test@nasmount.invalid'
git -C "$tag_test_repo" config tag.gpgSign false
git -C "$tag_test_repo" add VERSION packaging
git -C "$tag_test_repo" commit --quiet -m 'Test release state'
git -C "$tag_test_repo" remote add origin "$tag_test_origin"
git -C "$tag_test_repo" push --quiet --set-upstream origin master

touch "$tag_test_repo/untracked-change"
if "$tag_test_repo/packaging/tag-release.sh" >"$tag_test_root/dirty.out" 2>&1; then
    echo "ERROR: release helper accepted a dirty worktree" >&2
    exit 1
fi
rm -- "$tag_test_repo/untracked-change"

expected_tag="v$(tr -d '\n' < "$tag_test_repo/VERSION")"
tag_output="$("$tag_test_repo/packaging/tag-release.sh")"
[ "$(git -C "$tag_test_repo" cat-file -t "refs/tags/$expected_tag")" = tag ]
grep -Fq "git push origin refs/tags/$expected_tag" <<<"$tag_output"
[ -z "$(git --git-dir="$tag_test_origin" tag --list "$expected_tag")" ] || {
    echo "ERROR: release helper pushed the tag instead of leaving it local" >&2
    exit 1
}
if "$tag_test_repo/packaging/tag-release.sh" >"$tag_test_root/local-duplicate.out" 2>&1; then
    echo "ERROR: release helper accepted an existing local tag" >&2
    exit 1
fi

git -C "$tag_test_repo" push --quiet origin "refs/tags/$expected_tag"
git -C "$tag_test_repo" tag --delete "$expected_tag" >/dev/null
if "$tag_test_repo/packaging/tag-release.sh" >"$tag_test_root/remote-duplicate.out" 2>&1; then
    echo "ERROR: release helper accepted an existing remote tag" >&2
    exit 1
fi

echo "Native package shell checks passed."
