/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mountactions.h"
#include "helperinvoke.h"
#include "store.h"
#include "userlock.h"
#include "verify.h"

#include <QDir>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <memory>
#include <unistd.h>

using Session::HelperOutcome;
using Session::HelperResult;
using Session::UserLock;

namespace
{

/**
 * The stored record must hold the *same* canonical path the helper derives,
 * because that is what every later check compares against.
 *
 * The helper runs QDir::cleanPath() on whatever it is given and writes the
 * result as Where=. Storing the user's raw text instead means a mount point
 * typed with a trailing slash (or a "//" or "/./") is written as one string
 * and compared as another: inspectDefinition's "effective Where= agrees with
 * the canonical mount point" rule then fails, and a perfectly good share is
 * stuck at NeedsAttention forever with every action refused. Normalising once
 * here covers both frontends, since both go through MountActions.
 */
QString canonicalMountPoint(const QString &raw)
{
    return QDir::cleanPath(raw.trimmed());
}

/**
 * What the worker thread hands back to the GUI-thread continuation, which
 * only ever Q_EMITs finished() from it: the per-user lock is
 * acquired first thing on the worker thread and held across the Store
 * snapshot read, the helper call and the checked Store commit, all of
 * which now happen on the worker thread too — none of it belongs on the GUI
 * thread, and the lock must cover all of it, not just the KAuth call.
 */
struct WorkResult {
    bool success = false;
    QString id; ///< unchanged from the input id, except addShare's newly assigned one
    QString message;
    std::shared_ptr<UserLock> lock;
};

/**
 * Renders one helper outcome for display: `successMessage` on
 * ConfirmedSuccess, the helper's own error text on ConfirmedFailure, and —
 * for Unknown — text that says plainly the result could not be confirmed
 * rather than guessing at either success or failure.
 */
QString describeOutcome(HelperOutcome outcome, const QString &detail, const QString &successMessage)
{
    switch (outcome) {
    case HelperOutcome::ConfirmedSuccess:
        return successMessage;
    case HelperOutcome::ConfirmedFailure:
        return detail;
    case HelperOutcome::Unknown:
        return QStringLiteral("could not confirm the result (%1) — refresh before retrying")
            .arg(detail.isEmpty() ? QStringLiteral("connection to the helper was lost") : detail);
    }
    return detail;
}

} // namespace

namespace Session
{

bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password)
{
    return !username.isEmpty() || (domain.isEmpty() && password.isEmpty());
}

MountActions::MountActions(QObject *parent) : QObject(parent) { }

