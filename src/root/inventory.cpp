/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "inventory.h"
#include "credentialstore.h"
#include "verify.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Root::Inventory
{

QList<ShareRecord> buildFor(uid_t uid, QString *error)
{
    Q_UNUSED(error); // nothing here can fail: unreadable entries are skipped, not errors
    QList<ShareRecord> records;
    for (const Verify::OwnedUnit &unit : Verify::enumerateOwnedUnits(uid)) {
        if (unit.id.isEmpty() || unit.state == Verify::Definition::Tampered) {
            continue; // an id that cannot be trusted has nothing this can look up
        }
        if (unit.authentication != UnitValue::AuthenticationKind::Credentials) {
            continue; // guest: never applicable, never checked
        }
        ShareRecord rec;
        rec.id = unit.id;
        rec.credentialApplicable = true;
        QString ignored;
        rec.credentialHealthy = CredentialStore::healthy(unit.id, &ignored);
        records.append(rec);
    }
    return records;
}

QString toJson(const QList<ShareRecord> &records)
{
    QJsonArray arr;
    for (const ShareRecord &rec : records) {
        QJsonObject o;
        o[QStringLiteral("id")] = rec.id;
        o[QStringLiteral("credentialApplicable")] = rec.credentialApplicable;
        o[QStringLiteral("credentialHealthy")] = rec.credentialHealthy;
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

} // namespace Root::Inventory
