/*
 * operations — define/undefine/purge as direct, checked operations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Owner and identity come from the caller (helper.cpp), which takes them from
 * KAuth and the validated marker, never from a free-form caller argument.
 * There is no in-place replace/edit: changing a share's UNC, mount point,
 * credentials, authentication kind, or access mode is done by removing the
 * definition and creating it again.
 *
 * Neither writes a durable manifest: a same-process failure is compensated
 * inline (checked, best-effort, tracked only in memory); a crash or kill
 * leaves whatever was durably written, visible on the next inventory
 * refresh as an owned Partial pair or a plain checked-failure retry. There
 * is nothing to recover at startup or before a mutating action.
 */

#pragma once

#include "arming.h"
#include "unitspec.h"
#include "unitvalue.h"

#include <QString>

#include <sys/types.h>

namespace Root::Operations
{

// ---------------------------------------------------------------------------
// define
// ---------------------------------------------------------------------------

struct DefineInput {
    uid_t ownerUid = 0;
    gid_t ownerGid = 0;
    QString unc;        ///< already validated by the caller
    QString mountPoint; ///< canonical, already validated
    QString unitName;   ///< re-derived by the caller via UnitValue::unitPathsFor()
    QString username;   ///< empty => guest

    /** The mount's access mode, and the one genuinely caller-chosen field
     *  here: the user picks it in the Add form, unlike everything the entry
     *  point fixes. Because there is no in-place Edit, this is the only
     *  moment it can be chosen, and it is recorded in the marker rather than
     *  in Store -- validation re-derives `Options=` from the marker, so a
     *  mode Store alone knew about would make the share Tampered.
     *
     *  The default is ReadWrite so that a caller which does not set it (an
     *  old front end calling a new helper across an upgrade) gets exactly
     *  today's behaviour. The helper still rejects a malformed value rather
     *  than defaulting it; only an *absent* one means read-write. */
    UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite;

    /** Authenticated shares only: written after both unit halves exist, so
     *  an interrupted define leaves discoverable unit state rather than an
     *  unindexed secret. Ignored for a guest share. */
    QString domain;
    QString password;

    /** The same MountpointPlan the caller already validated via
     *  UnitSpec::validateMountpoint() (plan.path must equal `mountPoint`) --
     *  needed for the immediate-arm path walk. */
    UnitSpec::MountpointPlan mountPlan;
};

struct DefineOutput {
    bool ok = false;
    QString shareId;
    QString error;
    /** Whether the share is now actually active. Add has no "armed after
     *  the next reboot" success state, so a successful define sets it. */
    bool activated = false;
};

/**
 * Fresh define, write-forward only (`Definition::None` only — an existing
 * Partial pair is never repaired; the caller must remove it first). Writes
 * the mount unit, then the automount unit; for an authenticated share, writes
 * the credential only once both halves exist; for a guest share, asserts no
 * credential artifact exists. The definition then arms immediately, as its
 * last step: on arm
 * failure, the new definition and credential are removed with checked
 * same-call compensation and the whole call reports failure, matching
 * "creation succeeds only with an active trigger and matching id."
 */
DefineOutput define(const DefineInput &input);

// ---------------------------------------------------------------------------
// undefine
// ---------------------------------------------------------------------------

struct RemovalInput {
    uid_t ownerUid = 0;
    gid_t ownerGid = 0;
    QString shareId;
    UnitValue::AuthenticationKind authentication;
    QString mountPoint;
    QString unitName;
    QString what;   ///< the .mount unit's own What=, for the correlation gate
};

struct RemovalOutput {
    bool ok = false;
    QString error;
};

/** undefine as a direct, checked removal: safely stops the live runtime,
 *  then removes the credential, both unit halves, and the automount-ID
 *  record. Every step is idempotent, so a failure partway simply leaves the
 *  remaining steps safe to retry. */
RemovalOutput remove(const RemovalInput &input);

// ---------------------------------------------------------------------------
// authenticated uninstall purge
// ---------------------------------------------------------------------------

struct PurgeOutput {
    bool ok = false;
    int removedShares = 0;
    QString error;
};

/** Removes all root-owned state created by nasmount for `ownerUid`. This is
 * deliberately all-or-nothing with respect to ownership: every managed
 * definition must be well formed and owned by the caller, and every live
 * instance must pass safeStop() before its artifacts are removed.
 * Mount-point directories are never removed. */
PurgeOutput purge(uid_t ownerUid);

} // namespace Root::Operations
