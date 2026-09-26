/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareaddress.h"
#include "unitspec.h"

#include <KLocalizedString>

#include <QRegularExpression>
#include <QUrl>

namespace Session::ShareAddress
{

namespace
{

const QString smbPrefix = QStringLiteral("smb://");

bool hasSmbPrefix(const QString &input)
{
    return input.startsWith(smbPrefix, Qt::CaseInsensitive);
}

} // namespace

bool parseSmbUrl(const QString &raw, QString *unc, QString *user, QString *error)
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

bool sameIdentity(const QString &a, const QString &b)
{
    const Identity left = splitDomainUser(a);
    const Identity right = splitDomainUser(b);
    if (left.username.compare(right.username, Qt::CaseInsensitive) != 0) {
        return false;
    }
    return left.domain.isEmpty() || right.domain.isEmpty()
        || left.domain.compare(right.domain, Qt::CaseInsensitive) == 0;
}

QString displayUrl(const QString &unc)
{
    if (!unc.startsWith(QStringLiteral("//"))) {
        return unc;
    }
    QString rest = unc.mid(2);
    // '%' first, so the escapes added for '#' and '?' are not escaped again.
    rest.replace(QLatin1Char('%'), QStringLiteral("%25"));
    rest.replace(QLatin1Char('#'), QStringLiteral("%23"));
    rest.replace(QLatin1Char('?'), QStringLiteral("%3F"));
    return smbPrefix + rest;
}

bool resolveShareInput(const QString &input, const QString &username, QString *unc, QString *error)
{
    QString candidate;
    QString urlUser;
    if (hasSmbPrefix(input)) {
        if (!parseSmbUrl(input, &candidate, &urlUser, error)) {
            return false;
        }
    } else if (input.startsWith(QStringLiteral("//"))) {
        candidate = input;
    } else {
        *error = QStringLiteral("not a share address: %1 (expected smb://host/share)").arg(input);
        return false;
    }

    // Validated here, and reworded in smb:// terms, so the core's own
    // "expected //host/share" wording — which names a spelling the user was
    // never shown — can only surface for input that already passed this.
    if (UnitSpec::hasControlChars(candidate)) {
        *error = QStringLiteral("the share address contains control characters");
        return false;
    }
    QString normalised;
    QString ignored;
    if (!UnitSpec::validateUnc(candidate, &normalised, &ignored)) {
        *error = QStringLiteral("not a valid share address: %1 (expected smb://host/share[/subdir], "
                                "with no '..' component)")
                     .arg(input);
        return false;
    }

    if (!urlUser.isEmpty()) {
        if (username.isEmpty()) {
            *error = QStringLiteral("the smb:// address names a user — enter it in Username, or remove "
                                    "it from the address");
            return false;
        }
        if (!sameIdentity(urlUser, username)) {
            *error = QStringLiteral("the smb:// address names a different user than the Username field");
            return false;
        }
    }

    *unc = normalised;
    return true;
}

QString userInShareInput(const QString &input)
{
    if (!hasSmbPrefix(input)) {
        return QString();
    }
    QString unc;
    QString user;
    QString error;
    return parseSmbUrl(input, &unc, &user, &error) ? user : QString();
}

} // namespace Session::ShareAddress
