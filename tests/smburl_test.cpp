/*
 * Tests for Dialog::SmbUrl — the service menu's own presentation helpers:
 * the suggested mount point and the credential-lookup target.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The smb:// parsing and the identity split that used to be tested here moved
 * into the session library with the code, and are covered by
 * shareaddress_test.
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

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== suggestMountpoint ===" << Qt::endl;
    {
        const QString home = QDir::homePath();
        check(QStringLiteral("uses the share's own leaf name under $HOME"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/Docs/Archive"))
                  == home + QStringLiteral("/Archive"),
              Dialog::SmbUrl::suggestMountpoint(QStringLiteral("//nas.local/DATA/Docs/Archive")));
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
              !Dialog::SmbUrl::authLookupTarget(QStringLiteral("//nas.local")).isValid());
        check(QStringLiteral("an empty UNC has no lookup target"),
              !Dialog::SmbUrl::authLookupTarget(QString()).isValid());
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
