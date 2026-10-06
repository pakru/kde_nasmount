/*
 * Tests for Session::ShareAddress - the smb:// ⇄ //host/share conversion
 * both front ends use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pins the behaviour the parser's comments describe - especially the two
 * non-obvious cases: Dolphin substitutes %u with the URL *as displayed*, so
 * literal spaces must parse, while a malformed %-escape must be refused
 * rather than guessed at (Qt's repair mode would otherwise silently resolve
 * "Media%20Library" to a different share name than intended).
 *
 * The round trip is the property the KCM list depends on: every address it
 * shows must resolve back to the same UNC when pasted into Add.
 *
 * None of this is a security boundary -- the KAuth helper re-validates every
 * field -- but a wrong UNC here means mounting the wrong share.
 */

#include "shareaddress.h"

#include <QCoreApplication>
#include <QList>
#include <QTextStream>
#include <QUrl>

using namespace Session::ShareAddress;

static int passed = 0;
static int failed = 0;

static void check(const QString &label, bool condition, const QString &detail = QString())
{
    QTextStream out(stdout);
    out << (condition ? "  PASS  " : "  FAIL  ") << label;
    if (!detail.isEmpty()) {
        out << "   " << detail;
    }
    out << Qt::endl;
    condition ? ++passed : ++failed;
}

