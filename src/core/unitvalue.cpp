/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unitvalue.h"
#include "unitspec.h"

#include <QProcess>
#include <QRegularExpression>
#include <QSet>

namespace UnitValue
{

bool encodeUnitValue(const QString &value, QString *encoded, QString *error)
{
    if (UnitSpec::hasControlChars(value)) {
        *error = QStringLiteral("value contains control characters");
        return false;
    }
    if (value != value.trimmed()) {
        *error = QStringLiteral("value may not begin or end with whitespace");
        return false;
    }
    if (value.isEmpty()) {
        *error = QStringLiteral("value is empty");
        return false;
    }
    // A trailing backslash merges this line with the next (systemd.syntax(7));
    // trimmed() above already proved the last character is not a plain space.
    if (value.endsWith(QLatin1Char('\\'))) {
        *error = QStringLiteral("value may not end with a backslash");
        return false;
    }
    // A leading quote mark would put the parser into quoted mode, where
    // C-style escapes and early termination at the matching quote apply.
    // Nothing this tool generates legitimately starts with one.
    if (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))) {
        *error = QStringLiteral("value may not begin with a quote mark");
        return false;
    }

    QString out = value;
    out.replace(QLatin1Char('%'), QStringLiteral("%%"));
    *encoded = out;
    return true;
}

bool unitPathsFor(const QString &mountPoint, UnitPaths *paths, QString *error)
{
    QProcess proc;
    proc.start(QStringLiteral("systemd-escape"), {QStringLiteral("-p"), mountPoint});
    if (!proc.waitForFinished(10000) || proc.exitCode() != 0) {
        *error = QStringLiteral("systemd-escape failed: %1")
                     .arg(QString::fromLocal8Bit(proc.readAllStandardError()).trimmed());
        return false;
    }
    const QString name = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
    if (name.isEmpty()) {
        *error = QStringLiteral("systemd-escape produced an empty name");
        return false;
    }
    paths->unitName = name;
    paths->mountUnitPath = QStringLiteral("/etc/systemd/system/%1.mount").arg(name);
    paths->automountUnitPath = QStringLiteral("/etc/systemd/system/%1.automount").arg(name);
    return true;
}

namespace
{

const QLatin1String MarkerPrefix("# X-Nasmount-");

bool isValidShareIdImpl(const QString &id)
{
    if (id.size() != 32) {
        return false;
    }
    for (const QChar c : id) {
        const ushort u = c.unicode();
        const bool digit = (u >= '0' && u <= '9');
        const bool lowerHex = (u >= 'a' && u <= 'f');
        if (!digit && !lowerHex) {
            return false;
        }
    }
    return true;
}

/** One managed-namespace field: its key, the regex capturing its whole line
 *  (anchored, so a line either matches exactly or is rejected outright), and
 *  whether parseMarker() insists on seeing it.
 *
 *  `required` exists for exactly one field, `Access`, and the exception is
 *  load-bearing rather than a convenience -- see markerComment()'s doc block.
 *  A field may be optional-on-read only if its absence reproduces the exact
 *  pre-existing byte output; anything else silently turns every unit already
 *  on disk into Tampered at the next upgrade. */
struct FieldSpec {
    QLatin1String key;
    QRegularExpression pattern;
    bool required = true;
};

const QList<FieldSpec> &fieldSpecs()
{
    static const QList<FieldSpec> specs = {
        {QLatin1String("Managed"), QRegularExpression(QStringLiteral("^# X-Nasmount-Managed=(1)$"))},
        {QLatin1String("Owner-Uid"),
         QRegularExpression(QStringLiteral("^# X-Nasmount-Owner-Uid=([0-9]+)$"))},
        {QLatin1String("Owner-Gid"),
         QRegularExpression(QStringLiteral("^# X-Nasmount-Owner-Gid=([0-9]+)$"))},
        {QLatin1String("Id"), QRegularExpression(QStringLiteral("^# X-Nasmount-Id=([0-9a-f]{32})$"))},
        // Only `system` is legal. There is one lifecycle, so this field is a
        // constant -- kept in the marker because a unit that does not carry it
        // exactly is not one of ours, and validation says so rather than
        // guessing.
        {QLatin1String("Mode"),
         QRegularExpression(QStringLiteral("^# X-Nasmount-Mode=(system)$"))},
        {QLatin1String("Authentication"),
         QRegularExpression(QStringLiteral("^# X-Nasmount-Authentication=(credentials|guest)$"))},
        // The one optional field. Alternatives are ordered longest-first so
        // the match never depends on backtracking off the `$` anchor.
        {QLatin1String("Access"),
         QRegularExpression(
             QStringLiteral("^# X-Nasmount-Access=(readwrite-executable|readwrite|readonly)$")),
         /*required=*/false},
    };
    return specs;
}

} // namespace

bool isValidShareId(const QString &id)
{
    return isValidShareIdImpl(id);
}

QString accessModeToString(AccessMode access)
{
    switch (access) {
    case AccessMode::ReadOnly:
        return QStringLiteral("readonly");
    case AccessMode::ReadWriteExecutable:
        return QStringLiteral("readwrite-executable");
    case AccessMode::ReadWrite:
        break;
    }
    return QStringLiteral("readwrite");
}

