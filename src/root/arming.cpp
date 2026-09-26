/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arming.h"
#include "credentialstore.h"
#include "runtimefiles.h"
#include "systemdops.h"
#include "unitspec.h"
#include "verify.h"

#include <unistd.h>

namespace Root::Arming
{

namespace
{

/** Stops only the exact automount instance observed immediately after this
 *  call's successful start. A missing/mismatched id is not authority to stop
 *  a unit by name: leave the definition intact so the active-untrusted state
 *  remains visible instead. */
bool stopStartedAutomount(const QString &automountUnit, const QString &unitName,
                          const QString &mountPoint, uint64_t startedId, QString *error)
{
    if (startedId == 0) {
        *error = QStringLiteral("could not identify the automount instance started by this call");
        return false;
    }
    const uint64_t currentId = Verify::uniqueMountId(mountPoint);
    if (currentId == 0 || currentId != startedId) {
        *error = QStringLiteral("the started automount instance changed before compensation");
        return false;
    }
    if (!SystemdOps::stop(automountUnit, error)) {
        return false;
    }

    // The trigger is confirmed stopped, so a stale record is no longer an
    // authority problem. Remove it here and let a defining caller retry the
    // same checked cleanup if this unlink itself fails.
    QString idError;
    if (!RuntimeFiles::removeAutomountId(unitName, &idError)) {
        *error = QStringLiteral("trigger stopped, but automount-id cleanup failed (%1)").arg(idError);
    }
    return true;
}

} // namespace

StopResult safeStop(const QString &unitName, const QString &mountPoint, const QString &expectedWhat, QString *error)
{
    const Verify::RuntimeSnapshot snap = Verify::inspectRuntime(unitName, mountPoint, expectedWhat);
    const StopPrecheck precheck = evaluateStopPrecheck(snap, error);
    if (precheck == StopPrecheck::Indeterminate) {
        return StopResult::Indeterminate;
    }
    if (precheck == StopPrecheck::CorrelationMismatch) {
        return StopResult::CorrelationMismatch;
    }
    if (precheck == StopPrecheck::AlreadyInactive) {
        return StopResult::AlreadyInactive;
    }

    QString mountError, automountError;
    const bool mountOk = SystemdOps::stop(unitName + QStringLiteral(".mount"), &mountError);
    const bool automountOk = SystemdOps::stop(unitName + QStringLiteral(".automount"), &automountError);
    if (!mountOk || !automountOk) {
        *error = mountOk ? automountError : mountError;
        return StopResult::Busy;
    }
    return StopResult::Stopped;
}

StopPrecheck evaluateStopPrecheck(const Verify::RuntimeSnapshot &snap, QString *error)
{
    if (snap.mount == Verify::MountState::Indeterminate || snap.automount == Verify::AutomountState::Indeterminate) {
        *error = QStringLiteral("runtime state could not be determined");
        return StopPrecheck::Indeterminate;
    }
    if (snap.mount == Verify::MountState::Present) {
        if (snap.verification != Verify::VerificationState::Match) {
            *error = QStringLiteral("the mount does not correlate with this definition (best-effort check failed)");
            return StopPrecheck::CorrelationMismatch;
        }
    } else if (snap.automount == Verify::AutomountState::Active) {
        if (snap.activationTrust != Verify::ActivationTrust::Trusted) {
            *error = QStringLiteral("the armed automount is not the instance this tool recorded");
            return StopPrecheck::CorrelationMismatch;
        }
    } else {
        return StopPrecheck::AlreadyInactive;
    }
    return StopPrecheck::ShouldStop;
}

ArmPrecheck evaluateArmPrecheck(const Verify::RuntimeSnapshot &snapshot, QString *error)
{
    if (snapshot.mount == Verify::MountState::Indeterminate
        || snapshot.automount == Verify::AutomountState::Indeterminate) {
        *error = QStringLiteral("runtime state could not be determined");
        return ArmPrecheck::Blocked;
    }
    if (snapshot.mount == Verify::MountState::Present) {
        *error = QStringLiteral("a live mount already occupies the path");
        return ArmPrecheck::Blocked;
    }
    if (snapshot.automount == Verify::AutomountState::Active) {
        if (snapshot.activationTrust == Verify::ActivationTrust::Trusted) {
            return ArmPrecheck::AlreadyArmed;
        }
        *error = QStringLiteral("an active automount has no matching recorded instance -- refusing to bless it");
        return ArmPrecheck::Blocked;
    }
    return ArmPrecheck::ReadyToArm;
}

ArmShareResult armShare(const ArmShareRequest &req)
{
    ArmShareResult result;
    const QString automountUnit = req.unitName + QStringLiteral(".automount");
    bool startedByUs = false;
    uint64_t startedId = 0;

    auto fail = [&](const QString &message) {
        QString finalMessage = message;
        if (startedByUs) {
            QString stopError;
            if (!stopStartedAutomount(automountUnit, req.unitName, req.plan.path, startedId, &stopError)) {
                result.definitionCleanupSafe = false;
            }
            if (!stopError.isEmpty()) {
                finalMessage += QStringLiteral("; compensation failed (%1)").arg(stopError);
            }
        }
        result.outcome = ArmShareOutcome::NeedsAttention;
        result.error = finalMessage;
        return result;
    };

    // Inspect runtime *before* touching the path at all: a live mount or an
    // untrusted trigger there must block arming, not be walked into.
    const Verify::RuntimeSnapshot snapshot = Verify::inspectRuntime(req.unitName, req.plan.path, req.what);
    QString precheckError;
    const ArmPrecheck pre = evaluateArmPrecheck(snapshot, &precheckError);
    if (pre == ArmPrecheck::Blocked) {
        return fail(precheckError);
    }
    if (pre == ArmPrecheck::AlreadyArmed) {
        result.outcome = ArmShareOutcome::AlreadyArmed;
        return result;
    }

    // Credential health, never a write (the credential was already
    // durably written by define(), after both unit files were created and
    // before systemd was reloaded).
    QString credError;
    if (req.authentication == UnitValue::AuthenticationKind::Credentials) {
        if (!CredentialStore::healthy(req.shareId, &credError)) {
            return fail(credError.isEmpty() ? QStringLiteral("credential is missing or unhealthy") : credError);
        }
    } else if (!CredentialStore::assertAbsent(req.shareId, &credError)) {
        return fail(credError.isEmpty()
                        ? QStringLiteral("internal error: unexpected credential artifact for a guest share")
                        : credError);
    }

    QString walkError;
    const int mpFd = (req.pathPolicy == PathPolicy::InteractiveCreate)
        ? UnitSpec::openMountpointNoFollow(req.plan, req.ownerUid, req.ownerGid, &walkError)
        : UnitSpec::openMountpointNoCreate(req.plan, req.ownerUid, req.ownerGid, &walkError);
    if (mpFd < 0) {
        return fail(walkError);
    }
    ::close(mpFd);

    QString startError;
    if (!SystemdOps::start(automountUnit, &startError)) {
        // A checked command failure does not by itself prove that PID 1 left
        // the unit inactive. Re-inspect: if an automount appeared, capture its
        // identity and compensate it exactly; otherwise forbid definition
        // cleanup whenever runtime is not conclusively inactive.
        const Verify::RuntimeSnapshot after = Verify::inspectRuntime(req.unitName, req.plan.path, req.what);
        if (after.automount == Verify::AutomountState::Active && after.mount == Verify::MountState::Absent) {
            startedByUs = true;
            startedId = Verify::uniqueMountId(req.plan.path);
        } else if (after.automount != Verify::AutomountState::Inactive
                   || after.mount != Verify::MountState::Absent) {
            result.definitionCleanupSafe = false;
        }
        return fail(startError);
    }
    startedByUs = true;

    const uint64_t id = Verify::uniqueMountId(req.plan.path);
    startedId = id;
    QString idError;
    if (id == 0 || !RuntimeFiles::writeAutomountId(req.unitName, id, &idError)) {
        return fail(idError.isEmpty() ? QStringLiteral("kernel does not support recording the instance id")
                                      : idError);
    }

    result.outcome = ArmShareOutcome::Armed;
    return result;
}

} // namespace Root::Arming
