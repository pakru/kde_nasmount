/*
 * arming — the shared arm and safe-stop routines every privileged caller
 * that starts or stops a share's automount goes through.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "The automount reports active" and "this is the instance we are allowed
 * to act on" are different claims. Every stop in this project
 * goes through safeStop(), which proves the second claim via the recorded
 * instance id before touching anything; every start goes through armShare(),
 * which never returns success without that id durably recorded, and rolls
 * itself back rather than leave an active-but-unrecorded ("blessed")
 * trigger if recording fails.
 *
 * There is no durable transaction manifest here: a same-process failure is
 * compensated inline (stop what this call started, remove what this call
 * wrote); a crash or kill leaves whatever was durably written, visible on
 * the next inventory refresh. An active trigger with no matching recorded
 * id is never adopted or stopped automatically regardless of how it got
 * there — that gate does not depend on recovery.
 */

#pragma once

#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <QString>

#include <sys/types.h>

namespace Root::Arming
{

/**
 * The structured result of an attempted stop, rather than a
 * bare bool: callers that must distinguish "nothing to do" from "could not
 * prove it was safe" from "the process itself failed" make different
 * decisions.
 */
enum class StopResult { Stopped, AlreadyInactive, Busy, CorrelationMismatch, Indeterminate };

/** Pure runtime-correlation decision used by safeStop(), exposed for the
 *  complete fail-closed table in arming_test. */
enum class StopPrecheck { ShouldStop, AlreadyInactive, CorrelationMismatch, Indeterminate };
StopPrecheck evaluateStopPrecheck(const Verify::RuntimeSnapshot &snapshot, QString *error);

/**
 * The correlation gate plus the actual stop, as one
 * operation: computes the runtime snapshot for `unitName`/`mountPoint`, and
 * — only if the mount is verified `Match`, or the automount is `Inactive`,
 * or the automount is `Active` with `ActivationTrust::Trusted` — stops both
 * halves. Never stops a unit whose active instance cannot be proven to be
 * the one this tool recorded.
 */
StopResult safeStop(const QString &unitName, const QString &mountPoint, const QString &expectedWhat, QString *error);

// ---------------------------------------------------------------------------
// Shared single-share arming — used by
// definesystem's immediate arm and by nasmount-boot, so both
// implement the exact same idempotency and path-safety rules. Never writes a
// credential (an authenticated share's was durably written by definesystem before
// this ever runs; a guest share never has one) — only validates it.
// ---------------------------------------------------------------------------

/** How the mount point may be touched while arming. An
 *  interactive Add may create it exactly like
 *  UnitSpec::openMountpointNoFollow(); nasmount-boot must never create or
 *  chown anything and instead verifies whatever already exists via
 *  UnitSpec::openMountpointNoCreate(). */
enum class PathPolicy { InteractiveCreate, BootNoCreate };

enum class ArmPrecheck { ReadyToArm, AlreadyArmed, Blocked };

/**
 * Pure decision table over an already-computed runtime snapshot, so runtime
 * is inspected before the path is touched at all: `Blocked` for
 * Indeterminate automount/mount state, a live mount already occupying the
 * path, or an active automount whose instance id does not match what was
 * recorded (never "bless" it); `AlreadyArmed` only when the automount is
 * Active *and* its ActivationTrust is Trusted; `ReadyToArm` otherwise
 * (Inactive automount, nothing mounted). Exposed for testing without
 * systemctl/mountinfo access; production code reaches this only through
 * armShare(), which computes the snapshot via Verify::inspectRuntime() first.
 */
ArmPrecheck evaluateArmPrecheck(const Verify::RuntimeSnapshot &snapshot, QString *error);

enum class ArmShareOutcome {
    Armed,          ///< freshly started; its instance id is durably recorded
    AlreadyArmed,   ///< idempotent no-op: the recorded id already matches an active instance
    NeedsAttention, ///< failed closed for this one share; inspect definitionCleanupSafe before compensating
};

struct ArmShareRequest {
    uid_t ownerUid = 0;
    gid_t ownerGid = 0;
    QString shareId;
    UnitValue::AuthenticationKind authentication = UnitValue::AuthenticationKind::Credentials;
    UnitSpec::MountpointPlan plan; ///< plan.path is this share's canonical mount point
    QString unitName;
    QString what; ///< the .mount unit's own What=, for the correlation gate
    PathPolicy pathPolicy = PathPolicy::BootNoCreate;
};

struct ArmShareResult {
    ArmShareOutcome outcome = ArmShareOutcome::NeedsAttention;
    QString error; ///< set only for NeedsAttention
    /** Whether a caller that just created this definition may remove its unit
     *  files and credential. False means a newly started trigger could not be
     *  proven stopped, so the complete definition must remain visible for
     *  refresh/administrator repair. */
    bool definitionCleanupSafe = true;
};

/**
 * Arms one share, standalone — used
 * by `definesystem`'s immediate arm and by nasmount-boot for each share it
 * enumerates. Inspects runtime *before* touching the path: a
 * recorded instance id that already matches an active automount is an
 * idempotent no-op that touches nothing further; an active-but-unrecorded
 * instance, or a live mount that does not correlate, fails closed without
 * starting or recording anything. Only when the automount is inactive and
 * nothing is mounted does it validate the credential (healthy for
 * Credentials, absent for Guest — never write one), walk the path per
 * `req.pathPolicy`, start the automount, and durably record its instance id.
 * Any failure after the automount is actually started stops that exact
 * trigger before returning, so a caller's own unit-file/credential cleanup
 * (which this never performs) never has to reason about a still-active
 * automount referencing units it is removing.
 */
ArmShareResult armShare(const ArmShareRequest &req);

} // namespace Root::Arming