bool accessModeFromString(const QString &text, AccessMode *access)
{
    if (text == QStringLiteral("readwrite")) {
        *access = AccessMode::ReadWrite;
    } else if (text == QStringLiteral("readonly")) {
        *access = AccessMode::ReadOnly;
    } else if (text == QStringLiteral("readwrite-executable")) {
        *access = AccessMode::ReadWriteExecutable;
    } else {
        return false;
    }
    return true;
}

QString markerComment(const Marker &marker)
{
    Q_ASSERT(isValidShareIdImpl(marker.id));
    QString block = QStringLiteral("# X-Nasmount-Managed=1\n"
                                   "# X-Nasmount-Owner-Uid=%1\n"
                                   "# X-Nasmount-Owner-Gid=%2\n"
                                   "# X-Nasmount-Id=%3\n"
                                   "# X-Nasmount-Mode=system\n"
                                   "# X-Nasmount-Authentication=%4\n")
                        .arg(marker.ownerUid)
                        .arg(marker.ownerGid)
                        .arg(marker.id,
                             marker.authentication == AuthenticationKind::Guest
                                 ? QStringLiteral("guest")
                                 : QStringLiteral("credentials"));
    // Nothing is emitted for ReadWrite. That is not a shortcut: it is what
    // keeps a read-write share's bytes identical to what 0.1.0-0.1.3 wrote,
    // so the frozen corpus still regenerates exactly and no existing unit
    // pair becomes Tampered on upgrade. Do not "normalise" this by always
    // writing the line.
    switch (marker.access) {
    case AccessMode::ReadWrite:
        break;
    case AccessMode::ReadOnly:
        block += QStringLiteral("# X-Nasmount-Access=readonly\n");
        break;
    case AccessMode::ReadWriteExecutable:
        block += QStringLiteral("# X-Nasmount-Access=readwrite-executable\n");
        break;
    }
    return block;
}

bool hasMarker(const QString &unitFileContent)
{
    for (const QString &line : unitFileContent.split(QLatin1Char('\n'))) {
        if (line.startsWith(MarkerPrefix)) {
            return true;
        }
    }
    return false;
}

bool parseMarker(const QString &unitFileContent, Marker *marker, QString *error)
{
    QSet<QString> seen;
    QString ownerUidValue, ownerGidValue, idValue, authValue, accessValue;

    for (const QString &line : unitFileContent.split(QLatin1Char('\n'))) {
        if (!line.startsWith(MarkerPrefix)) {
            continue;
        }
        bool matched = false;
        for (const FieldSpec &spec : fieldSpecs()) {
            const auto m = spec.pattern.match(line);
            if (!m.hasMatch()) {
                continue;
            }
            matched = true;
            if (seen.contains(spec.key)) {
                *error = QStringLiteral("duplicate marker field: %1").arg(spec.key);
                return false;
            }
            seen.insert(spec.key);
            const QString value = m.captured(1);
            if (spec.key == QLatin1String("Owner-Uid")) {
                ownerUidValue = value;
            } else if (spec.key == QLatin1String("Owner-Gid")) {
                ownerGidValue = value;
            } else if (spec.key == QLatin1String("Id")) {
                idValue = value;
            } else if (spec.key == QLatin1String("Authentication")) {
                authValue = value;
            } else if (spec.key == QLatin1String("Access")) {
                accessValue = value;
            }
            break;
        }
        if (!matched) {
            *error = QStringLiteral("unknown or malformed marker field: %1").arg(line);
            return false;
        }
    }

    for (const FieldSpec &spec : fieldSpecs()) {
        if (spec.required && !seen.contains(spec.key)) {
            *error = QStringLiteral("missing marker field: %1").arg(spec.key);
            return false;
        }
    }

    bool ok = false;
    const uint uidValue = ownerUidValue.toUInt(&ok);
    if (!ok) {
        *error = QStringLiteral("invalid Owner-Uid");
        return false;
    }
    const uint gidValue = ownerGidValue.toUInt(&ok);
    if (!ok) {
        *error = QStringLiteral("invalid Owner-Gid");
        return false;
    }

    Marker result;
    result.ownerUid = static_cast<uid_t>(uidValue);
    result.ownerGid = static_cast<gid_t>(gidValue);
    result.id = idValue;
    result.authentication =
        (authValue == QStringLiteral("guest")) ? AuthenticationKind::Guest : AuthenticationKind::Credentials;
    // An absent Access line leaves accessValue empty and means ReadWrite --
    // that is the whole point of the field being optional. An explicit
    // `readwrite` is accepted too, even though markerComment() never writes
    // one: refusing it would be a needless asymmetry.
    //
    // Decoded through accessModeFromString() rather than a second if-chain so
    // the spelling lives in exactly one place. The regex above has already
    // restricted the value to the closed vocabulary, so the failure branch is
    // unreachable today; it is kept so that a future divergence between the
    // regex and the decoder fails closed rather than silently defaulting a
    // value the marker did contain.
    result.access = AccessMode::ReadWrite;
    if (!accessValue.isEmpty() && !accessModeFromString(accessValue, &result.access)) {
        *error = QStringLiteral("invalid Access");
        return false;
    }
    *marker = result;
    return true;
}

} // namespace UnitValue
