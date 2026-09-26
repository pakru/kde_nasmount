/*
 * Tests for Root::Operations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * define()/remove() write below /etc/nasmount and /etc/systemd/system, so —
 * exactly like durablefs_test.cpp before it — a
 * real accept path needs root and belongs to VM integration testing. What
 * this file checks, as an unprivileged process, is that both fail closed
 * rather than silently succeeding or crashing, whether or not a password
 * is supplied.
 *
 * purge() has no equivalent fail-closed case here: with no managed units on
 * disk (this sandbox's actual state) it has nothing to validate or stop, so
 * it reaches only read-only, world-readable steps and a `daemon-reload`
 * whose permission outcome for an unprivileged caller is polkit-policy-
 * dependent, not a reliable, deterministic wall the way opening a root-owned
 * 0700 credential directory is for define()/remove(). A meaningful test
 * needs a real fabricated managed unit, which requires root to create in the
 * first place -- purge()'s validation and two-pass ordering are exercised by
 * the privileged VM integration suite instead.
 */

#include "operations.h"

#include <QCoreApplication>
#include <QTextStream>

#include <unistd.h>

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

    if (::getuid() == 0) {
        out << "Skipping: this binary is meant to run unprivileged (its whole point is proving "
               "the fail-closed path without root); running it as root would silently pass "
               "everything for the wrong reason."
            << Qt::endl;
        return 0;
    }

    out << "=== define(): fails closed without root ===" << Qt::endl;
    {
        Root::Operations::DefineInput input;
        input.ownerUid = ::getuid();
        input.ownerGid = ::getgid();
        input.unc = QStringLiteral("//host/share");
        input.mountPoint = QStringLiteral("/mnt/nasmount-operations-test");
        input.unitName = QStringLiteral("mnt-nasmount\\x2doperations\\x2dtest");
        input.username = QStringLiteral("alice");

        const auto result = Root::Operations::define(input);
        check(QStringLiteral("define() without a password does not report ok without root"), !result.ok);
        check(QStringLiteral("define() without a password sets an error"), !result.error.isEmpty());
    }
    {
        Root::Operations::DefineInput input;
        input.ownerUid = ::getuid();
        input.ownerGid = ::getgid();
        input.unc = QStringLiteral("//host/share2");
        input.mountPoint = QStringLiteral("/mnt/nasmount-operations-test2");
        input.unitName = QStringLiteral("mnt-nasmount\\x2doperations\\x2dtest2");
        input.username = QStringLiteral("alice");
        input.domain = QStringLiteral("EXAMPLE");
        input.password = QStringLiteral("hunter2");

        const auto result = Root::Operations::define(input);
        check(QStringLiteral("define() with a full credential does not report ok without root"), !result.ok);
        check(QStringLiteral("define() with a full credential sets an error"), !result.error.isEmpty());
    }

    out << "=== remove(): fails closed without root ===" << Qt::endl;
    {
        Root::Operations::RemovalInput input;
        input.ownerUid = ::getuid();
        input.ownerGid = ::getgid();
        input.shareId = QStringLiteral("0123456789abcdef0123456789abcdef");
        input.authentication = UnitValue::AuthenticationKind::Credentials;
        input.mountPoint = QStringLiteral("/mnt/nasmount-operations-test");
        input.unitName = QStringLiteral("mnt-nasmount\\x2doperations\\x2dtest");
        input.what = QStringLiteral("//host/share");

        const auto result = Root::Operations::remove(input);
        check(QStringLiteral("remove() does not report ok without root"), !result.ok);
        check(QStringLiteral("remove() sets an error"), !result.error.isEmpty());
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
