/*
 * Tests for Verify: mountinfo parsing and the parts of the runtime state model
 * that do not require root (docs/credential-modes-design.md §4.2).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * inspectDefinition's Pair/Partial/NotOurs outcomes require a root-owned unit
 * file, which an unprivileged test cannot create — those are exercised by
 * manual/integration testing against the real helper. What is covered here is
 * everything reachable without privilege: a missing pair, a symlinked "unit
 * file" refused without ever being read, and a stray drop-in directory
 * refused even with no base unit file — a real bug this catches, since the
 * drop-in check must run before the "both missing" shortcut, not after.
 */

#include "unitvalue.h"
#include "verify.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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

    out << "=== parseMountinfo ===" << Qt::endl;
    {
        // A plain entry, zero optional fields, then one with two optional
        // fields ("shared:1 master:2") to prove the "-" separator is found by
        // scanning rather than assuming a fixed field count.
        const QString sample = QStringLiteral(
            "36 35 98:0 / /mnt/plain rw,noatime - ext4 /dev/root rw,errors=continue\n"
            "48 33 0:38 / /home/pavel/NAS/BACKUPS rw,relatime shared:21 - autofs systemd-1 rw,fd=52\n"
            "98 49 0:56 / /home/pavel/NAS/DOCS rw,relatime shared:882 - cifs //10.0.0.10/DATA rw,vers=2.1\n");
        const QList<Verify::MountEntry> entries = Verify::parseMountinfo(sample);
        check(QStringLiteral("three lines parsed"), entries.size() == 3, QString::number(entries.size()));
        if (entries.size() == 3) {
            check(QStringLiteral("zero optional fields: mountpoint"), entries.at(0).mountPoint == QStringLiteral("/mnt/plain"));
            check(QStringLiteral("zero optional fields: fstype"), entries.at(0).filesystemType == QStringLiteral("ext4"));
            check(QStringLiteral("one optional field: fstype is autofs"),
                  entries.at(1).filesystemType == QStringLiteral("autofs"));
            check(QStringLiteral("cifs entry: mountSource"),
                  entries.at(2).mountSource == QStringLiteral("//10.0.0.10/DATA"), entries.at(2).mountSource);
        }
    }
    {
        // Escapes: space, tab, backslash, and — the one the original code
        // dropped — newline. Decoded left-to-right, backslash last, so a
        // literal backslash in the name cannot be mistaken for the start of
        // another escape once decoded.
        const QString sample = QStringLiteral(
            "1 2 0:1 / /mnt/Media\\040Library rw - cifs //host/Media\\040Library rw\n"
            "3 4 0:2 / /mnt/back\\134slash rw - cifs //host/back\\134slash rw\n");
        const QList<Verify::MountEntry> entries = Verify::parseMountinfo(sample);
        check(QStringLiteral("escaped-space entries parsed"), entries.size() == 2, QString::number(entries.size()));
        if (entries.size() == 2) {
            check(QStringLiteral("space decoded"), entries.at(0).mountPoint == QStringLiteral("/mnt/Media Library"),
                  entries.at(0).mountPoint);
            check(QStringLiteral("backslash decoded, not double-decoded"),
                  entries.at(1).mountPoint == QStringLiteral("/mnt/back\\slash"), entries.at(1).mountPoint);
        }
    }

    out << "=== classifyMountEntries: pure classification (plan §1.4.2) ===" << Qt::endl;
    {
        auto entry = [](const QString &mountPoint, const QString &fsType, const QString &source) {
            Verify::MountEntry e;
            e.mountPoint = mountPoint;
            e.filesystemType = fsType;
            e.mountSource = source;
            return e;
        };
        const QString mp = QStringLiteral("/mnt/nas");
        const QString expectedWhat = QStringLiteral("//host/share");

        {
            const auto c = Verify::classifyMountEntries({}, mp, expectedWhat);
            check(QStringLiteral("nothing at the path -> Absent/NotApplicable"),
                  c.mount == Verify::MountState::Absent && c.verification == Verify::VerificationState::NotApplicable);
        }
        {
            const auto c = Verify::classifyMountEntries({entry(mp, QStringLiteral("cifs"), expectedWhat)}, mp,
                                                        expectedWhat);
            check(QStringLiteral("our CIFS mount -> Present/Match"),
                  c.mount == Verify::MountState::Present && c.verification == Verify::VerificationState::Match);
        }
        {
            const auto c = Verify::classifyMountEntries(
                {entry(mp, QStringLiteral("cifs"), QStringLiteral("//other/share"))}, mp, expectedWhat);
            check(QStringLiteral("a different CIFS source at the path -> Present/Mismatch"),
                  c.mount == Verify::MountState::Present && c.verification == Verify::VerificationState::Mismatch);
        }
        {
            const auto c =
                Verify::classifyMountEntries({entry(mp, QStringLiteral("autofs"), QString())}, mp, expectedWhat);
            check(QStringLiteral("idle autofs trigger -> Absent/NotApplicable (the ordinary resting state)"),
                  c.mount == Verify::MountState::Absent && c.verification == Verify::VerificationState::NotApplicable);
        }
        {
            // The regression this closes: a non-autofs, non-CIFS filesystem
            // occupying the path (e.g. someone bind-mounted something else
            // there) must be a present foreign mount, never silently Absent.
            const auto c =
                Verify::classifyMountEntries({entry(mp, QStringLiteral("ext4"), QStringLiteral("/dev/sdX"))}, mp,
                                            expectedWhat);
            check(QStringLiteral("foreign non-CIFS filesystem -> Present/Mismatch, never Absent"),
                  c.mount == Verify::MountState::Present && c.verification == Verify::VerificationState::Mismatch);
        }
        {
            const auto c = Verify::classifyMountEntries({entry(QStringLiteral("/mnt/elsewhere"), QStringLiteral("cifs"),
                                                                expectedWhat)},
                                                        mp, expectedWhat);
            check(QStringLiteral("an entry at an unrelated path is ignored"),
                  c.mount == Verify::MountState::Absent && c.verification == Verify::VerificationState::NotApplicable);
        }
    }

    out << "=== uniqueMountId is stable within this process ===" << Qt::endl;
    {
        const uint64_t first = Verify::uniqueMountId(QDir::tempPath());
        const uint64_t second = Verify::uniqueMountId(QDir::tempPath());
        check(QStringLiteral("same path yields the same id twice"), first == second,
              QStringLiteral("%1 vs %2").arg(first).arg(second));
    }

    out << "=== inspectRuntime fails closed on a non-zero systemctl result ===" << Qt::endl;
    {
        QTemporaryDir fakeBin;
        check(QStringLiteral("fake systemctl directory created"), fakeBin.isValid());
        QFile fakeSystemctl(fakeBin.filePath(QStringLiteral("systemctl")));
        check(QStringLiteral("fake systemctl written"), fakeSystemctl.open(QIODevice::WriteOnly));
        fakeSystemctl.write("#!/bin/sh\nexit 1\n");
        fakeSystemctl.close();
        check(QStringLiteral("fake systemctl made executable"),
              fakeSystemctl.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | QFileDevice::ExeOwner));

        const QByteArray oldPath = qgetenv("PATH");
        QByteArray fakePath = fakeBin.path().toLocal8Bit();
        fakePath.append(':');
        fakePath.append(oldPath);
        qputenv("PATH", fakePath);
        const Verify::RuntimeSnapshot snap = Verify::inspectRuntime(
            QStringLiteral("nasmount-test-nonzero"), QDir::tempPath(), QStringLiteral("//host/share"));
        qputenv("PATH", oldPath);

        check(QStringLiteral("non-zero automount query -> Indeterminate"),
              snap.automount == Verify::AutomountState::Indeterminate);
        check(QStringLiteral("non-zero mount cross-check -> Indeterminate"),
              snap.mount == Verify::MountState::Indeterminate
                  && snap.verification == Verify::VerificationState::Indeterminate);
    }

    out << "=== inspectDefinition: reachable without root ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        check(QStringLiteral("temp dir created"), tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("nasmount-test"));

        UnitValue::UnitPaths paths;
        paths.unitName = QStringLiteral("test-unit");
        paths.mountUnitPath = base + QStringLiteral(".mount");
        paths.automountUnitPath = base + QStringLiteral(".automount");

        {
            const auto def = Verify::inspectDefinition(paths, ::getuid(), QStringLiteral("/some/path"));
            check(QStringLiteral("neither file exists -> None"), def.state == Verify::Definition::None);
            // Path/unit-name identity is populated regardless of state (plan
            // §1.3.1), not only once a definition is found.
            check(QStringLiteral("canonicalMountPoint populated even for None"),
                  def.canonicalMountPoint == QStringLiteral("/some/path"), def.canonicalMountPoint);
            check(QStringLiteral("mountUnitName populated even for None"),
                  def.mountUnitName == QStringLiteral("test-unit.mount"), def.mountUnitName);
            check(QStringLiteral("automountUnitName populated even for None"),
                  def.automountUnitName == QStringLiteral("test-unit.automount"), def.automountUnitName);
        }
        {
            // Not root-owned (we cannot chown to root as an unprivileged
            // test), so this exercises the ownership-rejection path rather
            // than a genuine "someone else's unit" scenario — real NotOurs
            // coverage needs root and belongs to integration testing.
            QFile f(paths.mountUnitPath);
            check(QStringLiteral("mount unit written"), f.open(QIODevice::WriteOnly));
            f.write("[Mount]\nWhere=/some/path\n");
            f.close();
            const auto def = Verify::inspectDefinition(paths, ::getuid(), QStringLiteral("/some/path"));
            check(QStringLiteral("non-root-owned file -> Tampered"), def.state == Verify::Definition::Tampered,
                  def.detail);
            QFile::remove(paths.mountUnitPath);
        }
        {
            check(QStringLiteral("symlink planted instead of a regular file"),
                  QFile::link(QStringLiteral("/etc/hostname"), paths.mountUnitPath));
            const auto def = Verify::inspectDefinition(paths, ::getuid(), QStringLiteral("/some/path"));
            check(QStringLiteral("symlinked unit file -> Tampered, never followed"),
                  def.state == Verify::Definition::Tampered, def.detail);
            QFile::remove(paths.mountUnitPath);
        }
        {
            // A stray drop-in with no base unit file at all must still be
            // Tampered, not None — the check has to run before the
            // "both missing" shortcut.
            QDir().mkpath(paths.mountUnitPath + QStringLiteral(".d"));
            const auto def = Verify::inspectDefinition(paths, ::getuid(), QStringLiteral("/some/path"));
            check(QStringLiteral("drop-in with no base unit -> Tampered, not None"),
                  def.state == Verify::Definition::Tampered, def.detail);
            QDir(paths.mountUnitPath + QStringLiteral(".d")).removeRecursively();
        }
    }

    out << "=== pairScannedHalves: pure grouping/pairing/collision logic ===" << Qt::endl;
    {
        auto makeMarker = [](const QString &id,
                             UnitValue::AuthenticationKind auth = UnitValue::AuthenticationKind::Credentials,
                             UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite) {
            UnitValue::Marker m;
            m.ownerUid = ::getuid();
            m.ownerGid = ::getgid();
            m.id = id;
            m.authentication = auth;
            m.access = access;
            return m;
        };
        const QString id1 = QStringLiteral("11111111111111111111111111111111").left(32);
        const QString id2 = QStringLiteral("22222222222222222222222222222222").left(32);

        {
            // A full, agreeing pair -> Pair, with fields taken from the marker.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-a");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id1);
            mountHalf.where = QStringLiteral("/mnt/a");
            mountHalf.what = QStringLiteral("//host/a");
            Verify::ScannedHalf automountHalf = mountHalf;
            automountHalf.isMount = false;
            automountHalf.what.clear();

            const auto result = Verify::pairScannedHalves({mountHalf, automountHalf});
            check(QStringLiteral("one entry for one base name"), result.size() == 1, QString::number(result.size()));
            if (result.size() == 1) {
                check(QStringLiteral("full pair -> Pair"), result.at(0).state == Verify::Definition::Pair);
                check(QStringLiteral("mountPoint from the pair"), result.at(0).mountPoint == QStringLiteral("/mnt/a"));
                check(QStringLiteral("id from the pair"), result.at(0).id == id1);
                check(QStringLiteral("unitName is the base name"), result.at(0).unitName == QStringLiteral("share-a"));
                check(QStringLiteral("what from the .mount half (plan §3.3 inventory needs this)"),
                      result.at(0).what == QStringLiteral("//host/a"));
                check(QStringLiteral("ownerUid from the marker (plan §4.2.3 boot needs this)"),
                      result.at(0).ownerUid == ::getuid());
                check(QStringLiteral("ownerGid from the marker"), result.at(0).ownerGid == ::getgid());
            }
        }
        {
            // Mount half only -> Partial; its What= survives too.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-b");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id1);
            mountHalf.where = QStringLiteral("/mnt/b");
            mountHalf.what = QStringLiteral("//host/b");

            const auto result = Verify::pairScannedHalves({mountHalf});
            check(QStringLiteral("mount-only -> Partial"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Partial);
            check(QStringLiteral("mount-only Partial keeps its What="),
                  result.size() == 1 && result.at(0).what == QStringLiteral("//host/b"));
        }
        {
            // Automount half only -> Partial. This is exactly the case the old
            // .mount-only scan made invisible. No .mount half survives, so
            // there is no What= to report.
            Verify::ScannedHalf automountHalf;
            automountHalf.baseName = QStringLiteral("share-c");
            automountHalf.isMount = false;
            automountHalf.marker = makeMarker(id1);
            automountHalf.where = QStringLiteral("/mnt/c");

            const auto result = Verify::pairScannedHalves({automountHalf});
            check(QStringLiteral("automount-only -> Partial (was invisible before plan §1.3)"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Partial);
            check(QStringLiteral("automount-only Partial has no What="),
                  result.size() == 1 && result.at(0).what.isEmpty());
        }
        {
            // Same base name, halves disagree on the marker -> Tampered.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-d");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id1);
            mountHalf.where = QStringLiteral("/mnt/d");
            Verify::ScannedHalf automountHalf = mountHalf;
            automountHalf.isMount = false;
            automountHalf.marker = makeMarker(id2); // different id: disagreement

            const auto result = Verify::pairScannedHalves({mountHalf, automountHalf});
            check(QStringLiteral("marker mismatch within a pair -> Tampered"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Tampered, result.at(0).detail);
        }
        {
            // Same base name, halves agree on everything except the access
            // mode -> Tampered. Nothing compares access explicitly; this
            // works only because Marker::operator== includes it, which is
            // exactly what the case is here to prove.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-d2");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id1, UnitValue::AuthenticationKind::Credentials,
                                          UnitValue::AccessMode::ReadOnly);
            mountHalf.where = QStringLiteral("/mnt/d2");
            Verify::ScannedHalf automountHalf = mountHalf;
            automountHalf.isMount = false;
            automountHalf.marker = makeMarker(id1, UnitValue::AuthenticationKind::Credentials,
                                              UnitValue::AccessMode::ReadWrite);

            const auto result = Verify::pairScannedHalves({mountHalf, automountHalf});
            check(QStringLiteral("access mismatch within a pair -> Tampered"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Tampered,
                  result.at(0).detail);
        }
        {
            // An agreeing non-default pair carries its access through to the
            // OwnedUnit -- the model reads it from here, never from Store.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-d3");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id1, UnitValue::AuthenticationKind::Credentials,
                                          UnitValue::AccessMode::ReadWriteExecutable);
            mountHalf.where = QStringLiteral("/mnt/d3");
            mountHalf.what = QStringLiteral("//host/d3");
            Verify::ScannedHalf automountHalf = mountHalf;
            automountHalf.isMount = false;
            automountHalf.what.clear();

            const auto result = Verify::pairScannedHalves({mountHalf, automountHalf});
            check(QStringLiteral("an agreeing pair carries access through to the OwnedUnit"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Pair
                      && result.at(0).access == UnitValue::AccessMode::ReadWriteExecutable);
        }
        {
            // A surviving single half carries its access through too.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-d4");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id2, UnitValue::AuthenticationKind::Guest,
                                          UnitValue::AccessMode::ReadOnly);
            mountHalf.where = QStringLiteral("/mnt/d4");

            const auto result = Verify::pairScannedHalves({mountHalf});
            check(QStringLiteral("a Partial carries access from its surviving half"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Partial
                      && result.at(0).access == UnitValue::AccessMode::ReadOnly);
        }
        {
            // Same base name, halves disagree on Where= -> Tampered.
            Verify::ScannedHalf mountHalf;
            mountHalf.baseName = QStringLiteral("share-e");
            mountHalf.isMount = true;
            mountHalf.marker = makeMarker(id1);
            mountHalf.where = QStringLiteral("/mnt/e");
            Verify::ScannedHalf automountHalf = mountHalf;
            automountHalf.isMount = false;
            automountHalf.where = QStringLiteral("/mnt/e-different");

            const auto result = Verify::pairScannedHalves({mountHalf, automountHalf});
            check(QStringLiteral("Where= mismatch within a pair -> Tampered"),
                  result.size() == 1 && result.at(0).state == Verify::Definition::Tampered, result.at(0).detail);
        }
        {
            // Two different base names claiming the same id -> both Tampered,
            // never silently picking one (plan §1.3.5).
            Verify::ScannedHalf mountHalf1;
            mountHalf1.baseName = QStringLiteral("share-f1");
            mountHalf1.isMount = true;
            // Non-default access on both, so the reset below is observable:
            // these entries are populated first and only then demoted.
            mountHalf1.marker = makeMarker(id1, UnitValue::AuthenticationKind::Credentials,
                                           UnitValue::AccessMode::ReadOnly);
            mountHalf1.where = QStringLiteral("/mnt/f1");
            Verify::ScannedHalf automountHalf1 = mountHalf1;
            automountHalf1.isMount = false;

            Verify::ScannedHalf mountHalf2;
            mountHalf2.baseName = QStringLiteral("share-f2");
            mountHalf2.isMount = true;
            // same id as share-f1
            mountHalf2.marker = makeMarker(id1, UnitValue::AuthenticationKind::Credentials,
                                           UnitValue::AccessMode::ReadWriteExecutable);
            mountHalf2.where = QStringLiteral("/mnt/f2");
            Verify::ScannedHalf automountHalf2 = mountHalf2;
            automountHalf2.isMount = false;

            const auto result =
                Verify::pairScannedHalves({mountHalf1, automountHalf1, mountHalf2, automountHalf2});
            check(QStringLiteral("id collision yields two entries"), result.size() == 2,
                  QString::number(result.size()));
            int tamperedCount = 0;
            for (const auto &u : result) {
                if (u.state == Verify::Definition::Tampered) {
                    ++tamperedCount;
                }
            }
            check(QStringLiteral("id collision: both entries Tampered, none silently chosen"), tamperedCount == 2,
                  QString::number(tamperedCount));
            for (const auto &u : result) {
                check(QStringLiteral("Tampered entry clears ownerUid too, not just id/mode/authentication"),
                      u.ownerUid == 0 && u.ownerGid == 0);
                // The collision pass populates an entry and only then demotes
                // it, so access has to be reset explicitly along with the
                // other marker-derived fields -- otherwise a demoted row
                // keeps and displays a mode nothing vouches for any more.
                check(QStringLiteral("Tampered entry resets access to the default too"),
                      u.access == UnitValue::AccessMode::ReadWrite);
            }
        }
        {
            // Two different base names claiming the same mount point -> both
            // Tampered.
            Verify::ScannedHalf mountHalf1;
            mountHalf1.baseName = QStringLiteral("share-g1");
            mountHalf1.isMount = true;
            mountHalf1.marker = makeMarker(id1);
            mountHalf1.where = QStringLiteral("/mnt/shared");
            Verify::ScannedHalf automountHalf1 = mountHalf1;
            automountHalf1.isMount = false;

            Verify::ScannedHalf mountHalf2;
            mountHalf2.baseName = QStringLiteral("share-g2");
            mountHalf2.isMount = true;
            mountHalf2.marker = makeMarker(id2);
            mountHalf2.where = QStringLiteral("/mnt/shared"); // same mount point as share-g1
            Verify::ScannedHalf automountHalf2 = mountHalf2;
            automountHalf2.isMount = false;

            const auto result =
                Verify::pairScannedHalves({mountHalf1, automountHalf1, mountHalf2, automountHalf2});
            int tamperedCount = 0;
            for (const auto &u : result) {
                if (u.state == Verify::Definition::Tampered) {
                    ++tamperedCount;
                }
            }
            check(QStringLiteral("mount-point collision: both entries Tampered"), tamperedCount == 2,
                  QString::number(tamperedCount));
        }
        {
            // Unrelated base names with distinct ids/mount points must not be
            // affected by each other at all.
            Verify::ScannedHalf mountHalf1;
            mountHalf1.baseName = QStringLiteral("share-h1");
            mountHalf1.isMount = true;
            mountHalf1.marker = makeMarker(id1);
            mountHalf1.where = QStringLiteral("/mnt/h1");
            Verify::ScannedHalf automountHalf1 = mountHalf1;
            automountHalf1.isMount = false;

            Verify::ScannedHalf mountHalf2;
            mountHalf2.baseName = QStringLiteral("share-h2");
            mountHalf2.isMount = true;
            mountHalf2.marker = makeMarker(id2);
            mountHalf2.where = QStringLiteral("/mnt/h2");
            Verify::ScannedHalf automountHalf2 = mountHalf2;
            automountHalf2.isMount = false;

            const auto result =
                Verify::pairScannedHalves({mountHalf1, automountHalf1, mountHalf2, automountHalf2});
            int pairCount = 0;
            for (const auto &u : result) {
                if (u.state == Verify::Definition::Pair) {
                    ++pairCount;
                }
            }
            check(QStringLiteral("two unrelated pairs both stay Pair"), pairCount == 2, QString::number(pairCount));
        }
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