void MountActions::addShare(const QString &unc, const QString &rawMountPoint, const QString &username,
                            const QString &domain, const QString &password, const QString &access)
{
    const QString kind = QStringLiteral("add");
    const QString mountPoint = canonicalMountPoint(rawMountPoint);
    Q_EMIT started(QString(), kind);

    if (!guestFieldsConsistent(username, domain, password)) {
        Q_EMIT finished(QString(), kind, false,
                        QStringLiteral("a share with no username is guest, and cannot also have a password or "
                                       "domain"));
        return;
    }

    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }

        const HelperResult defineResult = invokeHelperAction(
            QStringLiteral("definesystem"),
            {{QStringLiteral("unc"), unc}, {QStringLiteral("path"), mountPoint},
             {QStringLiteral("username"), username}, {QStringLiteral("domain"), domain},
             {QStringLiteral("password"), password}, {QStringLiteral("access"), access}});
        if (defineResult.outcome != HelperOutcome::ConfirmedSuccess) {
            r.message = describeOutcome(defineResult.outcome, defineResult.message, QString());
            return r;
        }
        if (!UnitValue::isValidShareId(defineResult.id)) {
            r.message = QStringLiteral("the helper returned an invalid share id");
            return r;
        }
        r.id = defineResult.id;

        // definesystem arms immediately, as its own last step: there is no
        // separate arm step to call here, and no secret
        // to store locally -- the credential is the helper's root-owned file.
        Store::Share share;
        share.id = r.id;
        share.unc = unc;
        share.mountPoint = mountPoint;
        share.username = username;
        share.domain = domain;
        // Convenience data only -- the marker stays authoritative, and
        // MountModel reports a disagreement between the two as drift rather
        // than trusting this copy. Recorded so the list can be rendered
        // without re-reading a unit file per row.
        share.access = access.isEmpty() ? UnitValue::accessModeToString(UnitValue::AccessMode::ReadWrite)
                                        : access;
        QString commitError;
        if (Store::commitShare(share, /*expectedGeneration=*/0, &commitError) != Store::CommitResult::Ok) {
            r.message = QStringLiteral(
                            "the definition was created, but could not be saved locally (%1) — it exists on this "
                            "machine but will not appear here until this is resolved")
                            .arg(commitError);
            return r;
        }

        r.success = true;
        r.message = defineResult.activated
            ? QStringLiteral("Share successfuly added and mounted")
            : QStringLiteral("Share added, but could not be mounted — check setting for more info"); // TODO improve this message
        return r;
    });

    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::deleteShare(const QString &id)
{
    const QString kind = QStringLiteral("delete");
    Q_EMIT started(id, kind);

    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }

        const Store::Snapshot snap = Store::snapshotById(id);
        if (!snap.exists) {
            r.message = QStringLiteral("no such share");
            return r;
        }
        const Store::Share &share = snap.share;
        const HelperResult undefineResult = invokeHelperAction(
            QStringLiteral("undefinesystem"), {{QStringLiteral("path"), share.mountPoint}});
        if (undefineResult.outcome != HelperOutcome::ConfirmedSuccess) {
            // Neither a confirmed failure nor an unknown result may remove
            // the Store record — an unknown removal that actually succeeded
            // would otherwise leave a root definition with no local record
            // pointing at it.
            r.message = QStringLiteral("could not fully remove %1: %2%3")
                            .arg(share.mountPoint,
                                 describeOutcome(undefineResult.outcome, undefineResult.message, QString()),  QStringLiteral(" — retry removal later"));
            return r;
        }
        r.success = Store::removeShare(id);
        r.message = r.success ? QStringLiteral("Removed")
                              : QStringLiteral("removed the definition, but the local record could not be fully "
                                               "cleared — retry from this list");
        return r;
    });

    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::removeOrphanedRecord(const QString &id)
{
    const QString kind = QStringLiteral("removeRecord");
    Q_EMIT started(id, kind);
    // No helper call: Definition::None means there is nothing on the
    // privileged side for it to act on. It is still a Store mutation, so it
    // follows the same worker-thread/UserLock rule as every other client
    // operation.
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        r.id = id;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        r.success = Store::removeShare(id);
        r.message = r.success ? QStringLiteral("Removed")
                              : QStringLiteral("could not confirm local-record cleanup — retry");
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}

void MountActions::removeOrphanByPath(const QString &mountPoint)
{
    const QString kind = QStringLiteral("removeOrphan");
    Q_EMIT started(QString(), kind);
    auto future = QtConcurrent::run([=]() -> WorkResult {
        WorkResult r;
        QString lockError;
        r.lock = UserLock::acquire(&lockError);
        if (!r.lock) {
            r.message = lockError;
            return r;
        }
        UnitValue::UnitPaths paths;
        QString pathsError;
        if (!UnitValue::unitPathsFor(mountPoint, &paths, &pathsError)) {
            r.message = pathsError;
            return r;
        }
        const auto def = Verify::inspectDefinition(paths, ::getuid(), mountPoint);
        if (def.state != Verify::Definition::Pair && def.state != Verify::Definition::Partial) {
            r.message = QStringLiteral("no owned definition exists at %1").arg(mountPoint);
            return r;
        }
        const HelperResult result = invokeHelperAction(QStringLiteral("undefinesystem"),
                                                       {{QStringLiteral("path"), mountPoint}});
        r.success = (result.outcome == HelperOutcome::ConfirmedSuccess);
        r.message = describeOutcome(result.outcome, result.message, QStringLiteral("Removed"));
        return r;
    });
    auto *watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kind]() {
        const WorkResult r = watcher->future().result();
        watcher->deleteLater();
        Q_EMIT finished(r.id, kind, r.success, r.message);
    });
    watcher->setFuture(future);
}


} // namespace Session
