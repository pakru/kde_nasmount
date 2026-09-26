/*
 * Tests for Root::CredentialStore.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every function here reads or writes below /etc/nasmount, so --
 * like every other kde_nasmount-root test file -- a real accept path needs root
 * and belongs to VM integration testing. What is testable without root is
 * the id-validation fail-closed path, common to every function and checked
 * first, before any filesystem access at all.
 */

#include "credentialstore.h"

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

    const QString invalidId = QStringLiteral("too-short");

    out << "=== every function rejects an invalid share id before any filesystem access ===" << Qt::endl;
    {
        QString error;
        check(QStringLiteral("write() rejects invalid id"),
              !Root::CredentialStore::write(invalidId, QStringLiteral("u"),
                                            QString(), QString(), &error),
              error);
        check(QStringLiteral("write() error names the id"), error.contains(QStringLiteral("id")), error);
    }
    {
        QString error;
        check(QStringLiteral("remove() rejects invalid id"),
              !Root::CredentialStore::remove(invalidId, /*allowMissing=*/true,
                                             &error),
              error);
    }
    {
        QString error;
        check(QStringLiteral("healthy() rejects invalid id"),
              !Root::CredentialStore::healthy(invalidId, &error), error);
    }
    {
        QString error;
        check(QStringLiteral("assertAbsent() rejects invalid id (as a real failure, not a false meaning absent)"),
              !Root::CredentialStore::assertAbsent(invalidId, &error), error);
        check(QStringLiteral("assertAbsent() sets an error for the invalid-id case (an invalid id is a real failure, not absence)"),
              !error.isEmpty(), error);
    }
    out << "=== write(): a non-empty username with an empty password is rejected, not silently written ==="
        << Qt::endl;
    {
        // A well-formed id, so this exercises validateFields() itself, not
        // the id check that already short-circuits the case above.
        const QString validId = QStringLiteral("0123456789abcdef0123456789abcdef");
        QString error;
        check(QStringLiteral("rejected before any filesystem access"),
              !Root::CredentialStore::write(validId, QStringLiteral("alice"),
                                            QString(), QString(), &error),
              error);
        check(QStringLiteral("specific 'password is required' error, not a permission/id error"),
              error.contains(QStringLiteral("password is required")), error);
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
