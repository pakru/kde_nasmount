/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "operations.h"
#include "arming.h"
#include "credentialstore.h"
#include "durablefs.h"
#include "runtimefiles.h"
#include "systemdops.h"
#include "unitspec.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator64>
#include <QSet>

#include <pwd.h>
#include <unistd.h>

namespace Root::Operations
{

namespace
{

constexpr qint64 MaxUnitBytes = 65536;

int openUnitsDir(QString *error)
{
    return DurableFs::openSystemRoot(QStringLiteral("/etc/systemd/system"), error);
}

bool writeUnitFile(int dirFd, const QString &fileName, const QString &content, QString *error)
{
    return DurableFs::durableReplace(dirFd, fileName, content.toUtf8(), DurableFs::ArtifactKind::UnitFile, error);
}

/** `allowMissing` is always true here: every removal in this module is
 *  idempotent by design, since a caller retrying after a
 *  partial failure does not know how far the previous attempt got. */
bool removeUnitFile(int dirFd, const QString &fileName, QString *error)
{
    return DurableFs::durableUnlink(dirFd, fileName, /*allowMissing=*/true, error);
}

bool readUnitFile(int dirFd, const QString &fileName, QByteArray *content, QString *error)
{
    return DurableFs::readFileBounded(dirFd, fileName, MaxUnitBytes, DurableFs::ArtifactKind::UnitFile, content,
                                      error);
}

/** Whether any managed marker anywhere, or a credential file in either
 *  the credential namespace, already claims `id`: generation retries until
 *  the value is absent from every managed unit marker and credential
 *  namespace. Deliberately global, not scoped to one uid -- ids must be
 *  unique across every user's shares. */
bool shareIdInUse(const QString &id)
{
    QString ignored;
    if (!CredentialStore::assertAbsent(id, &ignored)) {
        return true;
    }
    QDir dir(QStringLiteral("/etc/systemd/system"));
    const QStringList files = dir.entryList({QStringLiteral("*.mount"), QStringLiteral("*.automount")}, QDir::Files);
    for (const QString &name : files) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString content = QString::fromUtf8(f.readAll());
        UnitValue::Marker marker;
        QString markerError;
        if (UnitValue::parseMarker(content, &marker, &markerError) && marker.id == id) {
            return true;
        }
    }
    return false;
}

bool generateUniqueShareId(QString *id, QString *error)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const QString candidate = QStringLiteral("%1%2")
                                       .arg(QRandomGenerator64::global()->generate64(), 16, 16, QLatin1Char('0'))
                                       .arg(QRandomGenerator64::global()->generate64(), 16, 16, QLatin1Char('0'));
        if (!shareIdInUse(candidate)) {
            *id = candidate;
            return true;
        }
    }
    *error = QStringLiteral("cannot allocate a unique share id");
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// define
// ---------------------------------------------------------------------------

