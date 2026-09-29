#!/bin/sh
#
# Opens the SMB Network Mounts module in its own window.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This is the same module System Settings hosts, so it needs no privileges of
# its own; every change still goes through the authenticated KAuth helper.

set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo "Do not run nasmount as root." >&2
    exit 1
fi

if ! command -v kcmshell6 >/dev/null 2>&1; then
    echo "nasmount: kcmshell6 not found; install KDE's kf6-kcmutils." >&2
    exit 1
fi

exec kcmshell6 kcm_nasmount "$@"
