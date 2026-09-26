/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "store.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QFile>
#include <QStandardPaths>

namespace
{

const QLatin1String ConfigName("nasmountrc");
const QLatin1String SharesGroup("Shares");

/** Reads one share group's fields, whatever is present. Does not decide
 *  corrupt/exists -- the caller does, since "exists" also depends on whether
 *  the group is present at all. */
Store::Share readShareFields(const KConfigGroup &g, const QString &id)
{
    Store::Share s;
    s.id = id;
    s.mountPoint = g.readEntry("MountPoint", QString());
    s.unc = g.readEntry("Unc", QString());
    s.username = g.readEntry("Username", QString());
    s.domain = g.readEntry("Domain", QString());
    // Every record written before 0.1.4 predates the access-mode feature and
    // has no Access key. Those shares are read-write, so that is what a
    // missing key means -- not a corrupt record, and deliberately not part of
    // the corrupt-group check below.
    s.access = g.readEntry("Access", QStringLiteral("readwrite"));
    return s;
}

void writeShareFields(KConfigGroup &g, const Store::Share &share)
{
    g.writeEntry("MountPoint", share.mountPoint);
    g.writeEntry("Unc", share.unc);
    g.writeEntry("Username", share.username);
    g.writeEntry("Domain", share.domain);
    // Normalised on write so an empty value can never reach the file. An
    // empty Access entry is *present* as far as KConfig is concerned, so
    // readShareFields()'s default would not apply to it on the way back, and
    // the record would then read as neither a valid mode nor a pre-0.1.4
    // record -- and be reported as drift against a perfectly good marker.
    g.writeEntry("Access",
                 share.access.isEmpty() ? QStringLiteral("readwrite") : share.access);
}

Store::Snapshot readSnapshot(const KConfigGroup &root, const QString &id)
{
    Store::Snapshot snap;
    if (!root.hasGroup(id)) {
        return snap;
    }
    const KConfigGroup g = root.group(id);
    snap.share = readShareFields(g, id);
    snap.generation = static_cast<quint64>(g.readEntry("Generation", 0LL));
    snap.exists = true;
    snap.corrupt = snap.share.unc.isEmpty() || snap.share.mountPoint.isEmpty();
    return snap;
}

} // namespace

namespace Store
{

QList<Snapshot> shareSnapshots()
{
    QList<Snapshot> result;
    auto config = KSharedConfig::openConfig(ConfigName);
    config->reparseConfiguration();
    const KConfigGroup root = config->group(SharesGroup);
    const QStringList groups = root.groupList();
    result.reserve(groups.size());
    for (const QString &id : groups) {
        result.append(readSnapshot(root, id));
    }
    return result;
}

QList<Share> shares()
{
    QList<Share> result;
    for (const Snapshot &snap : shareSnapshots()) {
        if (!snap.corrupt) {
            result.append(snap.share);
        }
    }
    return result;
}

Snapshot snapshotById(const QString &id)
{
    auto config = KSharedConfig::openConfig(ConfigName);
    config->reparseConfiguration();
    const KConfigGroup root = config->group(SharesGroup);
    return readSnapshot(root, id);
}

CommitResult commitShare(const Share &share, quint64 expectedGeneration, QString *error)
{
    Q_ASSERT(!share.id.isEmpty());
    auto config = KSharedConfig::openConfig(ConfigName);
    // Re-read immediately before the compare, not the caller's possibly-stale
    // in-memory snapshot: another cooperating process may have committed
    // since this caller's own read. This check is meaningful
    // because the caller is expected to hold Session::UserLock across both;
    // it catches a lock-discipline bug, it is not a substitute for the lock.
    config->reparseConfiguration();
    const KConfigGroup root = config->group(SharesGroup);
    const Snapshot current = readSnapshot(root, share.id);
    if (current.exists != (expectedGeneration != 0) || (current.exists && current.generation != expectedGeneration)) {
        *error = QStringLiteral("share record changed since it was read; reload and retry");
        return CommitResult::GenerationConflict;
    }

    KConfigGroup mutableRoot = config->group(SharesGroup);
    KConfigGroup g = mutableRoot.group(share.id);
    writeShareFields(g, share);
    g.writeEntry("Generation", static_cast<qlonglong>(expectedGeneration + 1));
    if (!config->sync()) {
        *error = QStringLiteral("could not write configuration to disk");
        return CommitResult::SyncFailed;
    }
    return CommitResult::Ok;
}

bool removeShare(const QString &id)
{
    // Nothing secret lives here: the share's credential is a root-owned file
    // the helper removes when it undefines the definition, so this only
    // clears the local convenience record.
    auto config = KSharedConfig::openConfig(ConfigName);
    config->reparseConfiguration();
    KConfigGroup root = config->group(SharesGroup);
    if (root.hasGroup(id)) {
        root.deleteGroup(id);
        if (!config->sync()) {
            return false;
        }
    }
    return true;
}

bool purgeApplicationData(QString *error)
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (configDir.isEmpty()) {
        *error = QStringLiteral("the user configuration directory is unavailable");
        return false;
    }
    const QString configPath = configDir + QStringLiteral("/nasmountrc");
    if (QFile::exists(configPath) && !QFile::remove(configPath)) {
        *error = QStringLiteral("could not remove %1").arg(configPath);
        return false;
    }
    return true;
}

} // namespace Store
