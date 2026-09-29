/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mountpointcheck.h"
#include "unitvalue.h"

#include <QDir>
#include <QFile>

namespace
{

/** Sentence case: the helper's messages are fragments ("mount point must be
 *  ..."), and shown on a line of their own they start with a capital. */
QString capitalised(QString message)
{
    if (!message.isEmpty()) {
        message[0] = message.at(0).toUpper();
    }
    return message;
}

} // namespace

namespace Session
{

QString canonicalMountPoint(const QString &raw, const QString &homeDir)
{
    QString path = raw.trimmed();
    const bool ownHome = path == QLatin1String("~") || path.startsWith(QLatin1String("~/"));
    if (ownHome && !homeDir.isEmpty()) {
        path = homeDir + path.mid(1);
    }
    return QDir::cleanPath(path);
}

QString mountPointProblem(const QString &raw, const QString &homeDir)
{
    if (raw.trimmed().isEmpty()) {
        return QStringLiteral("Choose a folder to mount the share on");
    }

    const QString canonical = canonicalMountPoint(raw, homeDir);
    // Still starting with '~' after expansion means it was ~name (or the home
    // directory is unknown): a relative path the helper would refuse, with a
    // message about absolute paths that would not say what went wrong.
    if (canonical.startsWith(QLatin1Char('~'))) {
        return QStringLiteral("Only ~ for your own home folder is supported; enter the full path");
    }

    UnitSpec::MountpointPlan plan;
    QString error;
    if (!UnitSpec::validateMountpoint(canonical, homeDir, &plan, &error)) {
        return capitalised(error);
    }
    return QString();
}

QString unitCollisionProblem(const UnitSpec::MountpointPlan &plan, const UnitFileExists &unitFileExists)
{
    auto hasUnits = [&](const QString &path) {
        UnitValue::UnitPaths paths;
        QString error;
        if (!UnitValue::unitPathsFor(path, &paths, &error)) {
            return false; // cannot tell
        }
        return unitFileExists(paths.mountUnitPath) || unitFileExists(paths.automountUnitPath);
    };

    // Existence of the file, whatever it says: the helper refuses on that too
    // and does not look at who wrote it.
    if (hasUnits(plan.path)) {
        return QStringLiteral("Another mount already uses this folder");
    }
    // Every folder between the allowed root and the path. The root itself
    // cannot be a mount point of ours, so it is not examined.
    QString ancestor = plan.root;
    for (int i = 0; i + 1 < plan.suffix.size(); ++i) {
        ancestor += QLatin1Char('/') + plan.suffix.at(i);
        if (hasUnits(ancestor)) {
            return QStringLiteral("This folder is inside another mount (%1)").arg(ancestor);
        }
    }
    return QString();
}

QString mountPointFsProblem(const QString &raw, const QString &homeDir, uid_t uid,
                            const UnitFileExists &unitFileExists)
{
    UnitSpec::MountpointPlan plan;
    QString error;
    if (!UnitSpec::validateMountpoint(canonicalMountPoint(raw, homeDir), homeDir, &plan, &error)) {
        return QString();
    }

    const UnitFileExists exists = unitFileExists ? unitFileExists : UnitFileExists([](const QString &path) {
        return QFile::exists(path);
    });
    const QString collision = unitCollisionProblem(plan, exists);
    if (!collision.isEmpty()) {
        return collision;
    }
    return UnitSpec::previewMountpointProblem(plan, uid);
}

} // namespace Session
