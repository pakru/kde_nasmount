/*
 * Tests for Dialog::SmbUrl (the service menu's smb:// parsing).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This parsing was untestable while it lived as a static member of the
 * QWidgets dialog; extracting it for the shared-QML front end made it
 * reachable, so pin the behaviour its comments describe -- especially the
 * two non-obvious cases: Dolphin substitutes %u with the URL *as displayed*,
 * so literal spaces must parse, while a malformed %-escape must be refused
 * rather than guessed at (Qt's repair mode would otherwise silently resolve
 * "Media%20Library" to a different share name than intended).
 *
 * None of this is a security boundary -- the KAuth helper re-validates every
 * field -- but a wrong UNC here means mounting the wrong share.
 */

#include "smburl.h"

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

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
    const bool ok = Dialog::SmbUrl::parse(raw, &unc, &user, &error);
    check(label, ok && unc == expectedUnc && user == expectedUser,
          ok ? QStringLiteral("unc=%1 user=%2").arg(unc, user) : error);
}

void expectRejected(const QString &label, const QString &raw)
{
    QString unc;
    QString user;
    QString error;
    const bool ok = Dialog::SmbUrl::parse(raw, &unc, &user, &error);
    check(label, !ok && !error.isEmpty(), ok ? QStringLiteral("accepted as %1").arg(unc) : error);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== parse: accepted forms ===" << Qt::endl;
    expectParsed(QStringLiteral("plain host/share"), QStringLiteral("smb://10.0.0.10/DATA"),
                 QStringLiteral("//10.0.0.10/DATA"), QString());
    expectParsed(QStringLiteral("nested subdirectories are kept"),
                 QStringLiteral("smb://10.0.0.10/DATA/Pavel/Docs/Torrents"),
                 QStringLiteral("//10.0.0.10/DATA/Pavel/Docs/Torrents"), QString());
    expectParsed(QStringLiteral("user in the URL is recovered"),
                 QStringLiteral("smb://pa_kru@nas.local/DATA"), QStringLiteral("//nas.local/DATA"),
                 QStringLiteral("pa_kru"));
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

    out << "=== parse: rejections ===" << Qt::endl;
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

    out << "=== suggestMountpoint ===" << Qt::endl;
    {
        const QString home = QDir::homePath();
        check(QStringLiteral("uses the share's own leaf name under $HOME"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/Docs/Torrents"))
                  == home + QStringLiteral("/Torrents"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/Docs/Torrents")));
        check(QStringLiteral("trailing slash does not produce an empty leaf"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/"))
                  == home + QStringLiteral("/DATA"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/")));
        check(QStringLiteral("degenerate input falls back to a usable name"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//")) == home + QStringLiteral("/Share"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//")));
    }

    out << "=== authLookupTarget ===" << Qt::endl;
    {
        auto expectTarget = [](const QString &label, const QString &unc, const QString &expected) {
            const QUrl url = Dialog::SmbUrl::authLookupTarget(unc);
            check(label, url.toString(QUrl::FullyEncoded) == expected, url.toString());
        };
        // The share root is the lookup target even when the user opened a
        // folder deep inside it: that is what KDE's own SMB worker
        // authenticates against, so anything narrower would miss the entry
        // Dolphin saved.
        expectTarget(QStringLiteral("share root"), QStringLiteral("//10.0.0.10/DATA"),
                     QStringLiteral("smb://10.0.0.10/DATA"));
        expectTarget(QStringLiteral("subdirectories are dropped"),
                     QStringLiteral("//10.0.0.10/DATA/Pavel/Docs"),
                     QStringLiteral("smb://10.0.0.10/DATA"));
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
              !Dialog::SmbUrl::authLookupTarget(QStringLiteral("//nas.local")).isValid());
        check(QStringLiteral("an empty UNC has no lookup target"),
              !Dialog::SmbUrl::authLookupTarget(QString()).isValid());
    }

    out << "=== splitDomainUser ===" << Qt::endl;
    {
        auto expectSplit = [](const QString &label, const QString &combined,
                              const QString &domain, const QString &username) {
            const Dialog::SmbUrl::Identity identity = Dialog::SmbUrl::splitDomainUser(combined);
            check(label, identity.domain == domain && identity.username == username,
                  QStringLiteral("domain=%1 user=%2").arg(identity.domain, identity.username));
        };
        expectSplit(QStringLiteral("plain username"), QStringLiteral("pavel"), QString(),
                    QStringLiteral("pavel"));
        expectSplit(QStringLiteral("backslash-qualified"), QStringLiteral("WORKGROUP\\pavel"),
                    QStringLiteral("WORKGROUP"), QStringLiteral("pavel"));
        expectSplit(QStringLiteral("slash-qualified"), QStringLiteral("WORKGROUP/pavel"),
                    QStringLiteral("WORKGROUP"), QStringLiteral("pavel"));
        // A UPN is one name, not a qualified pair: splitting it at the @
        // would submit "example.com" as a CIFS domain and "pavel" as a user
        // that the server has never heard of.
        expectSplit(QStringLiteral("a UPN is left whole"), QStringLiteral("pavel@example.com"),
                    QString(), QStringLiteral("pavel@example.com"));
        // Upstream's rule, kept exactly: the first separator wins, whichever
        // it is.
        expectSplit(QStringLiteral("the first separator wins"),
                    QStringLiteral("WORKGROUP/sub\\pavel"), QStringLiteral("WORKGROUP"),
                    QStringLiteral("sub\\pavel"));
        expectSplit(QStringLiteral("a leading separator does not split"),
                    QStringLiteral("\\pavel"), QString(), QStringLiteral("\\pavel"));
        expectSplit(QStringLiteral("an empty name stays empty"), QString(), QString(), QString());
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