namespace
{

/** Accept-path helper: parses and reports the resulting UNC/user. */
void expectParsed(const QString &label, const QString &raw, const QString &expectedUnc,
                  const QString &expectedUser)
{
    QString unc;
    QString user;
    QString error;
    const bool ok = parseSmbUrl(raw, &unc, &user, &error);
    check(label, ok && unc == expectedUnc && user == expectedUser,
          ok ? QStringLiteral("unc=%1 user=%2").arg(unc, user) : error);
}

void expectRejected(const QString &label, const QString &raw)
{
    QString unc;
    QString user;
    QString error;
    const bool ok = parseSmbUrl(raw, &unc, &user, &error);
    check(label, !ok && !error.isEmpty(), ok ? QStringLiteral("accepted as %1").arg(unc) : error);
}

void expectResolved(const QString &label, const QString &input, const QString &username,
                    const QString &expectedUnc)
{
    QString unc;
    QString error;
    const bool ok = resolveShareInput(input, username, &unc, &error);
    check(label, ok && unc == expectedUnc, ok ? QStringLiteral("unc=%1").arg(unc) : error);
}

/** A refusal must explain itself in the spelling the user was shown: an
 *  error naming "//host/share" would describe a form the KCM never shows. */
void expectRefused(const QString &label, const QString &input, const QString &username)
{
    QString unc;
    QString error;
    const bool ok = resolveShareInput(input, username, &unc, &error);
    check(label, !ok && !error.isEmpty() && !error.contains(QStringLiteral("expected //")),
          ok ? QStringLiteral("accepted as %1").arg(unc) : error);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== parseSmbUrl: accepted forms ===" << Qt::endl;
    expectParsed(QStringLiteral("plain host/share"), QStringLiteral("smb://192.0.2.10/DATA"),
                 QStringLiteral("//192.0.2.10/DATA"), QString());
    expectParsed(QStringLiteral("nested subdirectories are kept"),
                 QStringLiteral("smb://192.0.2.10/DATA/Projects/Docs/Archive"),
                 QStringLiteral("//192.0.2.10/DATA/Projects/Docs/Archive"), QString());
    expectParsed(QStringLiteral("user in the URL is recovered"),
                 QStringLiteral("smb://alice@nas.local/DATA"), QStringLiteral("//nas.local/DATA"),
                 QStringLiteral("alice"));
    expectParsed(QStringLiteral("percent-encoded space decodes"),
                 QStringLiteral("smb://nas.local/Media%20Library"),
                 QStringLiteral("//nas.local/Media Library"), QString());
    // Dolphin passes the URL as displayed; a literal space must still work.
    expectParsed(QStringLiteral("literal space (Dolphin %u substitution) parses"),
                 QStringLiteral("smb://nas.local/Media Library"),
                 QStringLiteral("//nas.local/Media Library"), QString());
    expectParsed(QStringLiteral("trailing slash is trimmed"), QStringLiteral("smb://nas.local/DATA/"),
                 QStringLiteral("//nas.local/DATA"), QString());
    expectParsed(QStringLiteral("encoded percent stays a literal percent"),
                 QStringLiteral("smb://nas.local/100%25"), QStringLiteral("//nas.local/100%"), QString());

    out << "=== parseSmbUrl: rejections ===" << Qt::endl;
    expectRejected(QStringLiteral("non-smb scheme"), QStringLiteral("http://nas.local/DATA"));
    expectRejected(QStringLiteral("server-only URL, no share"), QStringLiteral("smb://nas.local"));
    expectRejected(QStringLiteral("server-only URL with slash"), QStringLiteral("smb://nas.local/"));
    expectRejected(QStringLiteral("no host"), QStringLiteral("smb:///DATA"));
    expectRejected(QStringLiteral("port is not supported"), QStringLiteral("smb://nas.local:445/DATA"));
    expectRejected(QStringLiteral("password component is refused"),
                   QStringLiteral("smb://user:secret@nas.local/DATA"));
    expectRejected(QStringLiteral("empty-but-present password component is refused"),
                   QStringLiteral("smb://user:@nas.local/DATA"));
    expectRejected(QStringLiteral("query is refused"), QStringLiteral("smb://nas.local/DATA?x=1"));
    expectRejected(QStringLiteral("fragment is refused"), QStringLiteral("smb://nas.local/DATA#frag"));
    expectRejected(QStringLiteral("IPv6 host is refused"), QStringLiteral("smb://[fe80::1]/DATA"));
    // A lone '%' would put Qt into repair mode for every escape in the URL,
    // silently changing which share is meant -- refuse instead of guessing.
    expectRejected(QStringLiteral("malformed %-escape is refused, never repaired"),
                   QStringLiteral("smb://nas.local/Media%20Library/100%"));
    expectRejected(QStringLiteral("empty input"), QString());

    out << "=== splitDomainUser ===" << Qt::endl;
    {
        auto expectSplit = [](const QString &label, const QString &combined,
                              const QString &domain, const QString &username) {
            const Identity identity = splitDomainUser(combined);
            check(label, identity.domain == domain && identity.username == username,
                  QStringLiteral("domain=%1 user=%2").arg(identity.domain, identity.username));
        };
        expectSplit(QStringLiteral("plain username"), QStringLiteral("alice"), QString(),
                    QStringLiteral("alice"));
        expectSplit(QStringLiteral("backslash-qualified"), QStringLiteral("WORKGROUP\\alice"),
                    QStringLiteral("WORKGROUP"), QStringLiteral("alice"));
        expectSplit(QStringLiteral("slash-qualified"), QStringLiteral("WORKGROUP/alice"),
                    QStringLiteral("WORKGROUP"), QStringLiteral("alice"));
        // A UPN is one name, not a qualified pair: splitting it at the @
        // would submit "example.com" as a CIFS domain and "alice" as a user
        // that the server has never heard of.
        expectSplit(QStringLiteral("a UPN is left whole"), QStringLiteral("alice@example.com"),
                    QString(), QStringLiteral("alice@example.com"));
        // Upstream's rule, kept exactly: the first separator wins, whichever
        // it is.
        expectSplit(QStringLiteral("the first separator wins"),
                    QStringLiteral("WORKGROUP/sub\\alice"), QStringLiteral("WORKGROUP"),
                    QStringLiteral("sub\\alice"));
        expectSplit(QStringLiteral("a leading separator does not split"),
                    QStringLiteral("\\alice"), QString(), QStringLiteral("\\alice"));
        expectSplit(QStringLiteral("an empty name stays empty"), QString(), QString(), QString());
    }

    out << "=== displayUrl ===" << Qt::endl;
    {
        auto expectDisplay = [](const QString &label, const QString &unc, const QString &expected) {
            const QString shown = displayUrl(unc);
            check(label, shown == expected, shown);
        };
        expectDisplay(QStringLiteral("plain share"), QStringLiteral("//nas/share"),
                      QStringLiteral("smb://nas/share"));
        expectDisplay(QStringLiteral("spaces stay literal, like Dolphin"),
                      QStringLiteral("//nas.example.org/Media Library/Videos"),
                      QStringLiteral("smb://nas.example.org/Media Library/Videos"));
        expectDisplay(QStringLiteral("% # ? are encoded, and only those"),
                      QStringLiteral("//nas/100% C# why?"),
                      QStringLiteral("smb://nas/100%25 C%23 why%3F"));
        expectDisplay(QStringLiteral("host case is preserved"), QStringLiteral("//NAS.Local/DATA"),
                      QStringLiteral("smb://NAS.Local/DATA"));
        expectDisplay(QStringLiteral("non-ASCII stays literal"), QStringLiteral("//nas/Материалы"),
                      QStringLiteral("smb://nas/Материалы"));
        expectDisplay(QStringLiteral("a non-UNC mount source is shown as found"),
                      QStringLiteral("systemd-1"), QStringLiteral("systemd-1"));
        expectDisplay(QStringLiteral("empty stays empty"), QString(), QString());
    }

    out << "=== round trip: displayUrl -> resolveShareInput ===" << Qt::endl;
    {
        // Lowercase hosts only: the parser lowercases the host (QUrl does),
        // which is harmless for SMB but would fail a byte comparison.
        const QStringList uncs = {
            QStringLiteral("//nas.local/DATA"),
            QStringLiteral("//nas.local/Media Library/Videos"),
            QStringLiteral("//nas.local/100% done"),
            QStringLiteral("//nas.local/share/C# projects"),
            QStringLiteral("//nas.local/share/why?"),
            QStringLiteral("//nas.local/Media%20Library"),
            QStringLiteral("//192.0.2.10/DATA/Projects/Docs/Archive"),
            QStringLiteral("//nas.local/Материалы"),
        };
        for (const QString &unc : uncs) {
            QString back;
            QString error;
            const bool ok = resolveShareInput(displayUrl(unc), QString(), &back, &error);
            check(QStringLiteral("round trip: %1").arg(unc), ok && back == unc,
                  ok ? QStringLiteral("%1 -> %2").arg(displayUrl(unc), back) : error);
        }
    }

    out << "=== resolveShareInput ===" << Qt::endl;
    expectResolved(QStringLiteral("// passes through"), QStringLiteral("//nas/share"), QString(),
                   QStringLiteral("//nas/share"));
    expectResolved(QStringLiteral("// trailing slash is normalised"), QStringLiteral("//nas/share/"),
                   QString(), QStringLiteral("//nas/share"));
    expectResolved(QStringLiteral("smb:// is converted"), QStringLiteral("smb://nas/share/sub dir"),
                   QString(), QStringLiteral("//nas/share/sub dir"));
    expectResolved(QStringLiteral("scheme is case-insensitive"), QStringLiteral("SMB://nas/share"),
                   QString(), QStringLiteral("//nas/share"));
    expectRefused(QStringLiteral("server-only smb:// is refused"), QStringLiteral("smb://nas"), QString());
    expectRefused(QStringLiteral("bare smb:// is refused"), QStringLiteral("smb://"), QString());
    expectRefused(QStringLiteral("bare // is refused"), QStringLiteral("//"), QString());
    expectRefused(QStringLiteral("server-only // is refused"), QStringLiteral("//nas"), QString());
    expectRefused(QStringLiteral("http:// is refused"), QStringLiteral("http://nas/share"), QString());
    expectRefused(QStringLiteral("no prefix is refused"), QStringLiteral("nas/share"), QString());
    expectRefused(QStringLiteral("'..' is refused"), QStringLiteral("smb://nas/share/../other"), QString());
    expectRefused(QStringLiteral("a comma in smb:// is refused"), QStringLiteral("smb://nas/share/a,b"), QString());
    expectRefused(QStringLiteral("a comma in // is refused"), QStringLiteral("//nas/share/a,b"), QString());
    expectRefused(QStringLiteral("a percent-encoded comma is refused"), QStringLiteral("smb://nas/share/a%2Cb"),
                  QString());
    {
        QString unc;
        QString error;
        resolveShareInput(QStringLiteral("nas/share"), QString(), &unc, &error);
        check(QStringLiteral("a refusal names the smb:// spelling"),
              error.contains(QStringLiteral("smb://")), error);
    }

    out << "=== resolveShareInput: the address's user ===" << Qt::endl;
    // The form fills an empty Username from the address, so an empty one at
    // submit was cleared on purpose; filling it here would create an account
    // share whose password field was never enabled.
    expectRefused(QStringLiteral("address user with empty Username is refused"),
                  QStringLiteral("smb://alice@nas/share"), QString());
    expectResolved(QStringLiteral("same user is accepted"), QStringLiteral("smb://alice@nas/share"),
                   QStringLiteral("alice"), QStringLiteral("//nas/share"));
    expectResolved(QStringLiteral("user comparison is case-insensitive"),
                   QStringLiteral("smb://alice@nas/share"), QStringLiteral("Alice"),
                   QStringLiteral("//nas/share"));
    expectResolved(QStringLiteral("a domain on one side only still matches"),
                   QStringLiteral("smb://alice@nas/share"), QStringLiteral("DOM\\alice"),
                   QStringLiteral("//nas/share"));
    expectRefused(QStringLiteral("a different user is refused"), QStringLiteral("smb://alice@nas/share"),
                  QStringLiteral("bob"));
    expectRefused(QStringLiteral("a different domain is refused"),
                  QStringLiteral("smb://DOM1%5Calice@nas/share"), QStringLiteral("DOM2\\alice"));
    expectResolved(QStringLiteral("no user in the address leaves Username free"),
                   QStringLiteral("smb://nas/share"), QStringLiteral("bob"), QStringLiteral("//nas/share"));

    out << "=== userInShareInput ===" << Qt::endl;
    check(QStringLiteral("complete address with a user"),
          userInShareInput(QStringLiteral("smb://alice@nas/share")) == QStringLiteral("alice"));
    check(QStringLiteral("address without a user"), userInShareInput(QStringLiteral("smb://nas/share")).isEmpty());
    check(QStringLiteral("partial input while typing"), userInShareInput(QStringLiteral("smb://alice@")).isEmpty());
    check(QStringLiteral("// input has no user"), userInShareInput(QStringLiteral("//nas/share")).isEmpty());
    check(QStringLiteral("unparsable input"), userInShareInput(QStringLiteral("smb://nas:x/share")).isEmpty());

    out << "=== shareInputProblem: the form's verdict is the submit's verdict ===" << Qt::endl;
    {
        struct Case {
            QString input;
            QString username;
        };
        const QList<Case> cases = {
            {QString(), QString()},
            {QStringLiteral("smb://"), QString()},
            {QStringLiteral("//"), QString()},
            {QStringLiteral("SMB://"), QString()},
            {QStringLiteral("smb://nas.local/DATA"), QString()},
            {QStringLiteral("//nas.local/DATA"), QString()},
            {QStringLiteral("smb://nas.local/DATA/sub dir/"), QString()},
            {QStringLiteral("smb://nas.local"), QString()},
            {QStringLiteral("smb://nas.local/"), QString()},
            {QStringLiteral("nas/share"), QString()},
            {QStringLiteral("http://nas/share"), QString()},
            {QStringLiteral("smb://nas.local:445/DATA"), QString()},
            {QStringLiteral("smb://user:secret@nas.local/DATA"), QString()},
            {QStringLiteral("smb://nas.local/DATA?x=1"), QString()},
            {QStringLiteral("smb://[fe80::1]/DATA"), QString()},
            {QStringLiteral("smb://nas.local/100%"), QString()},
            {QStringLiteral("smb://nas.local/DATA/../other"), QString()},
            {QStringLiteral("smb://na$s/DATA"), QString()},
            {QStringLiteral("smb://alice@nas.local/DATA"), QString()},
            {QStringLiteral("smb://alice@nas.local/DATA"), QStringLiteral("alice")},
            {QStringLiteral("smb://alice@nas.local/DATA"), QStringLiteral("bob")},
        };
        for (const Case &c : cases) {
            QString unc;
            QString error;
            const bool accepted = resolveShareInput(c.input, c.username, &unc, &error);
            const QString problem = shareInputProblem(c.input, c.username);
            check(QStringLiteral("problem is empty exactly when the submit accepts: '%1' as '%2'")
                      .arg(c.input, c.username),
                  problem.isEmpty() == accepted, problem);
        }

        auto expectProblem = [](const QString &label, const QString &input, const QString &username,
                                const QString &mention) {
            const QString problem = shareInputProblem(input, username);
            check(label, problem.contains(mention, Qt::CaseInsensitive) && problem.at(0).isUpper(), problem);
        };
        expectProblem(QStringLiteral("the pre-filled prefix reads as nothing entered"),
                      QStringLiteral("smb://"), QString(), QStringLiteral("Enter the share address"));
        expectProblem(QStringLiteral("empty reads as nothing entered"), QString(), QString(),
                      QStringLiteral("Enter the share address"));
        expectProblem(QStringLiteral("a bare // reads as nothing entered"), QStringLiteral("//"), QString(),
                      QStringLiteral("Enter the share address"));
        expectProblem(QStringLiteral("no prefix"), QStringLiteral("nas/share"), QString(),
                      QStringLiteral("expected smb://host/share"));
        expectProblem(QStringLiteral("a server without a share says so"), QStringLiteral("smb://nas.local"),
                      QString(), QStringLiteral("not a share"));
        expectProblem(QStringLiteral("a port is named as unsupported"), QStringLiteral("smb://nas.local:445/DATA"),
                      QString(), QStringLiteral("not supported"));
        expectProblem(QStringLiteral("a host that cannot be parsed names the cause"), QStringLiteral("smb://na$s/DATA"),
                      QString(), QStringLiteral("invalid hostname"));
        check(QStringLiteral("and does not dump the parser's internals"),
              !shareInputProblem(QStringLiteral("smb://na$s/DATA"), QString()).contains(QStringLiteral("source was")));
        expectProblem(QStringLiteral("a host that parses but cannot be mounted"),
                      QStringLiteral("//na$s/DATA"), QString(), QStringLiteral("not a valid share address"));
        expectProblem(QStringLiteral("a comma is named as the cause"), QStringLiteral("smb://nas/share/a,b"),
                      QString(), QStringLiteral("comma"));
        expectProblem(QStringLiteral("a user in the address needs a Username"),
                      QStringLiteral("smb://alice@nas.local/DATA"), QString(), QStringLiteral("Username"));
        expectProblem(QStringLiteral("a user in the address must match Username"),
                      QStringLiteral("smb://alice@nas.local/DATA"), QStringLiteral("bob"),
                      QStringLiteral("different user"));
        check(QStringLiteral("a refusal does not echo the input back"),
              !shareInputProblem(QStringLiteral("nas/share"), QString()).contains(QStringLiteral("nas/share")));
    }

    out << "=== browsedShareInput: a folder picked in the Share browser ===" << Qt::endl;
    {
        struct Accept {
            const char *label;
            QString url;
            QString input;
            QString user;
            QString unc; ///< what resolveShareInput() must make of `input`
        };
        const QList<Accept> accepted = {
            {"share", QStringLiteral("smb://nas.local/DATA"), QStringLiteral("smb://nas.local/DATA"), QString(),
             QStringLiteral("//nas.local/DATA")},
            {"folder inside a share", QStringLiteral("smb://nas.local/DATA/Films/2024"),
             QStringLiteral("smb://nas.local/DATA/Films/2024"), QString(),
             QStringLiteral("//nas.local/DATA/Films/2024")},
            {"trailing slash", QStringLiteral("smb://nas.local/DATA/"), QStringLiteral("smb://nas.local/DATA"),
             QString(), QStringLiteral("//nas.local/DATA")},
            {"encoded space", QStringLiteral("smb://nas.local/Media%20Library"),
             QStringLiteral("smb://nas.local/Media Library"), QString(), QStringLiteral("//nas.local/Media Library")},
            {"a percent sign stays encoded in the field", QStringLiteral("smb://nas.local/100%25"),
             QStringLiteral("smb://nas.local/100%25"), QString(), QStringLiteral("//nas.local/100%")},
            {"a number sign stays encoded in the field", QStringLiteral("smb://nas.local/C%23"),
             QStringLiteral("smb://nas.local/C%23"), QString(), QStringLiteral("//nas.local/C#")},
            {"a question mark stays encoded in the field", QStringLiteral("smb://nas.local/why%3F"),
             QStringLiteral("smb://nas.local/why%3F"), QString(), QStringLiteral("//nas.local/why?")},
            {"the user moves out of the address", QStringLiteral("smb://alice@nas.local/DATA"),
             QStringLiteral("smb://nas.local/DATA"), QStringLiteral("alice"), QStringLiteral("//nas.local/DATA")},
        };
        for (const Accept &a : accepted) {
            const BrowsedShare picked = browsedShareInput(QUrl(a.url));
            check(QStringLiteral("browsed: %1").arg(QLatin1String(a.label)),
                  picked.error.isEmpty() && picked.input == a.input && picked.user == a.user,
                  picked.error.isEmpty() ? QStringLiteral("input='%1' user='%2'").arg(picked.input, picked.user)
                                         : picked.error);
            QString unc;
            QString error;
            const bool ok = resolveShareInput(picked.input, picked.user, &unc, &error);
            check(QStringLiteral("browsed text resolves to the picked share: %1").arg(QLatin1String(a.label)),
                  ok && unc == a.unc, ok ? unc : error);
        }

        // A server pick is shown, not dropped: the ordinary validation then
        // says a share is missing.
        const BrowsedShare server = browsedShareInput(QUrl(QStringLiteral("smb://nas.local")));
        check(QStringLiteral("browsed: a server is shown as a server"),
              server.error.isEmpty() && server.input == QStringLiteral("smb://nas.local"));
        check(QStringLiteral("browsed: and validation then explains the missing share"),
              shareInputProblem(server.input, server.user).contains(QStringLiteral("not a share")));

        const QStringList refused = {
            QStringLiteral("file:///home/user/Documents"),
            QStringLiteral("file:///run/user/1000/kio-fuse-ab12/smb/nas/DATA"),
            QStringLiteral("http://nas.local/DATA"),
            QStringLiteral("smb://nas.local:445/DATA"),
            QStringLiteral("smb://user:secret@nas.local/DATA"),
            QStringLiteral("smb://user:@nas.local/DATA"),
            QStringLiteral("smb://nas.local/DATA?x=1"),
            QStringLiteral("smb://nas.local/DATA#frag"),
            QStringLiteral("smb:///"),
            QStringLiteral("smb://[fe80::1]/DATA"),
        };
        for (const QString &url : refused) {
            const BrowsedShare picked = browsedShareInput(QUrl(url));
            check(QStringLiteral("browsed: refused %1").arg(url), !picked.error.isEmpty() && picked.input.isEmpty(),
                  picked.input);
        }
        check(QStringLiteral("browsed: an empty URL is refused"), !browsedShareInput(QUrl()).error.isEmpty());
    }

    out << "=== browseStartUrl: where the Share browser opens ===" << Qt::endl;
    {
        auto expectStart = [](const QString &label, const QString &input, const QString &host,
                              const QString &path, const QString &user = QString()) {
            const QUrl start = browseStartUrl(input);
            check(label,
                  start.scheme() == QStringLiteral("smb") && start.host() == host && start.path() == path
                      && start.userName() == user && start.password().isEmpty() && start.port() == -1
                      && !start.hasQuery() && !start.hasFragment(),
                  start.toString());
        };
        expectStart(QStringLiteral("nothing typed opens the network root"), QString(), QString(), QString());
        expectStart(QStringLiteral("the bare prefix opens the network root"), QStringLiteral("smb://"), QString(),
                    QString());
        expectStart(QStringLiteral("text that is not an address opens the network root"),
                    QStringLiteral("nas/share"), QString(), QString());
        expectStart(QStringLiteral("a server opens that server"), QStringLiteral("smb://nas.local"),
                    QStringLiteral("nas.local"), QString());
        expectStart(QStringLiteral("a share opens that share"), QStringLiteral("smb://nas.local/DATA"),
                    QStringLiteral("nas.local"), QStringLiteral("/DATA"));
        expectStart(QStringLiteral("a folder opens that folder"), QStringLiteral("smb://nas.local/DATA/Films"),
                    QStringLiteral("nas.local"), QStringLiteral("/DATA/Films"));
        expectStart(QStringLiteral("the // spelling is understood"), QStringLiteral("//nas.local/DATA"),
                    QStringLiteral("nas.local"), QStringLiteral("/DATA"));
        expectStart(QStringLiteral("a literal space is kept"), QStringLiteral("smb://nas.local/Media Library"),
                    QStringLiteral("nas.local"), QStringLiteral("/Media Library"));
        expectStart(QStringLiteral("the user is kept"), QStringLiteral("smb://alice@nas.local/DATA"),
                    QStringLiteral("nas.local"), QStringLiteral("/DATA"), QStringLiteral("alice"));
        // Nothing the user typed as a secret, or a port, may be handed on.
        expectStart(QStringLiteral("a password, port, query and fragment are dropped"),
                    QStringLiteral("smb://alice:secret@nas.local:445/DATA?x=1#f"), QStringLiteral("nas.local"),
                    QStringLiteral("/DATA"), QStringLiteral("alice"));
    }

    out << "=== authLookupTarget ===" << Qt::endl;
    {
        auto expectTarget = [](const QString &label, const QString &unc, const QString &expected) {
            const QUrl url = authLookupTarget(unc);
            check(label, url.toString(QUrl::FullyEncoded) == expected, url.toString());
        };
        // The share root is the lookup target even when the user opened a
        // folder deep inside it: that is what KDE's own SMB worker
        // authenticates against, so anything narrower would miss the entry
        // Dolphin saved.
        expectTarget(QStringLiteral("share root"), QStringLiteral("//192.0.2.10/DATA"),
                     QStringLiteral("smb://192.0.2.10/DATA"));
        expectTarget(QStringLiteral("subdirectories are dropped"),
                     QStringLiteral("//192.0.2.10/DATA/Documents/Docs"),
                     QStringLiteral("smb://192.0.2.10/DATA"));
        expectTarget(QStringLiteral("a space in the share name is encoded once"),
                     QStringLiteral("//nas.local/Media Library"),
                     QStringLiteral("smb://nas.local/Media%20Library"));
        expectTarget(QStringLiteral("a literal percent is encoded, not re-read"),
                     QStringLiteral("//nas.local/100%"), QStringLiteral("smb://nas.local/100%25"));
        expectTarget(QStringLiteral("an already-encoded-looking name is not decoded again"),
                     QStringLiteral("//nas.local/Media%20Library"),
                     QStringLiteral("smb://nas.local/Media%2520Library"));
        expectTarget(QStringLiteral("a non-ASCII share name survives"),
                     QStringLiteral("//nas.local/Материалы"),
                     QStringLiteral("smb://nas.local/%D0%9C%D0%B0%D1%82%D0%B5%D1%80%D0%B8%D0%B0%D0%BB%D1%8B"));
        expectTarget(QStringLiteral("a trailing slash does not create an empty component"),
                     QStringLiteral("//nas.local/DATA/"), QStringLiteral("smb://nas.local/DATA"));

        check(QStringLiteral("a server-only UNC has no lookup target"),
              !authLookupTarget(QStringLiteral("//nas.local")).isValid());
        check(QStringLiteral("an empty UNC has no lookup target"),
              !authLookupTarget(QString()).isValid());
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
