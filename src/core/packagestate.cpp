/*
 * packagestate - conservative, read-only native-package removal gate.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A valid marker, an invalid marker in nasmount's namespace, or any durable
 * credential/runtime root is sufficient to stop removal. The scan examines
 * every candidate half before returning Managed so a malformed sibling is
 * reported as Unsafe rather than hidden by a valid share found first.
 */

#include "packagestate.h"

#include "unitvalue.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace PackageState
{
namespace
{

QString underRoot(const QString &root, const QString &absolutePath)
{
    if (root == QLatin1String("/")) {
        return absolutePath;
    }
    return QDir::cleanPath(root + absolutePath);
}

Result result(Classification classification, const QString &detail)
{
    return {classification, detail};
}

} // namespace

Result classify(const QString &filesystemRoot)
{
    if (!QDir::isAbsolutePath(filesystemRoot) || QDir::cleanPath(filesystemRoot) != filesystemRoot) {
        return result(Classification::Indeterminate,
                      QStringLiteral("filesystem root is not an absolute normalized path"));
    }

    const QString unitPath = underRoot(filesystemRoot, QStringLiteral("/etc/systemd/system"));
    const QFileInfo unitInfo(unitPath);
    bool managedUnitFound = false;
    if (unitInfo.exists()) {
        if (!unitInfo.isDir() || unitInfo.isSymLink()) {
            return result(Classification::Unsafe,
                          QStringLiteral("system unit root is not a real directory: %1").arg(unitPath));
        }

        QDir units(unitPath);
        if (!units.isReadable()) {
            return result(Classification::Indeterminate,
                          QStringLiteral("cannot read system unit root: %1").arg(unitPath));
        }
        const QFileInfoList candidates = units.entryInfoList(
            {QStringLiteral("*.mount"), QStringLiteral("*.automount")},
            QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &candidate : candidates) {
            if (candidate.isSymLink() || !candidate.isFile()) {
                return result(Classification::Unsafe,
                              QStringLiteral("managed-unit candidate is not a regular file: %1")
                                  .arg(candidate.filePath()));
            }
            QFile file(candidate.filePath());
            if (!file.open(QIODevice::ReadOnly)) {
                return result(Classification::Indeterminate,
                              QStringLiteral("cannot read managed-unit candidate: %1")
                                  .arg(candidate.filePath()));
            }
            if (file.size() > 1024 * 1024) {
                return result(Classification::Unsafe,
                              QStringLiteral("managed-unit candidate is unexpectedly large: %1")
                                  .arg(candidate.filePath()));
            }
            const QString content = QString::fromUtf8(file.readAll());
            if (!UnitValue::hasMarker(content)) {
                continue;
            }
            UnitValue::Marker marker;
            QString markerError;
            if (!UnitValue::parseMarker(content, &marker, &markerError)) {
                return result(Classification::Unsafe,
                              QStringLiteral("malformed nasmount marker in %1: %2")
                                  .arg(candidate.filePath(), markerError));
            }
            managedUnitFound = true;
        }
    }

    const QStringList stateRoots = {
        QStringLiteral("/etc/nasmount"),
        QStringLiteral("/run/nasmount"),
        QStringLiteral("/run/nasmount-ids"),
    };
    for (const QString &stateRoot : stateRoots) {
        const QString path = underRoot(filesystemRoot, stateRoot);
        const QFileInfo stateInfo(path);
        if (stateInfo.exists() || stateInfo.isSymLink()) {
            return result(Classification::Managed,
                          QStringLiteral("nasmount state exists at %1").arg(path));
        }
    }

    if (managedUnitFound) {
        return result(Classification::Managed,
                      QStringLiteral("nasmount-managed systemd units are installed"));
    }
    return result(Classification::Empty, QStringLiteral("no managed nasmount root state exists"));
}

Result classifyHost()
{
    return classify(QStringLiteral("/"));
}

} // namespace PackageState