DefineOutput define(const DefineInput &input)
{
    DefineOutput out;

    QString shareId;
    if (!generateUniqueShareId(&shareId, &out.error)) {
        return out;
    }

    const UnitValue::AuthenticationKind authentication = input.username.isEmpty()
        ? UnitValue::AuthenticationKind::Guest
        : UnitValue::AuthenticationKind::Credentials;

    const QString mountFile = input.unitName + QStringLiteral(".mount");
    const QString automountFile = input.unitName + QStringLiteral(".automount");

    // A durableReplace() failure can occur after rename but before the parent
    // directory fsync completes. Track attempted artifacts, not only calls
    // that returned true, so same-call cleanup also covers that uncertain
    // installed-on-disk result.
    bool mountMayExist = false;
    bool automountMayExist = false;
    bool credentialMayExist = false;

    // Same-call, checked, best-effort compensation for exactly what this
    // call created -- never a persisted recovery record. A crash instead of a
    // same-process failure leaves whatever was durably written, visible on
    // the next inventory refresh as an owned Partial pair.
    auto cleanup = [&](const QString &reason) {
        out.error = reason;
        bool mayRemoveUnits = true;
        if (credentialMayExist) {
            QString credError;
            if (!CredentialStore::remove(shareId, /*allowMissing=*/true,
                                         &credError)) {
                out.error += QStringLiteral("; credential cleanup failed (%1); definition retained so the secret remains discoverable")
                                 .arg(credError);
                mayRemoveUnits = false;
            }
        }

        if (mayRemoveUnits && (mountMayExist || automountMayExist)) {
            QString dirError;
            const int cleanupFd = openUnitsDir(&dirError);
            if (cleanupFd < 0) {
                out.error += QStringLiteral("; cleanup could not open the unit directory (%1)").arg(dirError);
            } else {
                QString compError;
                if (automountMayExist && !removeUnitFile(cleanupFd, automountFile, &compError)) {
                    out.error += QStringLiteral("; cleanup failed (%1)").arg(compError);
                }
                if (mountMayExist && !removeUnitFile(cleanupFd, mountFile, &compError)) {
                    out.error += QStringLiteral("; cleanup failed (%1)").arg(compError);
                }
                ::close(cleanupFd);
            }
        }

        QString idError;
        if (!RuntimeFiles::removeAutomountId(input.unitName, &idError)) {
            out.error += QStringLiteral("; automount-id cleanup failed (%1)").arg(idError);
        }

        if (mayRemoveUnits && (mountMayExist || automountMayExist)) {
            QString reloadError;
            if (!SystemdOps::daemonReload(&reloadError)) {
                out.error += QStringLiteral("; cleanup daemon-reload failed (%1)").arg(reloadError);
            }
        }
    };

    const int sysFd = openUnitsDir(&out.error);
    if (sysFd < 0) {
        return out;
    }

    QByteArray existingMountBytes, existingAutomountBytes;
    const bool mountExists = readUnitFile(sysFd, mountFile, &existingMountBytes, &out.error);
    const bool automountExists = readUnitFile(sysFd, automountFile, &existingAutomountBytes, &out.error);
    out.error.clear();

    if (mountExists || automountExists) {
        ::close(sysFd);
        out.error = QStringLiteral("a definition already exists at this mount point");
        return out;
    }

    UnitValue::Marker marker;
    marker.ownerUid = input.ownerUid;
    marker.ownerGid = input.ownerGid;
    marker.id = shareId;
    marker.authentication = authentication;
    marker.access = input.access;

    QString mountContent, automountContent;
    if (!UnitSpec::buildMountUnitContent(marker, input.unc, input.mountPoint, &mountContent, &out.error)
        || !UnitSpec::buildAutomountUnitContent(marker, input.mountPoint, &automountContent, &out.error)) {
        ::close(sysFd);
        return out;
    }

    // Units first, then the credential: nasmount never reloads
    // or starts the new unit before the credential exists, so an interrupted
    // pre-credential crash is visible as owned unit state, discoverable and
    // removable, never an invisible orphaned secret.
    mountMayExist = true;
    if (!writeUnitFile(sysFd, mountFile, mountContent, &out.error)) {
        ::close(sysFd);
        cleanup(out.error);
        return out;
    }
    automountMayExist = true;
    if (!writeUnitFile(sysFd, automountFile, automountContent, &out.error)) {
        ::close(sysFd);
        cleanup(out.error);
        return out;
    }
    ::close(sysFd);

    if (authentication == UnitValue::AuthenticationKind::Credentials) {
        QString credErr;
        credentialMayExist = true;
        if (!CredentialStore::write(shareId, input.username, input.domain, input.password, &credErr)) {
            cleanup(credErr);
            return out;
        }
    } else {
        QString assertError;
        if (!CredentialStore::assertAbsent(shareId, &assertError)) {
            cleanup(assertError.isEmpty()
                        ? QStringLiteral("internal error: unexpected credential artifact for a guest share")
                        : assertError);
            return out;
        }
    }

    QString reloadError;
    if (!SystemdOps::daemonReload(&reloadError)) {
        cleanup(reloadError);
        return out;
    }

    // A definition arms immediately, as the last step: Add either produces an
    // armed share or reports why it could not.
    {
        Arming::ArmShareRequest armReq;
        armReq.ownerUid = input.ownerUid;
        armReq.ownerGid = input.ownerGid;
        armReq.shareId = shareId;
        armReq.authentication = authentication;
        armReq.plan = input.mountPlan;
        armReq.unitName = input.unitName;
        armReq.what = input.unc;
        armReq.pathPolicy = Arming::PathPolicy::InteractiveCreate;

        const Arming::ArmShareResult armResult = Arming::armShare(armReq);
        if (armResult.outcome == Arming::ArmShareOutcome::NeedsAttention) {
            if (!armResult.definitionCleanupSafe) {
                out.error = armResult.error
                    + QStringLiteral("; the new definition was retained because its started trigger could not be proven stopped");
                return out;
            }
            cleanup(armResult.error);
            return out;
        }
        out.activated = true; // Armed or AlreadyArmed both mean "active now"
    }

    out.ok = true;
    out.shareId = shareId;
    return out;
}

