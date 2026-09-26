#!/bin/bash

# Prevent the removed transaction/Edit/Forget surface from silently returning,
# and hold the placement rules that would otherwise rest only on review: which
# libraries may carry the smb:// address parser, which QML may import
# Kirigami, and which calls the KCM may use to open a folder. Every check
# greps comments too, so no comment may spell out a forbidden name -- word
# explanations around it.
# Run from CTest and install.sh.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

check_no_matches() {
    local label=$1
    local pattern=$2
    shift 2

    local output
    local status
    set +e
    output=$(grep -rn -E --exclude='removed_api_gates.sh' "$pattern" "$@" 2>&1)
    status=$?
    set -e
    if [ "$status" -eq 0 ]; then
        echo "ERROR: $label reintroduced a removed API or stale contract:" >&2
        echo "$output" >&2
        return 1
    fi
    if [ "$status" -ne 1 ]; then
        echo "ERROR: $label gate could not run:" >&2
        echo "$output" >&2
        return 1
    fi
}

check_no_matches "Edit" \
    'editShare|editSystemShare|isMountAffecting|doReplace|ReplaceInput|ReplaceOutput|recoverPendingReplace|replacesystem|Operations::replace|Operation::Replace' \
    src tests io.github.pakru.nasmount.actions

check_no_matches "Forget/tombstone/repair" \
    'tombstone|sweeptombstones|forgetShare|forgetPassword|forgetOrphanByPath|forgetsystem|Operation::Forget|isPartialRepair|partialRepair|forgetAutomountId' \
    src tests io.github.pakru.nasmount.actions

check_no_matches "transaction/recovery" \
    'Transaction::|transaction\.(h|cpp)|transaction_test|recoverAll|recoverPending' \
    src tests CMakeLists.txt install.sh uninstall.sh

check_no_matches "pending presentation roles" \
    'hasPendingTransactions|PendingTransactionRole|PendingOperationRole|pendingTransaction|pendingOperation|AdminOnlyRole|adminOnly' \
    src tests

check_no_matches "privileged inventory runtime coupling" \
    'runtimeCorrelation|verificationStr|inspectRuntime|\bwhat\b' \
    src/root/inventory.h src/root/inventory.cpp

# The smb:// address parser belongs to the session library. Core is linked
# into the privileged helper, so the parser appearing in core, the helper or
# the root library would let the helper be handed an smb:// value and accept
# it; the helper's contract is //host/share only.
check_no_matches "smb:// parser placement" \
    'ShareAddress|parseSmbUrl|shareaddress\.h' \
    src/core src/helper src/root

# The service-menu dialog used to carry its own copy of that parser and of the
# domain/user split. They moved into the session library; a second copy in
# the dialog would let the two front ends parse addresses differently.
check_no_matches "removed dialog parser" \
    'SmbUrl::(parse|splitDomainUser|Identity)\b' \
    src tests

# The KCM opens a mount point by asking the file manager over D-Bus. Qt's own
# URL-opening paths examine a local path inside this process to choose an
# application, and examining an automount point triggers the mount, blocking
# System Settings until it completes or times out.
check_no_matches "non-blocking Open" \
    'openUrlExternally|QDesktopServices' \
    src/kcm

# shareform_qml_test loads ShareForm.qml in the native package builds, whose
# containers have no Kirigami. A workstation does have it, so a Kirigami import
# in the form would pass every local test and fail only in a package build.
check_no_matches "Kirigami-free ShareForm" \
    'org\.kde\.kirigami' \
    src/kcm/ui/ShareForm.qml

set +e
documentation_output=$(grep -rn -E \
    '/etc/nasmount/transactions|recovery records' README.md src install.sh uninstall.sh 2>&1)
documentation_status=$?
set -e
if [ "$documentation_status" -eq 0 ]; then
    echo "ERROR: stale transaction storage documentation remains:" >&2
    echo "$documentation_output" >&2
    exit 1
fi
if [ "$documentation_status" -ne 1 ]; then
    echo "ERROR: documentation gate could not run:" >&2
    echo "$documentation_output" >&2
    exit 1
fi

echo "Removed-API gates passed."
