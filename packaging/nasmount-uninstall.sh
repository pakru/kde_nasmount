#!/bin/bash
#
# Persistent native-package uninstall entry point.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This command must run as the owning desktop user: nasmount-cleanup obtains
# KAuth authorization and purges verified share state before apt/dnf is
# allowed to remove the helper and policy that make that purge possible.
#
# The ordering rule that governs this file: *every* check that can fail
# without changing anything runs before nasmount-cleanup, never after. A
# missing sudo discovered after the purge would mean the user's shares are
# gone and the software is still installed -- the one outcome this command
# exists to avoid.
#
# Exit codes, which callers and the README depend on:
#
#   0  shares purged and package removed
#   1  usage or preflight error -- nothing was attempted
#   2  cleanup did not confirm success; the package was not removed. State may
#      be unchanged, partly removed, or fully removed -- see the message
#   3  cleanup confirmed, package removal failed: state is purged, software
#      still installed; re-run to finish

set -euo pipefail

readonly NASMOUNT_MANIFEST=/usr/share/nasmount/cleanup-manifest.txt
readonly NASMOUNT_FAMILY_FILE=/usr/share/nasmount/package-family
readonly NASMOUNT_CLEANUP=/usr/bin/nasmount-cleanup

assume_yes=0

usage()
{
    cat <<'USAGE'
Usage: nasmount-uninstall [--yes]

Purges every nasmount share owned by the calling user -- its credentials and
runtime records -- removes ~/.config/nasmountrc, then removes the native
package. Mount-point directories are retained. Must run as the desktop user
who owns the shares, not as root.

  -y, --yes    do not ask for interactive confirmation
  -h, --help   show this message
USAGE
}

package_manager_binary()
{
    case "$1" in
        deb) printf '%s\n' /usr/bin/apt-get ;;
        rpm) printf '%s\n' /usr/bin/dnf ;;
        *) return 1 ;;
    esac
}

package_manager_remove_arguments()
{
    case "$1" in
        # purge, not remove: dpkg otherwise retains the package in its
        # config-files ("rc") state, so `dpkg -l` keeps listing nasmount after
        # what this command tells the user was a full purge. The package ships
        # no conffiles, so purge and remove differ only in that bookkeeping.
        deb) printf '%s\n' purge -y nasmount ;;
        # -y for the same reason as the deb arm: this wrapper has already
        # taken its own confirmation, and a second prompt it cannot answer
        # would fail *after* the purge has destroyed the user's shares.
        # --no-autoremove stays mandatory: DNF may continue auto-removing
        # dependencies after a failed transaction, leaving nasmount installed
        # without Qt.
        rpm) printf '%s\n' remove -y --no-autoremove nasmount ;;
        *) return 1 ;;
    esac
}

validate_root_file()
{
    local path=$1
    local maximum_size=$2
    local owner mode size

    [ -f "$path" ] && [ ! -L "$path" ] || {
        echo "ERROR: required package metadata is not a regular file: $path" >&2
        return 1
    }
    owner=$(stat -c '%u' -- "$path")
    mode=$(stat -c '%a' -- "$path")
    size=$(stat -c '%s' -- "$path")
    [ "$owner" = 0 ] || {
        echo "ERROR: package metadata is not owned by root: $path" >&2
        return 1
    }
    (( (8#$mode & 8#022) == 0 )) || {
        echo "ERROR: package metadata is group/world writable: $path" >&2
        return 1
    }
    [ "$size" -le "$maximum_size" ] || {
        echo "ERROR: package metadata is unexpectedly large: $path" >&2
        return 1
    }
}

read_package_family()
{
    local family extra
    IFS= read -r family < "$NASMOUNT_FAMILY_FILE"
    IFS= read -r extra < <(sed -n '2p' "$NASMOUNT_FAMILY_FILE") || true
    [ -z "$extra" ] || {
        echo "ERROR: package-family metadata contains extra lines." >&2
        return 1
    }
    case "$family" in
        source|deb|rpm) printf '%s\n' "$family" ;;
        *)
            echo "ERROR: unsupported package family '$family'." >&2
            return 1
            ;;
    esac
}

