/*
 * inventory — the caller-scoped, privileged, read-only credential-health
 * view the `inventory` action returns.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The unprivileged frontend can already see the unit files themselves
 * (world-readable) and derive definition/runtime state through the core
 * verifier. The only fact it cannot see without root is whether a credential file
 * is actually healthy. This is only the privilege boundary for reading
 * credential files — it derives owner, stable ID, mode, and authentication
 * kind from validated unit markers, reads the corresponding credential
 * location, and returns only raw facts. It does not inspect runtime state,
 * call systemd, or decide whether a missing credential is currently an
 * error — Session::MountModel owns that interpretation, since it already has
 * the definition and runtime facts (that rule is never split between the
 * helper and the model).
 */

#pragma once

#include "unitvalue.h"

#include <QList>
#include <QString>

#include <sys/types.h>

namespace Root::Inventory
{

struct ShareRecord {
    QString id; ///< empty for a Tampered entry, whose id cannot be trusted

    /** Only meaningful when authentication == Credentials; both false for a
     *  guest share or a Tampered entry. */
    bool credentialApplicable = false;
    bool credentialHealthy = false;
};

/**
 * Enumerates `uid`'s owned units and reads the raw credential health for
 * each. `*error` is never set -- kept only so callers do not need to change
 * if a future failure mode appears.
 */
QList<ShareRecord> buildFor(uid_t uid, QString *error);

/** Serialises `records` as a compact JSON array -- the exact shape returned
 *  to the caller under the `shares` reply key. */
QString toJson(const QList<ShareRecord> &records);

} // namespace Root::Inventory
