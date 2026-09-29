/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtimefiles.h"
#include "durablefs.h"

#include <unistd.h>

namespace Root::RuntimeFiles
{

namespace
{

/** Opens (creating as needed) /run/nasmount-ids, root-owned 0755 - see the
 *  file comment for why this is a sibling of /run/nasmount rather than a
 *  subdirectory of it. */
int openIdDir(QString *error)
{
    const int runFd = DurableFs::openSystemRoot(QStringLiteral("/run"), error);
    if (runFd < 0) {
        return -1;
    }
    const int idsFd = DurableFs::createAndVerifyDir(runFd, QStringLiteral("nasmount-ids"),
                                                     DurableFs::ArtifactKind::PublicDirectory, error);
    ::close(runFd);
    return idsFd;
}

} // namespace

bool writeAutomountId(const QString &unitName, uint64_t id, QString *error)
{
    const int dirFd = openIdDir(error);
    if (dirFd < 0) {
        return false;
    }
    const QByteArray content = QByteArray::number(static_cast<qulonglong>(id));
    const bool ok = DurableFs::durableReplace(dirFd, unitName, content, DurableFs::ArtifactKind::PublicRecord, error);
    ::close(dirFd);
    return ok;
}

bool removeAutomountId(const QString &unitName, QString *error)
{
    const int dirFd = openIdDir(error);
    if (dirFd < 0) {
        return false;
    }
    const bool ok = DurableFs::durableUnlink(dirFd, unitName, /*allowMissing=*/true, error);
    ::close(dirFd);
    return ok;
}

} // namespace Root::RuntimeFiles