# Everything the removal step needs, proven before anything is destroyed.
# Populates NASMOUNT_MANAGER and NASMOUNT_REMOVE_ARGUMENTS.
validate_removal_environment()
{
    local family=$1
    local argument_lines
    NASMOUNT_MANAGER=$(package_manager_binary "$family") || {
        echo "ERROR: no native package manager for '$family'." >&2
        return 1
    }
    argument_lines=$(package_manager_remove_arguments "$family") || {
        echo "ERROR: no native package removal arguments for '$family'." >&2
        return 1
    }
    mapfile -t NASMOUNT_REMOVE_ARGUMENTS <<< "$argument_lines"
    [ -x /usr/bin/sudo ] || {
        echo "ERROR: /usr/bin/sudo is required to remove the package." >&2
        return 1
    }
    [ -x "$NASMOUNT_MANAGER" ] || {
        echo "ERROR: expected package manager is not installed: $NASMOUNT_MANAGER" >&2
        return 1
    }
}

# `-x /usr/bin/sudo` proves the binary exists, not that this user may use it.
# A user who can authenticate to KAuth may still not be in sudoers, and
# discovering that after the purge means their shares are gone and the package
# is still installed. Runs after the confirmation prompt (so an aborted run
# never asks for a password) but before any mutation.
validate_sudo_authorization()
{
    if [ "$assume_yes" -eq 1 ]; then
        /usr/bin/sudo -n -v >/dev/null 2>&1 || {
            echo "ERROR: --yes needs cached sudo credentials, and none are cached." >&2
            echo "Run 'sudo -v' first, or run without --yes." >&2
            return 1
        }
        return 0
    fi
    /usr/bin/sudo -v || {
        echo "ERROR: you are not authorized to run sudo on this host." >&2
        echo "Nothing was removed." >&2
        return 1
    }
}

remove_native_package()
{
    /usr/bin/sudo -- "$NASMOUNT_MANAGER" "${NASMOUNT_REMOVE_ARGUMENTS[@]}"
}

main()
{
    while [ $# -gt 0 ]; do
        case "$1" in
            -y|--yes) assume_yes=1 ;;
            -h|--help) usage; return 0 ;;
            *)
                echo "ERROR: unknown argument '$1'." >&2
                usage >&2
                return 1
                ;;
        esac
        shift
    done

    if [ "$(id -u)" -eq 0 ]; then
        echo "Do not run nasmount-uninstall as root." >&2
        return 1
    fi

    # --- preflight: nothing below this block has changed anything yet -------
    validate_root_file "$NASMOUNT_MANIFEST" $((64 * 1024))
    validate_root_file "$NASMOUNT_FAMILY_FILE" 32
    local family
    family=$(read_package_family)
    if [ "$family" = source ]; then
        echo "This installation is owned by a source checkout." >&2
        echo "Run ./uninstall.sh from the checkout that installed nasmount." >&2
        return 1
    fi
    [ -x "$NASMOUNT_CLEANUP" ] || {
        echo "ERROR: $NASMOUNT_CLEANUP is not installed." >&2
        return 1
    }
    validate_removal_environment "$family"
    # --- end preflight -----------------------------------------------------

    if [ "$assume_yes" -eq 0 ]; then
        echo "This will purge every nasmount share owned by your user, remove"
        echo "your nasmount configuration, and then remove the native package."
        echo "Mount-point directories will remain."
        read -r -p "Proceed? [y/N] " answer
        [ "$answer" = y ] || [ "$answer" = Y ] || {
            echo "Aborted."
            return 0
        }
    fi

    validate_sudo_authorization

    # nasmount-cleanup distinguishes a refusal (nothing dispatched) from an
    # indeterminate outcome; relay that distinction instead of flattening it
    # into "cleanup failed". Both leave the package installed.
    local cleanup_status=0
    "$NASMOUNT_CLEANUP" --manifest "$NASMOUNT_MANIFEST" || cleanup_status=$?
    if [ "$cleanup_status" -ne 0 ]; then
        if [ "$cleanup_status" -eq 2 ]; then
            echo "Nothing was removed and nasmount is still installed." >&2
        else
            echo "nasmount is still installed. Managed state may be unchanged," >&2
            echo "partly removed, or fully removed; re-run this command." >&2
        fi
        return 2
    fi

    if ! remove_native_package; then
        echo "ERROR: managed state was purged, but removing the package failed." >&2
        echo "nasmount is still installed with no shares; re-run to finish." >&2
        return 3
    fi

    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    else
        echo "Restart Dolphin and System Settings to refresh KDE integration."
    fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
