/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mountactions.h"
#include "helperinvoke.h"
#include "mountpointcheck.h"
#include "shareaddress.h"
#include "store.h"
#include "userlock.h"
#include "verify.h"

#include <QDir>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <memory>
#include <pwd.h>
#include <unistd.h>

using Session::HelperOutcome;
using Session::HelperResult;
using Session::UserLock;

namespace
{

/**
 * What the worker thread hands back to the GUI-thread continuation, which
 * only ever Q_EMITs finished() from it: the per-user lock is
 * acquired first thing on the worker thread and held across the Store
 * snapshot read, the helper call and the checked Store commit, all of
 * which now happen on the worker thread too - none of it belongs on the GUI
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
 * ConfirmedSuccess, the helper's own error text on ConfirmedFailure, and -
 * for Unknown - text that says plainly the result could not be confirmed
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
        return QStringLiteral("could not confirm the result (%1) - refresh before retrying")
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

QString localPathFromUrl(const QUrl &url)
{
    // A file URL with a host names a path on another machine; toLocalFile()
    // would render it as a //host/path string, which is not a folder here.
    if (!url.isLocalFile() || !url.host().isEmpty()) {
        return QString();
    }
    return url.toLocalFile();
}

MountActions::MountActions(QObject *parent)
    : QObject(parent)
    , m_uid(::getuid())
{
    // The passwd home, not $HOME or QDir::homePath(): it is what the helper
    // authorizes the mount point against (it reads the caller's uid), and a
    // different value here would accept paths the helper then refuses.
    if (const struct passwd *pw = ::getpwuid(m_uid)) {
        m_homeDir = QString::fromLocal8Bit(pw->pw_dir);
    }
    m_checkPool.setMaxThreadCount(1);
}

void MountActions::addShare(const QString &shareInput, const QString &rawMountPoint, const QString &username,
                            const QString &domain, const QString &password, const QString &access)
{
    const QString kind = QStringLiteral("add");
    // The stored record must hold the *same* canonical path the helper
    // derives, because that is what every later check compares against. The
    // helper runs QDir::cleanPath() on whatever it is given and writes the
    // result as Where=; storing the user's raw text instead means a mount
    // point typed with a trailing slash (or a "//" or "/./") is written as one
    // string and compared as another, and a perfectly good share is stuck at
    // NeedsAttention forever with every action refused. Normalising once here
    // covers both front ends, since both go through MountActions - and the
    // live check in the form uses the same function, so it cannot disagree.
    const QString mountPoint = canonicalMountPoint(rawMountPoint, m_homeDir);
    Q_EMIT started(QString(), kind);

    // First, before the helper call *and* the Store commit: Store's UNC is
    // re-validated as //host/share by the drift comparison, so an smb://
    // value reaching it would make every new share look like drift.
    QString unc;
    QString addressError;
    if (!ShareAddress::resolveShareInput(shareInput, username, &unc, &addressError)) {
        Q_EMIT finished(QString(), kind, false, addressError);
        return;
    }

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
                            "the definition was created, but could not be saved locally (%1) - it exists on this "
                            "machine but will not appear here until this is resolved")
                            .arg(commitError);
            return r;
        }

        r.success = true;
        r.message = defineResult.activated
            ? QStringLiteral("Share successfuly added and mounted")
            : QStringLiteral("Share added, but could not be mounted - check setting for more info"); // TODO improve this message
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
            // the Store record - an unknown removal that actually succeeded
            // would otherwise leave a root definition with no local record
            // pointing at it.
            r.message = QStringLiteral("could not fully remove %1: %2%3")
                            .arg(share.mountPoint,
                                 describeOutcome(undefineResult.outcome, undefineResult.message, QString()),  QStringLiteral(" - retry removal later"));
            return r;
        }
        r.success = Store::removeShare(id);
        r.message = r.success ? QStringLiteral("Removed")
                              : QStringLiteral("removed the definition, but the local record could not be fully "
                                               "cleared - retry from this list");
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
                              : QStringLiteral("could not confirm local-record cleanup - retry");
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

QString MountActions::displayUrl(const QString &unc) const
{
    return ShareAddress::displayUrl(unc);
}

QString MountActions::userInShareInput(const QString &text) const
{
    return ShareAddress::userInShareInput(text);
}

QString MountActions::localPathFromUrl(const QUrl &url) const
{
    return Session::localPathFromUrl(url);
}

QString MountActions::shareInputProblem(const QString &input, const QString &username) const
{
    return ShareAddress::shareInputProblem(input, username);
}

QString MountActions::mountPointProblem(const QString &raw) const
{
    return Session::mountPointProblem(raw, m_homeDir);
}

void MountActions::checkMountPoint(const QString &raw)
{
    if (m_checkRunning) {
        m_checkPending = true;
        m_pendingRaw = raw;
        return;
    }
    startMountPointCheck(raw);
}

void MountActions::startMountPointCheck(const QString &raw)
{
    m_checkRunning = true;
    const QString home = m_homeDir;
    const uid_t uid = m_uid;
    auto future = QtConcurrent::run(&m_checkPool, [raw, home, uid]() {
        return Session::mountPointFsProblem(raw, home, uid);
    });

    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, raw]() {
        const QString problem = watcher->future().result();
        watcher->deleteLater();
        // Unblocked and the waiting request taken before the signal goes out:
        // a receiver that asks for another check from its slot starts one
        // normally instead of finding the slot still occupied.
        m_checkRunning = false;
        const bool again = m_checkPending;
        const QString next = m_pendingRaw;
        m_checkPending = false;
        Q_EMIT mountPointChecked(raw, problem);
        if (again) {
            checkMountPoint(next);
        }
    });
    watcher->setFuture(future);
}

QVariantMap MountActions::browsedShareInput(const QUrl &picked) const
{
    const ShareAddress::BrowsedShare browsed = ShareAddress::browsedShareInput(picked);
    return {{QStringLiteral("input"), browsed.input},
            {QStringLiteral("user"), browsed.user},
            {QStringLiteral("error"), browsed.error}};
}

QUrl MountActions::browseStartUrl(const QString &shareInput) const
{
    return ShareAddress::browseStartUrl(shareInput);
}

QString MountActions::lookupTargetOf(const QString &shareInput) const
{
    // No Username to check against: this asks which share the text names, and
    // a text that names a user counts as unusable rather than as some share.
    QString unc;
    QString error;
    if (!ShareAddress::resolveShareInput(shareInput, QString(), &unc, &error)) {
        return QString();
    }
    const QUrl target = ShareAddress::authLookupTarget(unc);
    return target.isValid() ? target.toString(QUrl::FullyEncoded) : QString();
}

} // namespace Session
