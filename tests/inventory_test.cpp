/*
 * Tests for Root::Inventory.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * buildFor() itself enumerates real /etc/systemd/system state and reads
 * /etc/nasmount credential files, so — like durablefs_test.cpp before it —
 * it cannot exercise a real accept path as an unprivileged test process.
 * ShareRecord is {id, credentialApplicable, credentialHealthy}; toJson() is
 * the pure serialisation covered here.
 */

#include "inventory.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

    const QString id1 = QStringLiteral("11111111111111111111111111111111").left(32);

    out << "=== toJson: pure serialisation ===" << Qt::endl;
    {
        Root::Inventory::ShareRecord rec;
        rec.id = id1;
        rec.credentialApplicable = true;
        rec.credentialHealthy = false;

        const QString json = Root::Inventory::toJson({rec});
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
        check(QStringLiteral("produces valid JSON"), parseError.error == QJsonParseError::NoError,
              parseError.errorString());
        check(QStringLiteral("is a one-element array"), doc.isArray() && doc.array().size() == 1);
        if (doc.isArray() && doc.array().size() == 1) {
            const QJsonObject o = doc.array().at(0).toObject();
            check(QStringLiteral("id"), o.value(QStringLiteral("id")).toString() == id1);
            check(QStringLiteral("credentialApplicable"), o.value(QStringLiteral("credentialApplicable")).toBool());
            check(QStringLiteral("credentialHealthy is false"), !o.value(QStringLiteral("credentialHealthy")).toBool());
            check(QStringLiteral("exactly three keys -- the narrow shape"), o.size() == 3, QString::number(o.size()));
            check(QStringLiteral("no credential content leaks into JSON"),
                  !json.contains(QStringLiteral("password"), Qt::CaseInsensitive));
        }
    }

    out << "=== toJson: empty list ===" << Qt::endl;
    {
        const QString json = Root::Inventory::toJson({});
        check(QStringLiteral("empty array"), json == QStringLiteral("[]"), json);
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