// ---------------------------------------------------------------------------
// remove (undefine)
// ---------------------------------------------------------------------------

RemovalOutput remove(const RemovalInput &input)
{
    RemovalOutput out;

    QString stopError;
    const Arming::StopResult result = Arming::safeStop(input.unitName, input.mountPoint, input.what, &stopError);
    if (result == Arming::StopResult::Busy || result == Arming::StopResult::CorrelationMismatch
        || result == Arming::StopResult::Indeterminate) {
        // Never claim full removal. Nothing destructive has
        // happened; retry once the runtime is no longer busy.
        out.error = stopError;
        return out;
    }

    QString credError;
    if (!CredentialStore::remove(input.shareId, /*allowMissing=*/true, &credError)) {
        out.error = credError;
        return out; // every remaining step is idempotent; retry repeats this one
    }

    const int sysFd = openUnitsDir(&out.error);
    if (sysFd < 0) {
        return out;
    }
    bool unlinkOk = removeUnitFile(sysFd, input.unitName + QStringLiteral(".mount"), &out.error);
    if (unlinkOk) {
        unlinkOk = removeUnitFile(sysFd, input.unitName + QStringLiteral(".automount"), &out.error);
    }
    ::close(sysFd);
    if (!unlinkOk) {
        return out;
    }
    if (!RuntimeFiles::removeAutomountId(input.unitName, &out.error)) {
        return out;
    }
    if (!SystemdOps::daemonReload(&out.error)) {
        return out;
    }

    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// authenticated uninstall purge
// ---------------------------------------------------------------------------

PurgeOutput purge(uid_t ownerUid)
{
    PurgeOutput out;

    const QList<Verify::OwnedUnit> units = Verify::enumerateManagedUnits();
    QSet<QString> validBases;
    for (const Verify::OwnedUnit &unit : units) {
        if (unit.state == Verify::Definition::Tampered) {
            out.error = QStringLiteral("cannot purge a tampered or colliding managed definition");
            return out;
        }
        if (unit.ownerUid != ownerUid) {
            out.error = QStringLiteral("cannot purge: managed shares belonging to another user exist");
            return out;
        }
        validBases.insert(unit.unitName);
    }

    // enumerateManagedUnits() intentionally ignores malformed marker blocks.
    // Purge cannot do that: an invalid managed-looking unit must stop the
    // uninstall rather than be silently left behind or unlinked by guessing.
    QDir unitDir(QStringLiteral("/etc/systemd/system"));
    const QStringList candidates =
        unitDir.entryList({QStringLiteral("*.mount"), QStringLiteral("*.automount")}, QDir::Files);
    for (const QString &name : candidates) {
        QFile file(unitDir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString content = QString::fromUtf8(file.readAll());
        UnitValue::Marker marker;
        QString markerError;
        if (UnitValue::hasMarker(content)) {
            const QString base = name.endsWith(QStringLiteral(".mount")) ? name.chopped(6) : name.chopped(10);
            if (!UnitValue::parseMarker(content, &marker, &markerError) || !validBases.contains(base)) {
                out.error = QStringLiteral("cannot purge malformed or unclassified managed unit %1").arg(name);
                return out;
            }
        }
    }

    struct ValidatedUnit {
        Verify::DefinitionCheck definition;
        UnitValue::UnitPaths paths;
    };
    QList<ValidatedUnit> validated;
    validated.reserve(units.size());
    for (const Verify::OwnedUnit &unit : units) {
        UnitValue::UnitPaths paths;
        QString pathError;
        if (!UnitValue::unitPathsFor(unit.mountPoint, &paths, &pathError) || paths.unitName != unit.unitName) {
            out.error = pathError.isEmpty() ? QStringLiteral("managed unit name does not match its mount point")
                                            : pathError;
            return out;
        }
        const Verify::DefinitionCheck def = Verify::inspectDefinition(paths, ownerUid, unit.mountPoint);
        if ((def.state != Verify::Definition::Pair && def.state != Verify::Definition::Partial)
            || def.id != unit.id || def.ownerUid != ownerUid || def.ownerGid != unit.ownerGid
            || def.authentication != unit.authentication) {
            out.error = QStringLiteral("managed definition changed during purge validation");
            return out;
        }
        validated.append({def, paths});
    }

    // Complete the safety gates for every definition before deleting any
    // definition or credential. A stop failure may leave earlier shares
    // inactive, but all artifacts remain available for a safe retry.
    for (const ValidatedUnit &unit : validated) {
        QString stopError;
        const Arming::StopResult stopped = Arming::safeStop(unit.paths.unitName,
                                                            unit.definition.canonicalMountPoint,
                                                            unit.definition.what, &stopError);
        if (stopped == Arming::StopResult::Busy || stopped == Arming::StopResult::CorrelationMismatch
            || stopped == Arming::StopResult::Indeterminate) {
            out.error = QStringLiteral("cannot safely stop %1: %2")
                            .arg(unit.definition.canonicalMountPoint, stopError);
            return out;
        }
    }

    const int sysFd = openUnitsDir(&out.error);
    if (sysFd < 0) {
        return out;
    }
    for (const ValidatedUnit &unit : validated) {
        if (!removeUnitFile(sysFd, unit.paths.unitName + QStringLiteral(".mount"), &out.error)
            || !removeUnitFile(sysFd, unit.paths.unitName + QStringLiteral(".automount"), &out.error)) {
            ::close(sysFd);
            return out;
        }
        if (!RuntimeFiles::removeAutomountId(unit.paths.unitName, &out.error)
            || !CredentialStore::remove(unit.definition.id, /*allowMissing=*/true,
                                        &out.error)) {
            ::close(sysFd);
            return out;
        }
        ++out.removedShares;
    }
    ::close(sysFd);
    if (!SystemdOps::daemonReload(&out.error)) {
        return out;
    }

    // Remove private roots last. durableRemoveTree() rejects symlinks,
    // directories at unexpected levels, foreign owners and drifted modes.
    // /run/nasmount-ids is a sibling of /run/nasmount, not nested inside it
    // (runtimefiles.h), so the two are independent, order-insensitive
    // removals — neither call needs the other's directory open first.
    const int etcFd = DurableFs::openSystemRoot(QStringLiteral("/etc"), &out.error);
    if (etcFd < 0) {
        return out;
    }
    if (!DurableFs::durableRemoveTree(etcFd, QStringLiteral("nasmount"), DurableFs::ArtifactKind::Directory,
                                      DurableFs::ArtifactKind::SensitiveFile, &out.error)) {
        ::close(etcFd);
        return out;
    }
    ::close(etcFd);

    const int runFd = DurableFs::openSystemRoot(QStringLiteral("/run"), &out.error);
    if (runFd < 0) {
        return out;
    }
    if (!DurableFs::durableRemoveTree(runFd, QStringLiteral("nasmount-ids"), DurableFs::ArtifactKind::PublicDirectory,
                                      DurableFs::ArtifactKind::PublicRecord, &out.error)) {
        ::close(runFd);
        return out;
    }
    if (!DurableFs::durableRemoveTree(runFd, QStringLiteral("nasmount"), DurableFs::ArtifactKind::Directory,
                                      DurableFs::ArtifactKind::SensitiveFile, &out.error)) {
        ::close(runFd);
        return out;
    }
    ::close(runFd);

    out.ok = true;
    return out;
}

} // namespace Root::Operations
