/*
 * Tests for MountActions' pure decision functions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * addShare() itself is not unit-tested here: it dispatches a real KAuth call
 * on a worker thread, which is not mockable without a live D-Bus transport
 * (see helperinvoke_test.cpp's header for why). What *is* pure and
 * safety-relevant is isolated here: whether guest/authenticated fields are
 * self-consistent.
 */

#include "mountactions.h"

#include <QCoreApplication>
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

    out << "=== guestFieldsConsistent ===" << Qt::endl;
    {
        check(QStringLiteral("authenticated: username with password and domain"),
              Session::guestFieldsConsistent(QStringLiteral("alice"), QStringLiteral("WORKGROUP"),
                                             QStringLiteral("hunter2")));
        check(QStringLiteral("authenticated: username with empty password/domain (both optional)"),
              Session::guestFieldsConsistent(QStringLiteral("alice"), QString(), QString()));
        check(QStringLiteral("guest: no username, no password, no domain"),
              Session::guestFieldsConsistent(QString(), QString(), QString()));
        check(QStringLiteral("inconsistent: no username but a password supplied"),
              !Session::guestFieldsConsistent(QString(), QString(), QStringLiteral("hunter2")));
        check(QStringLiteral("inconsistent: no username but a domain supplied"),
              !Session::guestFieldsConsistent(QString(), QStringLiteral("WORKGROUP"), QString()));
        check(QStringLiteral("inconsistent: no username but both password and domain supplied"),
              !Session::guestFieldsConsistent(QString(), QStringLiteral("WORKGROUP"), QStringLiteral("hunter2")));
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
