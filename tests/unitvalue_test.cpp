/*
 * Validation tests for UnitValue.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * These pin down the encoder's behaviour against what was verified empirically
 * on the real system (systemctl --user show on a loaded unit, not just
 * systemd-analyze verify — see unitvalue.h): outside a leading quote mark,
 * systemd's unit-file parser does not interpret backslashes at all, and '%'
 * is the only character that needs doubling.
 */

#include "unitvalue.h"

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

    out << "=== encodeUnitValue: percent doubling ===" << Qt::endl;
    {
        QString encoded, error;
        check(QStringLiteral("literal percent doubled"),
              UnitValue::encodeUnitValue(QStringLiteral("/mnt/100%done"), &encoded, &error)
                  && encoded == QStringLiteral("/mnt/100%%done"),
              encoded);
        check(QStringLiteral("%h-shaped value doubled, not left as a specifier"),
              UnitValue::encodeUnitValue(QStringLiteral("/mnt/a%hb"), &encoded, &error)
                  && encoded == QStringLiteral("/mnt/a%%hb"),
              encoded);
    }

    out << "=== encodeUnitValue: backslashes and embedded quotes pass through ===" << Qt::endl;
    {
        // Confirmed on the real system: outside a leading quote mark these are
        // not interpreted at all, so the encoder must not touch them.
        QString encoded, error;
        check(QStringLiteral("internal backslash preserved"),
              UnitValue::encodeUnitValue(QStringLiteral("//host/Share\\with\\backslash"), &encoded, &error)
                  && encoded == QStringLiteral("//host/Share\\with\\backslash"),
              encoded);
        check(QStringLiteral("embedded quote preserved"),
              UnitValue::encodeUnitValue(QStringLiteral("comment=has\"quote'here"), &encoded, &error)
                  && encoded == QStringLiteral("comment=has\"quote'here"),
              encoded);
    }

    out << "=== encodeUnitValue: rejections ===" << Qt::endl;
    {
        QString encoded, error;
        check(QStringLiteral("control character rejected"),
              !UnitValue::encodeUnitValue(QStringLiteral("/mnt/a\nb"), &encoded, &error), error);
        check(QStringLiteral("leading whitespace rejected"),
              !UnitValue::encodeUnitValue(QStringLiteral(" /mnt/a"), &encoded, &error), error);
        check(QStringLiteral("trailing whitespace rejected"),
              !UnitValue::encodeUnitValue(QStringLiteral("/mnt/a "), &encoded, &error), error);
        check(QStringLiteral("trailing backslash rejected (line continuation)"),
              !UnitValue::encodeUnitValue(QStringLiteral("/mnt/a\\"), &encoded, &error), error);
        check(QStringLiteral("leading double quote rejected"),
              !UnitValue::encodeUnitValue(QStringLiteral("\"/mnt/a"), &encoded, &error), error);
        check(QStringLiteral("leading single quote rejected"),
              !UnitValue::encodeUnitValue(QStringLiteral("'/mnt/a"), &encoded, &error), error);
        check(QStringLiteral("empty value rejected"), !UnitValue::encodeUnitValue(QString(), &encoded, &error),
              error);
    }

    out << "=== unitPathsFor: agrees with systemd-escape, suffix is just appended ===" << Qt::endl;
    {
        UnitValue::UnitPaths paths;
        QString error;
        const QString tricky = QStringLiteral("/tmp/does not-exist_100%h-xyz");
        check(QStringLiteral("unitPathsFor succeeds"), UnitValue::unitPathsFor(tricky, &paths, &error), error);
        check(QStringLiteral("mount path is unitName + .mount"),
              paths.mountUnitPath == QStringLiteral("/etc/systemd/system/%1.mount").arg(paths.unitName),
              paths.mountUnitPath);
        check(QStringLiteral("automount path is unitName + .automount"),
              paths.automountUnitPath == QStringLiteral("/etc/systemd/system/%1.automount").arg(paths.unitName),
              paths.automountUnitPath);
    }

    out << "=== isValidShareId ===" << Qt::endl;
    {
        check(QStringLiteral("32 lowercase hex accepted"),
              UnitValue::isValidShareId(QStringLiteral("0123456789abcdef0123456789abcdef")));
        check(QStringLiteral("uppercase hex rejected"),
              !UnitValue::isValidShareId(QStringLiteral("0123456789ABCDEF0123456789abcdef")));
        check(QStringLiteral("too short rejected"), !UnitValue::isValidShareId(QStringLiteral("deadbeef")));
        check(QStringLiteral("too long rejected"),
              !UnitValue::isValidShareId(QStringLiteral("0123456789abcdef0123456789abcdef00")));
        check(QStringLiteral("non-hex character rejected"),
              !UnitValue::isValidShareId(QStringLiteral("0123456789abcdef0123456789abcdeg")));
        check(QStringLiteral("empty rejected"), !UnitValue::isValidShareId(QString()));
    }

    out << "=== marker v2: round trip, every authentication kind x access mode ===" << Qt::endl;
    {
        const uid_t uid = ::getuid();
        const gid_t gid = ::getgid();
        const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");

        const struct {
            UnitValue::AuthenticationKind auth;
            const char *label;
        } combos[] = {
            {UnitValue::AuthenticationKind::Credentials, "credentials"},
            {UnitValue::AuthenticationKind::Guest, "guest"},
        };
        const struct {
            UnitValue::AccessMode access;
            const char *label;
        } accessModes[] = {
            {UnitValue::AccessMode::ReadWrite, "readwrite"},
            {UnitValue::AccessMode::ReadOnly, "readonly"},
            {UnitValue::AccessMode::ReadWriteExecutable, "readwrite-executable"},
        };
        for (const auto &combo : combos) {
            for (const auto &mode : accessModes) {
                const QString label =
                    QStringLiteral("%1/%2").arg(QLatin1String(combo.label), QLatin1String(mode.label));
                UnitValue::Marker in;
                in.ownerUid = uid;
                in.ownerGid = gid;
                in.id = id;
                in.authentication = combo.auth;
                in.access = mode.access;

                const QString content = UnitValue::markerComment(in);
                check(QStringLiteral("%1: marker is detected").arg(label), UnitValue::hasMarker(content));

                UnitValue::Marker out;
                QString error;
                check(QStringLiteral("%1: marker parses back").arg(label),
                      UnitValue::parseMarker(content, &out, &error), error);
                check(QStringLiteral("%1: round-trips exactly").arg(label), out == in);
            }
        }

        check(QStringLiteral("ordinary unit content has no marker"),
              !UnitValue::hasMarker(QStringLiteral("[Unit]\nDescription=something else\n")));
    }

    out << "=== marker v2: the access field is optional on read, omitted on write ===" << Qt::endl;
    {
        // This block is the upgrade gate in miniature. If any of it fails,
        // every unit pair written by 0.1.0-0.1.3 becomes Tampered on upgrade
        // and is silently unarmed at the next boot; goldenunits_test is the
        // whole-file version of the same check.
        UnitValue::Marker base;
        base.ownerUid = 1000;
        base.ownerGid = 1000;
        base.id = QStringLiteral("0123456789abcdef0123456789abcdef");
        base.authentication = UnitValue::AuthenticationKind::Credentials;

        // The literal pre-change block, spelled out rather than derived, so a
        // change to markerComment() cannot redefine its own expected output.
        const QString frozenReadWriteBlock =
            QStringLiteral("# X-Nasmount-Managed=1\n"
                           "# X-Nasmount-Owner-Uid=1000\n"
                           "# X-Nasmount-Owner-Gid=1000\n"
                           "# X-Nasmount-Id=0123456789abcdef0123456789abcdef\n"
                           "# X-Nasmount-Mode=system\n"
                           "# X-Nasmount-Authentication=credentials\n");
        check(QStringLiteral("ReadWrite emits the pre-change block byte-for-byte"),
              UnitValue::markerComment(base) == frozenReadWriteBlock, UnitValue::markerComment(base));

        UnitValue::Marker readOnly = base;
        readOnly.access = UnitValue::AccessMode::ReadOnly;
        check(QStringLiteral("ReadOnly appends exactly one line"),
              UnitValue::markerComment(readOnly)
                  == frozenReadWriteBlock + QStringLiteral("# X-Nasmount-Access=readonly\n"),
              UnitValue::markerComment(readOnly));

        UnitValue::Marker executable = base;
        executable.access = UnitValue::AccessMode::ReadWriteExecutable;
        check(QStringLiteral("ReadWriteExecutable appends exactly one line"),
              UnitValue::markerComment(executable)
                  == frozenReadWriteBlock
                      + QStringLiteral("# X-Nasmount-Access=readwrite-executable\n"),
              UnitValue::markerComment(executable));

        // A marker with no Access line at all -- i.e. every unit already on a
        // user's disk -- must parse, and must mean ReadWrite.
        UnitValue::Marker parsed;
        QString error;
        check(QStringLiteral("a marker with no Access line still parses"),
              UnitValue::parseMarker(frozenReadWriteBlock, &parsed, &error), error);
        check(QStringLiteral("an absent Access line means ReadWrite"),
              parsed.access == UnitValue::AccessMode::ReadWrite);
        check(QStringLiteral("an absent Access line round-trips to the default marker"), parsed == base);

        // Generation never writes it, but accepting it costs nothing and
        // avoids a needless asymmetry.
        UnitValue::Marker explicitRw;
        check(QStringLiteral("an explicit Access=readwrite is accepted"),
              UnitValue::parseMarker(
                  frozenReadWriteBlock + QStringLiteral("# X-Nasmount-Access=readwrite\n"),
                  &explicitRw, &error),
              error);
        check(QStringLiteral("an explicit Access=readwrite means ReadWrite"),
              explicitRw.access == UnitValue::AccessMode::ReadWrite);

        // The comparison that makes a pair disagreeing on access Tampered.
        check(QStringLiteral("markers differing only in access compare unequal"), !(base == readOnly));
        check(QStringLiteral("readonly and executable compare unequal"), !(readOnly == executable));
        check(QStringLiteral("operator!= agrees"), base != executable);
    }

    out << "=== marker v2: embedded in a full unit file, other lines ignored ===" << Qt::endl;
    {
        UnitValue::Marker in;
        in.ownerUid = 1000;
        in.ownerGid = 1000;
        in.id = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        in.authentication = UnitValue::AuthenticationKind::Credentials;
        const QString content = QStringLiteral("# Managed by nasmount — do not edit by hand.\n%1"
                                                "[Unit]\nDescription=nasmount: //host/share\n\n"
                                                "[Mount]\nWhat=//host/share\nWhere=/mnt/x\n")
                                     .arg(UnitValue::markerComment(in));
        UnitValue::Marker out;
        QString error;
        check(QStringLiteral("parses out of a full unit file"), UnitValue::parseMarker(content, &out, &error),
              error);
        check(QStringLiteral("round-trips out of a full unit file"), out == in);
    }

    out << "=== marker v2: rejection matrix (every deviation is Tampered, not parsed) ===" << Qt::endl;
    {
        const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
        auto validLines = [&]() {
            return QStringList{
                QStringLiteral("# X-Nasmount-Managed=1"),
                QStringLiteral("# X-Nasmount-Owner-Uid=1000"),
                QStringLiteral("# X-Nasmount-Owner-Gid=1000"),
                QStringLiteral("# X-Nasmount-Id=%1").arg(id),
                QStringLiteral("# X-Nasmount-Mode=system"),
                QStringLiteral("# X-Nasmount-Authentication=credentials"),
            };
        };
        auto expectRejected = [&](const QString &label, const QStringList &lines) {
            UnitValue::Marker out;
            QString error;
            const bool parsed = UnitValue::parseMarker(lines.join(QLatin1Char('\n')), &out, &error);
            check(label, !parsed, parsed ? QStringLiteral("unexpectedly parsed") : error);
        };

        check(QStringLiteral("baseline: the valid marker actually parses"), [&] {
            UnitValue::Marker out;
            QString error;
            return UnitValue::parseMarker(validLines().join(QLatin1Char('\n')), &out, &error);
        }());

        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Owner-Uid=2000");
            expectRejected(QStringLiteral("duplicate field rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines.removeAt(2); // Owner-Gid
            expectRejected(QStringLiteral("missing field rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Credential-Id=%1").arg(id);
            expectRejected(QStringLiteral("unknown field rejected (old Credential-Id spelling not parsed)"), lines);
        }
        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Bogus=1");
            expectRejected(QStringLiteral("unrecognised field in managed namespace rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[1] = QStringLiteral("# X-Nasmount-Owner-Uid=not-a-number");
            expectRejected(QStringLiteral("invalid Owner-Uid integer rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[1] = QStringLiteral("# X-Nasmount-Owner-Uid=-1");
            expectRejected(QStringLiteral("negative Owner-Uid rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[2] = QStringLiteral("# X-Nasmount-Owner-Gid=abc");
            expectRejected(QStringLiteral("invalid Owner-Gid integer rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[3] = QStringLiteral("# X-Nasmount-Id=tooshort");
            expectRejected(QStringLiteral("invalid id (too short) rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[3] = QStringLiteral("# X-Nasmount-Id=%1").arg(QStringLiteral("A").repeated(32));
            expectRejected(QStringLiteral("invalid id (uppercase hex) rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[4] = QStringLiteral("# X-Nasmount-Mode=both");
            expectRejected(QStringLiteral("unknown Mode enum value rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[5] = QStringLiteral("# X-Nasmount-Authentication=anonymous");
            expectRejected(QStringLiteral("unknown Authentication enum value rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines[0] = QStringLiteral("# X-Nasmount-Managed=2");
            expectRejected(QStringLiteral("Managed value other than 1 rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Access=bogus");
            expectRejected(QStringLiteral("malformed Access value rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Access=ro");
            expectRejected(QStringLiteral("Access value outside the closed vocabulary rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Access=readonly")
                  << QStringLiteral("# X-Nasmount-Access=readonly");
            expectRejected(QStringLiteral("duplicate Access field rejected"), lines);
        }
        {
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Access=readonly")
                  << QStringLiteral("# X-Nasmount-Access=readwrite-executable");
            expectRejected(QStringLiteral("conflicting duplicate Access fields rejected"), lines);
        }
        {
            // The optional field must be optional in the *only* direction
            // that is safe: absent is fine, present-but-wrong never is.
            QStringList lines = validLines();
            lines << QStringLiteral("# X-Nasmount-Access=readwrite-executable");
            UnitValue::Marker out;
            QString error;
            check(QStringLiteral("a well-formed Access line parses"),
                  UnitValue::parseMarker(lines.join(QLatin1Char('\n')), &out, &error), error);
            check(QStringLiteral("and yields the mode it names"),
                  out.access == UnitValue::AccessMode::ReadWriteExecutable);
        }
        {
            // hasMarker() must still see this as "ours", just broken — never
            // silently treated as an ordinary foreign unit.
            QStringList lines = validLines();
            lines.removeAt(2);
            const QString content = lines.join(QLatin1Char('\n'));
            check(QStringLiteral("a broken-but-present marker still trips hasMarker()"),
                  UnitValue::hasMarker(content));
        }
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
