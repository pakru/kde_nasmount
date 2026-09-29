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
    // tolerant parsing - which percent-encodes the offending characters rather
    // than reinterpreting the URL's structure - otherwise. Safety does not rest
    // on the parsing mode: the component checks below reject anything unexpected,
    // and the helper re-validates everything regardless.
    QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid()) {
        // One caveat before falling back: a single malformed %-escape puts Qt
        // into repair mode for *every* percent sign in the URL, so
        // ".../Media%20Library/100%" would yield a literal "Media%20Library"
        // rather than "Media Library" - a different share name on the same
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
        // QUrl's text goes on to repeat the whole source and every parsed
        // part; shown under an input field that is noise, and a repeat of
        // what is already on screen. The cause is the part before it.
        *error = i18n("Not a valid address: %1",
                      url.errorString().section(QStringLiteral("; source was"), 0, 0));
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
        *error = i18n("This points at a server, not a share - add the share name after the server "
                      "(smb://server/share).");
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
        // The input is not echoed: shown under the field, it is already on
        // screen above the message.
        *error = QStringLiteral("not a share address (expected smb://host/share)");
        return false;
    }

    // Validated here, and reworded in smb:// terms, so the core's own
    // "expected //host/share" wording - which names a spelling the user was
    // never shown - can only surface for input that already passed this.
    if (UnitSpec::hasControlChars(candidate)) {
        *error = QStringLiteral("the share address contains control characters");
        return false;
    }
    QString normalised;
    QString ignored;
    if (!UnitSpec::validateUnc(candidate, &normalised, &ignored)) {
        *error = QStringLiteral("not a valid share address (expected smb://host/share[/subdir], "
                                "with no '..' component)");
        return false;
    }

    if (!urlUser.isEmpty()) {
        if (username.isEmpty()) {
            *error = QStringLiteral("the smb:// address names a user - enter it in Username, or remove "
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

QString shareInputProblem(const QString &input, const QString &username)
{
    QString rest = input;
    if (hasSmbPrefix(rest)) {
        rest.remove(0, smbPrefix.size());
    } else if (rest.startsWith(QStringLiteral("//"))) {
        rest.remove(0, 2);
    }
    if (rest.isEmpty()) {
        return QStringLiteral("Enter the share address, for example smb://nas/Media");
    }

    QString unc;
    QString error;
    if (resolveShareInput(input, username, &unc, &error)) {
        return QString();
    }
    // The refusals are lower-case fragments where they are used in a sentence
    // ("could not ... : <error>"); shown on their own they start a line.
    if (!error.isEmpty()) {
        error[0] = error.at(0).toUpper();
    }
    return error;
}

BrowsedShare browsedShareInput(const QUrl &picked)
{
    BrowsedShare result;
    if (picked.scheme().compare(QStringLiteral("smb"), Qt::CaseInsensitive) != 0) {
        result.error = QStringLiteral("Choose a folder on a network share (smb://)");
        return result;
    }
    // The same parts parseSmbUrl() refuses. KIO does not produce them, but the
    // conversion must not assume it.
    const bool hasPasswordComponent = picked.userInfo(QUrl::FullyEncoded).contains(QLatin1Char(':'));
    if (picked.hasQuery() || picked.hasFragment() || hasPasswordComponent || picked.port() != -1) {
        result.error = QStringLiteral("This address has parts that are not supported here "
                                      "(port, password, query or fragment)");
        return result;
    }
    const QString host = picked.host();
    if (host.isEmpty()) {
        // The network root itself was accepted rather than a server in it.
        result.error = QStringLiteral("Choose a share on a server, not the network root");
        return result;
    }
    if (host.contains(QLatin1Char(':'))) {
        result.error = QStringLiteral("IPv6 hosts are not supported yet");
        return result;
    }

    QString path = picked.path(QUrl::FullyDecoded);
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const QString user = picked.userName(QUrl::FullyDecoded);
    if (UnitSpec::hasControlChars(host) || UnitSpec::hasControlChars(path)
        || UnitSpec::hasControlChars(user)) {
        result.error = QStringLiteral("The address contains control characters");
        return result;
    }

    // Through displayUrl(), like every address the list shows, so what lands
    // in the field parses back to the same share.
    result.input = displayUrl(path.isEmpty() ? QStringLiteral("//%1").arg(host)
                                             : QStringLiteral("//%1/%2").arg(host, path));
    result.user = user;
    return result;
}

QUrl browseStartUrl(const QString &shareInput)
{
    const QUrl root(QStringLiteral("smb://"));

    QString text = shareInput.trimmed();
    if (text.startsWith(QStringLiteral("//"))) {
        text.prepend(QStringLiteral("smb:"));
    }
    if (!hasSmbPrefix(text)) {
        return root;
    }

    const QUrl typed(text, QUrl::TolerantMode);
    if (!typed.isValid() || typed.host().isEmpty()) {
        return root;
    }
    // Rebuilt from the parts worth keeping, so a password or a port typed into
    // the field can never be handed to the file dialog.
    QUrl start;
    start.setScheme(QStringLiteral("smb"));
    start.setHost(typed.host());
    if (!typed.userName().isEmpty()) {
        start.setUserName(typed.userName());
    }
    start.setPath(typed.path());
    return start;
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

} // namespace Session::ShareAddress
