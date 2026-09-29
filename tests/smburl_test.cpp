/*
 * Tests for Dialog::SmbUrl - the service menu's own presentation helpers:
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

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
