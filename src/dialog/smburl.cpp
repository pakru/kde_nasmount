/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "smburl.h"
#include "unitvalue.h"
#include "verify.h"

#include <KLocalizedString>

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

QString describeState(const QString &mountPoint)
{
    // The state is spelled out for one sentence, "This share is already saved
    // at ... (<state>).", so it reads as lower-case words in parentheses. It is
    // not the list's own state label, and the context keeps the two apart in
    // the catalog.
    UnitValue::UnitPaths paths;
    QString err;
    if (!UnitValue::unitPathsFor(mountPoint, &paths, &err)) {
        return i18nc("@info:status share state, in parentheses", "unknown");
    }
    const auto def = Verify::inspectDefinition(paths, ::getuid(), mountPoint);
    if (def.state != Verify::Definition::Pair) {
        return i18nc("@info:status share state, in parentheses", "needs attention");
    }
    const Verify::RuntimeSnapshot snap = Verify::inspectRuntime(paths.unitName, mountPoint, def.what);
    if (snap.mount == Verify::MountState::Present && snap.verification == Verify::VerificationState::Match) {
        return i18nc("@info:status share state, in parentheses", "mounted");
    }
    if (snap.mount == Verify::MountState::Indeterminate) {
        return i18nc("@info:status share state, in parentheses", "needs attention");
    }
    if (snap.automount == Verify::AutomountState::Active) {
        return (snap.activationTrust == Verify::ActivationTrust::Trusted)
            ? i18nc("@info:status share state, in parentheses", "ready - mounts on first access")
            : i18nc("@info:status share state, in parentheses", "needs attention");
    }
    return i18nc("@info:status share state, in parentheses", "defined, not active");
}

} // namespace Dialog::SmbUrl
