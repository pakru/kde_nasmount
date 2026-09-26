#!/bin/bash

# Prevent the removed transaction/Edit/Forget surface from silently returning.
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
