/*
 * Frozen on-disk unit format gate — the upgrade-compatibility test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every other test proves that generation and validation agree with each
 * other *today*. That is not enough for a package that upgrades in place: a
 * share's `.mount`/`.automount` pair lives in /etc/systemd/system and survives
 * the upgrade, so the new binaries must still accept bytes the old ones wrote.
 * Generation and validation share the same fixed-value functions, so changing
 * one changes both in lockstep and the existing suite stays green while every
 * pre-existing share on every user's disk silently becomes Tampered — which
 * unarms it at the next boot.
 *
 * This gate is the only thing that catches that. The corpus under
 * tests/golden/units/<version>/ is a byte-for-byte record of what a released
 * version wrote. It is evidence, not a fixture: if this test fails, the fix is
 * to revert the format change or to design a migration and raise the DEB
 * preinst's MIN_UPGRADABLE_VERSION — never to regenerate the corpus.
 */

#include "unitspec.h"
#include "unitvalue.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTextStream>

#include <iterator>

namespace
{
int passed = 0;
int failed = 0;

void check(const QString &label, bool condition, const QString &detail = {})
{
    QTextStream out(stdout);
    out << (condition ? "  PASS  " : "  FAIL  ") << label;
    if (!detail.isEmpty()) {
        out << "   " << detail;
    }
    out << Qt::endl;
    condition ? ++passed : ++failed;
}

/**
 * One frozen share. The inputs are recorded here rather than parsed out of the
 * corpus directory on purpose: re-deriving them from the very files under test
 * would let a generation change quietly redefine its own expected output.
 */
struct Fixture {
    const char *base;
    uid_t ownerUid;
    gid_t ownerGid;
    const char *id;
    UnitValue::AuthenticationKind authentication;
    UnitValue::AccessMode access;
    const char *unc;
    const char *mountPoint;
};

/** v0.1.0: the default read-write bytes, unchanged from 0.1.0 through 0.1.3. */
const Fixture ReadWriteFixtures[] = {
    {"credentials", 1000, 1000, "0123456789abcdef0123456789abcdef",
     UnitValue::AuthenticationKind::Credentials, UnitValue::AccessMode::ReadWrite,
     "//nas.example.org/media", "/home/tester/mnt/media"},
    {"guest", 1000, 1000, "fedcba9876543210fedcba9876543210",
     UnitValue::AuthenticationKind::Guest, UnitValue::AccessMode::ReadWrite,
     "//nas.example.org/public", "/home/tester/mnt/public"},
};

/** v0.1.4: only the non-default access modes. The read-write bytes did not
 *  change in 0.1.4 -- that is the whole point of the omit-on-read-write rule
 *  -- so they stay frozen in v0.1.0 and are deliberately not duplicated here.
 *  The .automount halves are included even though nothing in [Automount]
 *  varies by access: both halves carry the marker, so both halves' bytes
 *  changed, and both have to be frozen. */
const Fixture NonDefaultAccessFixtures[] = {
    {"credentials-ro", 1000, 1000, "00112233445566778899aabbccddeeff",
     UnitValue::AuthenticationKind::Credentials, UnitValue::AccessMode::ReadOnly,
     "//nas.example.org/archive", "/home/tester/mnt/archive"},
    {"guest-ro", 1000, 1000, "ffeeddccbbaa99887766554433221100",
     UnitValue::AuthenticationKind::Guest, UnitValue::AccessMode::ReadOnly,
     "//nas.example.org/reference", "/home/tester/mnt/reference"},
    {"credentials-exec", 1000, 1000, "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
     UnitValue::AuthenticationKind::Credentials, UnitValue::AccessMode::ReadWriteExecutable,
     "//nas.example.org/steam", "/home/tester/mnt/steam"},
    {"guest-exec", 1000, 1000, "1a2b3c4d5e6f708192a3b4c5d6e7f809",
     UnitValue::AuthenticationKind::Guest, UnitValue::AccessMode::ReadWriteExecutable,
     "//nas.example.org/games", "/home/tester/mnt/games"},
};

/**
 * Corpus versions this build must still read, each with its own fixture set.
 *
 * Each version names its own fixtures rather than every version sharing one
 * list: a release freezes whatever bytes it actually changed, which is not
 * necessarily every share shape. v0.1.4 changed only the non-default access
 * modes, so duplicating the read-write pair into it would freeze the same
 * bytes twice and imply a change that did not happen.
 *
 * Add a directory here when a release changes the format; never remove one
 * that users may still be upgrading from.
 */
struct Corpus {
    const char *version;
    const Fixture *fixtures;
    size_t count;
};

const Corpus Corpora[] = {
    {"v0.1.0", ReadWriteFixtures, std::size(ReadWriteFixtures)},
    {"v0.1.4", NonDefaultAccessFixtures, std::size(NonDefaultAccessFixtures)},
};

bool readCorpusFile(const QString &version, const QString &name, QString *content)
{
    QFile file(QStringLiteral(NASMOUNT_GOLDEN_DIR "/") + version + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    *content = QString::fromUtf8(file.readAll());
    return true;
}

UnitValue::Marker markerFor(const Fixture &fixture)
{
    UnitValue::Marker marker;
    marker.ownerUid = fixture.ownerUid;
    marker.ownerGid = fixture.ownerGid;
    marker.id = QString::fromLatin1(fixture.id);
    marker.authentication = fixture.authentication;
    marker.access = fixture.access;
    return marker;
}

void checkHalf(const QString &version, const Fixture &fixture, bool isMount)
{
    const QString base = QString::fromLatin1(fixture.base);
    const QString name = base + (isMount ? QStringLiteral(".mount") : QStringLiteral(".automount"));
    const QString label = version + QLatin1Char('/') + name;
    const QString mountPoint = QString::fromLatin1(fixture.mountPoint);

    QString frozen;
    if (!readCorpusFile(version, name, &frozen)) {
        check(label + QStringLiteral(": corpus file readable"), false);
        return;
    }

    // 1. The marker still parses. parseMarker() rejects any unrecognised
    //    X-Nasmount-* line, so this fails in both directions: a key added to
    //    the schema breaks the frozen units, and a key removed from the frozen
    //    units breaks the schema.
    UnitValue::Marker parsed;
    QString error;
    const bool markerOk = UnitValue::parseMarker(frozen, &parsed, &error);
    check(label + QStringLiteral(": marker parses"), markerOk, error);
    check(label + QStringLiteral(": marker round-trips"), markerOk && parsed == markerFor(fixture));

    // 2. The body still validates. This is the check that decides Tampered
    //    versus Pair for a share already on disk.
    if (isMount) {
        QString what;
        const bool bodyOk = UnitSpec::validateMountUnitBody(frozen, markerFor(fixture), mountPoint,
                                                            &what, &error);
        check(label + QStringLiteral(": body validates"), bodyOk, error);
        check(label + QStringLiteral(": What= recovers"),
              bodyOk && what == QString::fromLatin1(fixture.unc), what);
    } else {
        const bool bodyOk = UnitSpec::validateAutomountUnitBody(frozen, markerFor(fixture),
                                                                mountPoint, &error);
        check(label + QStringLiteral(": body validates"), bodyOk, error);
    }

    // 3. Generation still reproduces the frozen bytes exactly. Checks 1 and 2
    //    would both survive a purely cosmetic change (Description= wording,
    //    comment text, directive order) that still rewrites every user's unit
    //    file on the next Delete-then-Add cycle; only this one does not.
    QString regenerated;
    const bool built = isMount
        ? UnitSpec::buildMountUnitContent(markerFor(fixture), QString::fromLatin1(fixture.unc),
                                          mountPoint, &regenerated, &error)
        : UnitSpec::buildAutomountUnitContent(markerFor(fixture), mountPoint, &regenerated, &error);
    check(label + QStringLiteral(": generation succeeds"), built, error);
    if (built && regenerated != frozen) {
        check(label + QStringLiteral(": generation is byte-identical"), false,
              QStringLiteral("format drifted; revert it or design a migration — do not "
                             "regenerate the corpus"));
    } else {
        check(label + QStringLiteral(": generation is byte-identical"), built);
    }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    for (const Corpus &corpus : Corpora) {
        for (size_t i = 0; i < corpus.count; ++i) {
            const Fixture &fixture = corpus.fixtures[i];
            checkHalf(QString::fromLatin1(corpus.version), fixture, /*isMount=*/true);
            checkHalf(QString::fromLatin1(corpus.version), fixture, /*isMount=*/false);
        }
    }

    QTextStream(stdout) << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
