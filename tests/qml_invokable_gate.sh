#!/bin/bash
#
# Gate: every `actions.<name>(` call in QML must exist as a Q_INVOKABLE on
# Session::MountActions, and every `kcm.<name>` / `backend.<name>` must exist
# as a Q_PROPERTY or Q_INVOKABLE on its host object.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This boundary is invisible to everything else in the build. QML resolves
# these names at runtime against a QObject exposed as `var`, so a rename on
# the C++ side leaves the QML call compiling cleanly, passing qmllint, and
# passing every ctest binary -- then throwing a TypeError the first time a
# user clicks the button.
#
# Deliberately grep-based rather than a Qt test: the failure is a *name*
# mismatch across two languages, so comparing the declared names directly is
# both the simplest check and the one that cannot itself drift.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# Strips /* ... */ blocks and // line comments. Both matter: these files
# open with a block comment that deliberately names `kcm.` while explaining
# why it must not appear in the code below, and prose citing an old API name
# must never fail the gate.
strip_comments() {
    sed 's://.*::' "$1" | awk '
        { line = $0
          while (1) {
            if (inblock) {
              i = index(line, "*/")
              if (i == 0) { line = ""; break }
              line = substr(line, i + 2); inblock = 0
            } else {
              i = index(line, "/*")
              if (i == 0) break
              rest = substr(line, i + 2); line = substr(line, 1, i - 1); inblock = 1
              j = index(rest, "*/")
              if (j > 0) { line = line substr(rest, j + 2); inblock = 0 }
            }
          }
          print line }'
}

status=0
qml_files=$(find src -name '*.qml')

fail() {
    echo "ERROR: $1" >&2
    status=1
}

# --- actions.<name>( must be Q_INVOKABLE on MountActions --------------------
invokables=$(grep -oE 'Q_INVOKABLE[[:space:]]+void[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*' \
                 src/session/mountactions.h | awk '{print $NF}' | sort -u)

for f in $qml_files; do
    # Strip comments so prose mentioning an old name never trips the gate.
    calls=$(strip_comments "$f" | grep -oE '\bactions\.[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(' \
            | sed 's/^actions\.//; s/[[:space:]]*($//; s/($//' | sort -u)
    for c in $calls; do
        if ! printf '%s\n' "$invokables" | grep -qx "$c"; then
            fail "$f calls actions.$c(), which is not Q_INVOKABLE on Session::MountActions"
        fi
    done
done

# --- host context objects: kcm.<name> / backend.<name> ----------------------
check_host() {
    local prefix="$1" header="$2"
    local members
    members=$(grep -oE 'Q_PROPERTY\([^ ]+[[:space:]]+\*?[a-zA-Z_][a-zA-Z0-9_]*|Q_INVOKABLE[[:space:]]+[a-zA-Z:_<>]+[[:space:]]+\*?[a-zA-Z_][a-zA-Z0-9_]*' \
                  "$header" | awk '{print $NF}' | tr -d '*' | sort -u)
    for f in $qml_files; do
        local refs
        refs=$(strip_comments "$f" | grep -oE "\b${prefix}\.[a-zA-Z_][a-zA-Z0-9_]*" \
               | sed "s/^${prefix}\.//" | sort -u)
        for r in $refs; do
            if ! printf '%s\n' "$members" | grep -qx "$r"; then
                fail "$f references ${prefix}.$r, absent from $(basename "$header")"
            fi
        done
    done
}

check_host kcm src/kcm/kcmnasmount.h
check_host backend src/dialog/dialogbackend.h

# --- ShareForm must stay host-agnostic -------------------------------------
# It is embedded by both front ends; a host reference silently makes it
# usable by only one of them, and the two forms drift apart.
if strip_comments src/kcm/ui/ShareForm.qml | grep -qE '\b(kcm|backend)\.'; then
    fail "ShareForm.qml references a host context object; it must stay host-agnostic"
fi

if [ "$status" -eq 0 ]; then
    echo "QML/C++ invokable gate passed."
fi
exit "$status"
