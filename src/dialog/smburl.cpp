/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "smburl.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <KLocalizedString>

#include <QDir>
#include <QRegularExpression>
#include <QUrl>

#include <unistd.h>

namespace Dialog::SmbUrl
{

bool parse(const QString &raw, QString *unc, QString *user, QString *error)
{
    // Dolphin substitutes %u with the URL as displayed, which for a share like
    // "Media Library" contains literal spaces. QUrl::StrictMode rejects those
    // outright ("character ' ' not permitted"), so it cannot be used alone.
    //
    // Parse strictly when the URL is already well-formed, and fall back to
    // tolerant parsing — which percent-encodes the offending characters rather
    // than reinterpreting the URL's structure — otherwise. Safety does not rest
    // on the parsing mode: the component checks below reject anything unexpected,
    // and the helper re-validates everything regardless.
    QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid()) {
        // One caveat before falling back: a single malformed %-escape puts Qt
        // into repair mode for *every* percent sign in the URL, so
        // ".../Media%20Library/100%" would yield a literal "Media%20Library"
        // rather than "Media Library" — a different share name on the same
        // server. A correctly displayed literal percent is already "%25", so
        // refuse rather than guess.
        static const QRegularExpression badEscape(
            QStringLiteral("%(?![0-9A-Fa-f]{2})"));
        if (badEscape.match(raw).hasMatch()) {
            *error = i18n("This URL contains a malformed %1 escape: %2",
                          QStringLiteral("%"), raw);
            return false;
        }
        url = QUrl(raw, QUrl::TolerantMode);
    }
    if (!url.isValid()) {
        *error = i18n("Not a valid URL: %1 (%2)", raw, url.errorString());
        return false;
    }
    if (url.scheme() != QStringLiteral("smb")) {
        *error = i18n("Not an smb:// URL: %1", raw);
        return false;
    }
    // password().isEmpty() misses "smb://user:@host/share", where the component
    // is present but empty; check the raw separator instead.
    const bool hasPasswordComponent =
        url.userInfo(QUrl::FullyEncoded).contains(QLatin1Char(':'));
    if (url.hasQuery() || url.hasFragment() || hasPasswordComponent || url.port() != -1) {
        *error = i18n("This URL has parts that are not supported here "
                      "(port, password, query or fragment): %1", raw);
        return false;
    }
    const QString host = url.host();
    if (host.isEmpty()) {
        *error = i18n("No host in URL: %1", raw);
        return false;
    }
    if (host.contains(QLatin1Char(':'))) {
        *error = i18n("IPv6 hosts are not supported yet: %1", host);
        return false;
    }

    QString path = url.path(QUrl::FullyDecoded);
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path.isEmpty()) {
        *error = i18n("This URL points at a server, not a share — open a share first.");
        return false;
    }

    const QString decodedUser = url.userName(QUrl::FullyDecoded);
    if (UnitSpec::hasControlChars(host) || UnitSpec::hasControlChars(path)
        || UnitSpec::hasControlChars(decodedUser)) {
        *error = i18n("The URL contains control characters.");
        return false;
    }

    *unc = QStringLiteral("//%1/%2").arg(host, path);
    *user = decodedUser;
    return true;
}

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

Identity splitDomainUser(const QString &combined)
{
    const qsizetype slash = combined.indexOf(QLatin1Char('/'));
    const qsizetype backslash = combined.indexOf(QLatin1Char('\\'));
    // qMin only when both exist, since an absent one is -1. Upstream's own
    // formulation, kept recognisable.
    const qsizetype sep = (slash >= 0 && backslash >= 0) ? qMin(slash, backslash)
                                                         : qMax(slash, backslash);
    if (sep > 0) {
        return Identity{combined.left(sep), combined.mid(sep + 1)};
    }
    return Identity{QString(), combined};
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
