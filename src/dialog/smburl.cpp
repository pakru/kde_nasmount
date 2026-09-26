/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "smburl.h"
#include "unitvalue.h"
#include "verify.h"

#include <QDir>
#include <QUrl>

#include <unistd.h>

namespace Dialog::SmbUrl
{

QString suggestMountpoint(const QString &unc)
{
    QString leaf = unc;
    while (leaf.endsWith(QLatin1Char('/'))) {
        leaf.chop(1);
    }
    leaf = leaf.section(QLatin1Char('/'), -1);

    leaf.replace(QLatin1Char('/'), QLatin1Char('_'));
    leaf = leaf.trimmed();
    if (leaf.isEmpty() || leaf == QStringLiteral(".") || leaf == QStringLiteral("..")) {
        leaf = QStringLiteral("Share");
    }
    return QDir::homePath() + QLatin1Char('/') + leaf;
}

QUrl authLookupTarget(const QString &unc)
{
    // Validated already, but reached from the command line: treat anything
    // unexpected as "no target" rather than assuming the shape.
    QString rest = unc;
    while (rest.startsWith(QLatin1Char('/'))) {
        rest.remove(0, 1);
    }
    const QString host = rest.section(QLatin1Char('/'), 0, 0);
    const QString share = rest.section(QLatin1Char('/'), 1, 1);
    if (host.isEmpty() || share.isEmpty()) {
        return QUrl();
    }

    // The same three calls kio-extras' smbauthenticator.cpp makes. A
    // concatenated string would re-parse the share name as URL syntax, so
    // "Media Library" would reach the service under a different key.
    QUrl url(QStringLiteral("smb:///"));
    url.setHost(host);
    url.setPath(QLatin1Char('/') + share);
    if (!url.isValid() || url.host() != host) {
        return QUrl();
    }
    return url;
}

QString describeState(const QString &mountPoint)
{
    UnitValue::UnitPaths paths;
    QString err;
    if (!UnitValue::unitPathsFor(mountPoint, &paths, &err)) {
        return QStringLiteral("unknown");
    }
    const auto def = Verify::inspectDefinition(paths, ::getuid(), mountPoint);
    if (def.state != Verify::Definition::Pair) {
        return QStringLiteral("needs attention");
    }
    const Verify::RuntimeSnapshot snap = Verify::inspectRuntime(paths.unitName, mountPoint, def.what);
    if (snap.mount == Verify::MountState::Present && snap.verification == Verify::VerificationState::Match) {
        return QStringLiteral("mounted");
    }
    if (snap.mount == Verify::MountState::Indeterminate) {
        return QStringLiteral("needs attention");
    }
    if (snap.automount == Verify::AutomountState::Active) {
        return (snap.activationTrust == Verify::ActivationTrust::Trusted)
            ? QStringLiteral("armed — mounts on first access")
            : QStringLiteral("needs attention");
    }
    return QStringLiteral("defined, not armed");
}

} // namespace Dialog::SmbUrl
