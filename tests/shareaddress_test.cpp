/*
 * Tests for Session::ShareAddress — the smb:// ⇄ //host/share conversion
 * both front ends use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pins the behaviour the parser's comments describe — especially the two
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
#include <QTextStream>

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

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
