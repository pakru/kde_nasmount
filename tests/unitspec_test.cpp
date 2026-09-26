/*
 * Validation tests for UnitSpec.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * These exercise the same functions the privileged helper calls, so the
 * security-relevant behaviour is covered directly rather than by proxy. The
 * symlink test plants a real symlink and checks it is refused.
 */

#include "unitspec.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <pwd.h>
#include <sys/stat.h>
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

/** Asserts a validator's verdict, and surfaces its message when it rejects. */
static void expectUnc(const QString &label, const QString &unc, bool shouldPass)
{
    QString normalised;
    QString error;
    const bool okResult = UnitSpec::validateUnc(unc, &normalised, &error);
    check(label, okResult == shouldPass, okResult ? normalised : error);
}

static void expectMountpoint(const QString &label, const QString &path,
                             const QString &home, bool shouldPass)
{
    UnitSpec::MountpointPlan plan;
    QString error;
    const bool okResult = UnitSpec::validateMountpoint(path, home, &plan, &error);
    check(label, okResult == shouldPass,
          okResult ? QStringLiteral("%1  [root=%2]").arg(plan.path, plan.root) : error);
}

/** Convenience wrapper: validate then walk, as the helper does. */
static int openPath(const QString &path, const QString &home, uid_t uid, gid_t gid, QString *error)
{
    UnitSpec::MountpointPlan plan;
    if (!UnitSpec::validateMountpoint(path, home, &plan, error)) {
        return -1;
    }
    return UnitSpec::openMountpointNoFollow(plan, uid, gid, error);
}

/** Convenience wrapper for the boot no-create walk. */
static int openPathNoCreate(const QString &path, const QString &home, uid_t uid, gid_t gid, QString *error)
{
    UnitSpec::MountpointPlan plan;
    if (!UnitSpec::validateMountpoint(path, home, &plan, error)) {
        return -1;
    }
    return UnitSpec::openMountpointNoCreate(plan, uid, gid, error);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const struct passwd *pw = ::getpwuid(::getuid());
    const QString home = QString::fromLocal8Bit(pw->pw_dir);
    const uid_t uid = pw->pw_uid;
    const gid_t gid = pw->pw_gid;

    out << "=== UNC validation ===" << Qt::endl;
    expectUnc(QStringLiteral("plain share"), QStringLiteral("//192.0.2.10/DATA"), true);
    expectUnc(QStringLiteral("share with spaces"), QStringLiteral("//192.0.2.10/Media Library"), true);
    expectUnc(QStringLiteral("subdirectory of a share"), QStringLiteral("//192.0.2.10/DATA/projects"), true);
    expectUnc(QStringLiteral("newline injection"),
              QStringLiteral("//192.0.2.10/D\nExecStart=/bin/sh"), false);
    expectUnc(QStringLiteral("missing leading slashes"), QStringLiteral("192.0.2.10/DATA"), false);
    expectUnc(QStringLiteral("host only, no share"), QStringLiteral("//192.0.2.10/"), false);
    expectUnc(QStringLiteral("dotdot in share"), QStringLiteral("//192.0.2.10/a/../../etc"), false);

    out << "=== mount point validation ===" << Qt::endl;
    expectMountpoint(QStringLiteral("under home"), home + QStringLiteral("/Documents"), home, true);
    expectMountpoint(QStringLiteral("under /mnt"), QStringLiteral("/mnt/nas"), home, true);
    expectMountpoint(QStringLiteral("/etc/systemd/system"), QStringLiteral("/etc/systemd/system"), home, false);
    expectMountpoint(QStringLiteral("traversal out of home"),
                     home + QStringLiteral("/../../etc/evil"), home, false);
    expectMountpoint(QStringLiteral("home itself"), home, home, false);
    expectMountpoint(QStringLiteral("relative path"), QStringLiteral("Documents"), home, false);
    expectMountpoint(QStringLiteral("newline in path"),
                     QStringLiteral("/mnt/a\nWhat=/dev/sda"), home, false);
    expectMountpoint(QStringLiteral("bare /"), QStringLiteral("/"), home, false);

    out << "=== control-character rejection ===" << Qt::endl;
    check(QStringLiteral("clean username"), !UnitSpec::hasControlChars(QStringLiteral("someuser")));
    check(QStringLiteral("newline in password"),
          UnitSpec::hasControlChars(QStringLiteral("pw\npassword=root")));

    QString error;

    out << "=== mount options are hardened ===" << Qt::endl;
    const QString opts =
        UnitSpec::mountOptions(uid, gid, QStringLiteral("/etc/nasmount/x.cred"),
                               UnitValue::AccessMode::ReadWrite);
    const QStringList optList = opts.split(QLatin1Char(','));
    for (const QString &flag : {QStringLiteral("nosuid"), QStringLiteral("nodev"),
                                QStringLiteral("forceuid"), QStringLiteral("forcegid")}) {
        check(QStringLiteral("%1 present").arg(flag), optList.contains(flag));
    }
    check(QStringLiteral("file_mode=0600"), optList.contains(QStringLiteral("file_mode=0600")));
    check(QStringLiteral("dir_mode=0700"), optList.contains(QStringLiteral("dir_mode=0700")));
    // Without nounix a server negotiating Unix extensions can supply its own
    // permissive modes, and there is no forcemode counterpart to forceuid.
    check(QStringLiteral("nounix present"), optList.contains(QStringLiteral("nounix")));
    out << "        " << opts << Qt::endl;

    out << "=== access mode selects ro and the permission bits, and nothing else ===" << Qt::endl;
    {
        const QString cred = QStringLiteral("/etc/nasmount/0123456789abcdef0123456789abcdef.cred");
        const QString readWrite =
            UnitSpec::mountOptions(1000, 1000, cred, UnitValue::AccessMode::ReadWrite);
        const QString readOnly =
            UnitSpec::mountOptions(1000, 1000, cred, UnitValue::AccessMode::ReadOnly);
        const QString executable =
            UnitSpec::mountOptions(1000, 1000, cred, UnitValue::AccessMode::ReadWriteExecutable);

        // The default is spelled out literally, not derived: this exact byte
        // string is what every release through 0.1.3 wrote, and changing it
        // turns every share already on disk into Tampered.
        const QString frozenReadWrite =
            QStringLiteral("credentials=/etc/nasmount/0123456789abcdef0123456789abcdef.cred,"
                           "nosuid,nodev,forceuid,forcegid,uid=1000,gid=1000,nounix,"
                           "iocharset=utf8,file_mode=0600,dir_mode=0700");
        check(QStringLiteral("read-write options are byte-identical to the frozen string"),
              readWrite == frozenReadWrite, readWrite);

        const QStringList roList = readOnly.split(QLatin1Char(','));
        check(QStringLiteral("read-only carries ro"), roList.contains(QStringLiteral("ro")));
        check(QStringLiteral("read-only file_mode=0400"),
              roList.contains(QStringLiteral("file_mode=0400")));
        // 0500, not 0400: a directory still needs +x to be traversable.
        check(QStringLiteral("read-only dir_mode=0500"),
              roList.contains(QStringLiteral("dir_mode=0500")));
        check(QStringLiteral("read-only never emits rw"), !roList.contains(QStringLiteral("rw")));

        const QStringList execList = executable.split(QLatin1Char(','));
        check(QStringLiteral("executable file_mode=0700"),
              execList.contains(QStringLiteral("file_mode=0700")));
        check(QStringLiteral("executable dir_mode=0700"),
              execList.contains(QStringLiteral("dir_mode=0700")));
        check(QStringLiteral("executable is not ro"), !execList.contains(QStringLiteral("ro")));
        check(QStringLiteral("executable never emits rw"), !execList.contains(QStringLiteral("rw")));

        // `rw` would be semantically identical and would rewrite every
        // existing share's bytes. It must never appear in any mode.
        const QStringList rwList = readWrite.split(QLatin1Char(','));
        check(QStringLiteral("read-write never emits rw"), !rwList.contains(QStringLiteral("rw")));
        check(QStringLiteral("read-write is not ro"), !rwList.contains(QStringLiteral("ro")));

        // The security-load-bearing terms are identical in all three modes;
        // access mode must never be a way to relax them.
        for (const QString &fixed : {QStringLiteral("nosuid"), QStringLiteral("nodev"),
                                     QStringLiteral("forceuid"), QStringLiteral("forcegid"),
                                     QStringLiteral("nounix")}) {
            check(QStringLiteral("%1 fixed across every access mode").arg(fixed),
                  rwList.contains(fixed) && roList.contains(fixed) && execList.contains(fixed));
        }

        check(QStringLiteral("the three modes are actually distinct"),
              readWrite != readOnly && readOnly != executable && readWrite != executable);
        out << "        ro:   " << readOnly << Qt::endl;
        out << "        exec: " << executable << Qt::endl;
    }

    out << "=== access is load-bearing: a body never validates against another mode ===" << Qt::endl;
    {
        // Everything else in this suite would still pass if `access` were
        // threaded into the marker and then dropped on the floor by
        // mountOptionsFor(). This is the case that would not: it proves the
        // generated Options= actually depends on the mode the marker records,
        // in both directions, for every ordered pair.
        const UnitValue::AccessMode modes[] = {UnitValue::AccessMode::ReadWrite,
                                               UnitValue::AccessMode::ReadOnly,
                                               UnitValue::AccessMode::ReadWriteExecutable};
        const char *const modeNames[] = {"readwrite", "readonly", "readwrite-executable"};
        const QString mountPoint = QStringLiteral("/home/tester/mnt/media");
        const QString unc = QStringLiteral("//nas.example.org/media");

        for (int i = 0; i < 3; ++i) {
            UnitValue::Marker generated;
            generated.ownerUid = 1000;
            generated.ownerGid = 1000;
            generated.id = QStringLiteral("0123456789abcdef0123456789abcdef");
            generated.authentication = UnitValue::AuthenticationKind::Credentials;
            generated.access = modes[i];

            QString content;
            QString buildError;
            const bool built =
                UnitSpec::buildMountUnitContent(generated, unc, mountPoint, &content, &buildError);
            check(QStringLiteral("%1: builds").arg(QLatin1String(modeNames[i])), built, buildError);
            if (!built) {
                continue;
            }

            for (int j = 0; j < 3; ++j) {
                UnitValue::Marker against = generated;
                against.access = modes[j];
                QString what;
                QString error;
                const bool valid =
                    UnitSpec::validateMountUnitBody(content, against, mountPoint, &what, &error);
                const QString label = QStringLiteral("%1 body vs %2 marker")
                                          .arg(QLatin1String(modeNames[i]), QLatin1String(modeNames[j]));
                if (i == j) {
                    check(label + QStringLiteral(": validates"), valid, error);
                } else {
                    check(label + QStringLiteral(": rejected"), !valid,
                          valid ? QStringLiteral("unexpectedly validated") : error);
                }
            }
        }
    }

    out << "=== allowlist cannot be escaped by aliasing an allowed root ===" << Qt::endl;
    {
        // Covers the bug where authorization used the canonicalized ancestor while
        // the walk used the lexical path: a symlink /tmp/alias -> /mnt made
        // /tmp/alias/share pass, after which swapping the symlink for a real
        // directory had root create a mount point outside every allowed root.
        UnitSpec::MountpointPlan plan;
        QString err;
        const bool accepted =
            UnitSpec::validateMountpoint(QStringLiteral("/tmp/alias/share"), home, &plan, &err);
        check(QStringLiteral("path outside allowed roots refused lexically"), !accepted, err);

        UnitSpec::MountpointPlan good;
        if (UnitSpec::validateMountpoint(home + QStringLiteral("/Share/Sub"), home, &good, &err)) {
            check(QStringLiteral("plan root is the allowed root"), good.root == home, good.root);
            check(QStringLiteral("suffix is relative to that root"),
                  good.suffix == QStringList({QStringLiteral("Share"), QStringLiteral("Sub")}),
                  good.suffix.join(QLatin1Char('/')));
            check(QStringLiteral("root + suffix reconstructs the authorized path"),
                  good.root + QLatin1Char('/') + good.suffix.join(QLatin1Char('/')) == good.path);
        } else {
            check(QStringLiteral("valid path accepted"), false, err);
        }
    }

    out << "=== mount points are canonicalised, so stored == what the helper writes ===" << Qt::endl;
    {
        // Regression: a mount point typed with a trailing slash was stored raw
        // by the frontend while the helper wrote the cleaned form as Where=.
        // Every later "does Where= agree with the mount point?" check then
        // failed, stranding a healthy share at NeedsAttention with all actions
        // refused. validateMountpoint must hand back one canonical string.
        UnitSpec::MountpointPlan a;
        UnitSpec::MountpointPlan b;
        QString err;
        const bool okTrailing =
            UnitSpec::validateMountpoint(home + QStringLiteral("/Share/"), home, &a, &err);
        const bool okClean = UnitSpec::validateMountpoint(home + QStringLiteral("/Share"), home, &b, &err);
        check(QStringLiteral("trailing-slash path accepted"), okTrailing, err);
        check(QStringLiteral("trailing slash canonicalised away"),
              okTrailing && okClean && a.path == b.path, a.path);
        check(QStringLiteral("no trailing slash survives in the plan"),
              okTrailing && !a.path.endsWith(QLatin1Char('/')), a.path);

        UnitSpec::MountpointPlan c;
        const bool okDouble =
            UnitSpec::validateMountpoint(home + QStringLiteral("//Share/./"), home, &c, &err);
        check(QStringLiteral("redundant separators and /./ collapsed"),
              okDouble && c.path == b.path, okDouble ? c.path : err);
    }

    out << "=== systemd value hazards rejected ===" << Qt::endl;
    expectMountpoint(QStringLiteral("trailing backslash (line continuation)"),
                     home + QStringLiteral("/name\\"), home, false);
    expectMountpoint(QStringLiteral("trailing space (stripped by systemd)"),
                     home + QStringLiteral("/name "), home, false);

    out << "=== live symlink attack on the mount point ===" << Qt::endl;
    {
        QTemporaryDir tmp(home + QStringLiteral("/.nasmount-test-XXXXXX"));
        check(QStringLiteral("temp dir created"), tmp.isValid());

        const QString victim = tmp.filePath(QStringLiteral("victim"));
        QDir().mkpath(victim);
        const QString link = tmp.filePath(QStringLiteral("evil"));
        check(QStringLiteral("symlink planted"), QFile::link(victim, link));

        QString err;
        int fd = openPath(link, home, uid, gid, &err);
        check(QStringLiteral("symlinked mount point refused"), fd < 0, err);
        if (fd >= 0) {
            ::close(fd);
        }

        const QString full = tmp.filePath(QStringLiteral("full"));
        QDir().mkpath(full);
        QFile marker(full + QStringLiteral("/data"));
        check(QStringLiteral("non-empty dir seeded"), marker.open(QIODevice::WriteOnly));
        marker.write("x");
        marker.close();
        err.clear();
        fd = openPath(full, home, uid, gid, &err);
        check(QStringLiteral("non-empty mount point refused"), fd < 0, err);
        if (fd >= 0) {
            ::close(fd);
        }

        // A pre-existing directory belonging to someone else must not be taken
        // over: root would otherwise hand one user another user's empty directory.
        const QString foreign = tmp.filePath(QStringLiteral("foreign"));
        QDir().mkpath(foreign);
        UnitSpec::MountpointPlan fplan;
        err.clear();
        if (UnitSpec::validateMountpoint(foreign, home, &fplan, &err)) {
            const uid_t otherUid = (uid == 0) ? 1 : uid + 1;
            err.clear();
            fd = UnitSpec::openMountpointNoFollow(fplan, otherUid, gid, &err);
            check(QStringLiteral("pre-existing dir owned by another uid refused"), fd < 0, err);
            if (fd >= 0) {
                ::close(fd);
            }
        }

        const QString good = tmp.filePath(QStringLiteral("a/b/MOUNT"));
        err.clear();
        fd = openPath(good, home, uid, gid, &err);
        check(QStringLiteral("nested path created"), fd >= 0, err);
        if (fd >= 0) {
            struct stat st {};
            ::fstat(fd, &st);
            check(QStringLiteral("final dir is 0700"), (st.st_mode & 0777) == 0700,
                  QStringLiteral("%1").arg(st.st_mode & 0777, 0, 8));
            check(QStringLiteral("owned by invoking user"), st.st_uid == uid);
            ::close(fd);
        }
    }

    out << "=== openMountpointNoCreate: boot's no-create/no-chown walk ===" << Qt::endl;
    {
        QTemporaryDir tmp(home + QStringLiteral("/.nasmount-test-boot-XXXXXX"));
        check(QStringLiteral("temp dir created"), tmp.isValid());

        const QString missing = tmp.filePath(QStringLiteral("never-created/MOUNT"));
        QString err;
        int fd = openPathNoCreate(missing, home, uid, gid, &err);
        check(QStringLiteral("missing component refused, not created"), fd < 0, err);
        check(QStringLiteral("distinguishable 'does not exist' message"), err.contains(QStringLiteral("does not exist")),
              err);
        check(QStringLiteral("nothing was created on disk"), !QDir(tmp.filePath(QStringLiteral("never-created"))).exists());
        if (fd >= 0) {
            ::close(fd);
        }

        const QString victim = tmp.filePath(QStringLiteral("victim"));
        QDir().mkpath(victim);
        const QString link = tmp.filePath(QStringLiteral("evil"));
        check(QStringLiteral("symlink planted"), QFile::link(victim, link));
        err.clear();
        fd = openPathNoCreate(link, home, uid, gid, &err);
        check(QStringLiteral("symlinked mount point refused"), fd < 0, err);
        if (fd >= 0) {
            ::close(fd);
        }

        const QString full = tmp.filePath(QStringLiteral("full"));
        QDir().mkpath(full);
        QFile marker(full + QStringLiteral("/data"));
        check(QStringLiteral("non-empty dir seeded"), marker.open(QIODevice::WriteOnly));
        marker.write("x");
        marker.close();
        err.clear();
        fd = openPathNoCreate(full, home, uid, gid, &err);
        check(QStringLiteral("non-empty mount point refused"), fd < 0, err);
        if (fd >= 0) {
            ::close(fd);
        }

        const QString empty = tmp.filePath(QStringLiteral("empty"));
        QDir().mkpath(empty);
        struct stat beforeSt {};
        ::stat(empty.toLocal8Bit().constData(), &beforeSt);
        err.clear();
        fd = openPathNoCreate(empty, home, uid + 1, gid, &err);
        check(QStringLiteral("wrong recorded uid refused"), fd < 0, err);
        if (fd >= 0) {
            ::close(fd);
        }
        err.clear();
        fd = openPathNoCreate(empty, home, uid, gid + 1, &err);
        check(QStringLiteral("wrong recorded gid refused (stricter than the interactive walk)"), fd < 0, err);
        if (fd >= 0) {
            ::close(fd);
        }

        // The one accept path this project can test without root: an
        // already-existing, already-correctly-owned directory needs no
        // privilege to verify, only to have created in the first place
        // (done above by the test itself, standing in for an earlier
        // interactive Add).
        err.clear();
        fd = openPathNoCreate(empty, home, uid, gid, &err);
        check(QStringLiteral("pre-existing, correctly-owned, empty dir accepted"), fd >= 0, err);
        if (fd >= 0) {
            struct stat afterSt {};
            ::fstat(fd, &afterSt);
            check(QStringLiteral("mode left exactly as mkpath() created it (no chmod)"),
                  (afterSt.st_mode & 0777) == (beforeSt.st_mode & 0777),
                  QStringLiteral("before=%1 after=%2")
                      .arg(beforeSt.st_mode & 0777, 0, 8)
                      .arg(afterSt.st_mode & 0777, 0, 8));
            ::close(fd);
        }
    }

    out << "=== unit generation + restricted-template validation: all four combinations ===" << Qt::endl;
    {
        const QString mountPoint = QStringLiteral("/mnt/nas");
        const QString unc = QStringLiteral("//192.0.2.10/DATA");
        const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");

        const struct {
            UnitValue::AuthenticationKind auth;
            const char *label;
        } combos[] = {
            {UnitValue::AuthenticationKind::Credentials, "credentials"},
            {UnitValue::AuthenticationKind::Guest, "guest"},
        };
        for (const auto &combo : combos) {
            UnitValue::Marker marker;
            marker.ownerUid = uid;
            marker.ownerGid = gid;
            marker.id = id;
            marker.authentication = combo.auth;

            QString mountContent, automountContent, err;
            check(QStringLiteral("%1: mount unit generated").arg(combo.label),
                  UnitSpec::buildMountUnitContent(marker, unc, mountPoint, &mountContent, &err), err);
            check(QStringLiteral("%1: automount unit generated").arg(combo.label),
                  UnitSpec::buildAutomountUnitContent(marker, mountPoint, &automountContent, &err), err);

            const bool expectCredential = (combo.auth == UnitValue::AuthenticationKind::Credentials);
            const QString expectedCredPath = UnitSpec::credentialPathFor(id);
            check(QStringLiteral("%1: credential path present iff authenticated").arg(combo.label),
                  mountContent.contains(QStringLiteral("credentials=%1").arg(expectedCredPath)) == expectCredential,
                  mountContent);
            check(QStringLiteral("%1: guest unit never contains a credential path").arg(combo.label),
                  expectCredential || !mountContent.contains(QStringLiteral("credentials=")));

            QString extractedWhat;
            check(QStringLiteral("%1: mount body validates").arg(combo.label),
                  UnitSpec::validateMountUnitBody(mountContent, marker, mountPoint, &extractedWhat, &err), err);
            check(QStringLiteral("%1: extracted What= matches the original UNC").arg(combo.label),
                  extractedWhat == unc, extractedWhat);
            check(QStringLiteral("%1: automount body validates").arg(combo.label),
                  UnitSpec::validateAutomountUnitBody(automountContent, marker, mountPoint, &err), err);
        }
    }

    out << "=== restricted-template validation: rejection matrix ===" << Qt::endl;
    {
        const QString mountPoint = QStringLiteral("/mnt/nas");
        const QString unc = QStringLiteral("//192.0.2.10/DATA");
        UnitValue::Marker marker;
        marker.ownerUid = uid;
        marker.ownerGid = gid;
        marker.id = QStringLiteral("0123456789abcdef0123456789abcdef");
        marker.authentication = UnitValue::AuthenticationKind::Credentials;

        QString baseMount, baseAutomount, err;
        check(QStringLiteral("baseline mount unit generated"),
              UnitSpec::buildMountUnitContent(marker, unc, mountPoint, &baseMount, &err), err);
        check(QStringLiteral("baseline automount unit generated"),
              UnitSpec::buildAutomountUnitContent(marker, mountPoint, &baseAutomount, &err), err);

        auto expectMountRejected = [&](const QString &label, const QString &content) {
            QString what, error;
            const bool okResult = UnitSpec::validateMountUnitBody(content, marker, mountPoint, &what, &error);
            check(label, !okResult, okResult ? QStringLiteral("unexpectedly accepted") : error);
        };
        auto expectAutomountRejected = [&](const QString &label, const QString &content) {
            QString error;
            const bool okResult = UnitSpec::validateAutomountUnitBody(content, marker, mountPoint, &error);
            check(label, !okResult, okResult ? QStringLiteral("unexpectedly accepted") : error);
        };

        check(QStringLiteral("baseline: valid mount body actually validates"), [&] {
            QString what, error;
            return UnitSpec::validateMountUnitBody(baseMount, marker, mountPoint, &what, &error);
        }());

        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral("Type=cifs"), QStringLiteral("Where=/mnt/nas\nType=cifs"));
            expectMountRejected(QStringLiteral("duplicate functional assignment rejected"), tampered);
        }
        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral("Type=cifs"), QStringLiteral("Type=cifs\\"));
            expectMountRejected(QStringLiteral("line continuation rejected"), tampered);
        }
        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral("Type=cifs"), QStringLiteral("Type=cifs\nFreeform=1"));
            expectMountRejected(QStringLiteral("unknown functional directive in [Mount] rejected"), tampered);
        }
        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral("Type=cifs"), QStringLiteral("Type=nfs"));
            expectMountRejected(QStringLiteral("non-CIFS mount rejected"), tampered);
        }
        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral("Where=/mnt/nas"), QStringLiteral("Where=/mnt/other"));
            expectMountRejected(QStringLiteral("mismatched Where= rejected"), tampered);
        }
        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral("What=//192.0.2.10/DATA"), QStringLiteral("What=not-a-unc"));
            expectMountRejected(QStringLiteral("invalid What= rejected"), tampered);
        }
        {
            QString tampered = baseMount;
            tampered.replace(QStringLiteral(",nosuid,"), QStringLiteral(",EXTRA,nosuid,"));
            expectMountRejected(QStringLiteral("unexpected fixed option rejected"), tampered);
        }
        {
            // A credential path from a different (but validly-shaped) id must
            // be rejected: Options= no longer matches this marker's id.
            QString tampered = baseMount;
            const QString wrongCredPath =
                UnitSpec::credentialPathFor(QStringLiteral("ffffffffffffffffffffffffffffffff"));
            tampered.replace(UnitSpec::credentialPathFor(marker.id), wrongCredPath);
            expectMountRejected(QStringLiteral("credential path inconsistent with marker id rejected"), tampered);
        }
        {
            // Take a genuine guest unit and splice a credential path into its
            // Options= — must be rejected even though the path itself is
            // well-formed for this marker's id.
            UnitValue::Marker guestMarker = marker;
            guestMarker.authentication = UnitValue::AuthenticationKind::Guest;
            QString guestContent;
            check(QStringLiteral("guest baseline generated"),
                  UnitSpec::buildMountUnitContent(guestMarker, unc, mountPoint, &guestContent, &err), err);
            guestContent.replace(QStringLiteral("Options=guest,"),
                                 QStringLiteral("Options=credentials=%1,")
                                     .arg(UnitSpec::credentialPathFor(guestMarker.id)));
            QString what, error;
            const bool okResult =
                UnitSpec::validateMountUnitBody(guestContent, guestMarker, mountPoint, &what, &error);
            check(QStringLiteral("guest unit carrying a credential path rejected"), !okResult, error);
        }

        check(QStringLiteral("baseline: valid automount body actually validates"), [&] {
            QString error;
            return UnitSpec::validateAutomountUnitBody(baseAutomount, marker, mountPoint, &error);
        }());
        {
            QString tampered = baseAutomount;
            tampered.replace(QStringLiteral("TimeoutIdleSec=60"),
                             QStringLiteral("TimeoutIdleSec=60\nTimeoutIdleSec=60"));
            expectAutomountRejected(QStringLiteral("duplicate [Automount] assignment rejected"), tampered);
        }
        {
            QString tampered = baseAutomount;
            tampered.replace(QStringLiteral("TimeoutIdleSec=60"), QStringLiteral("TimeoutIdleSec=5"));
            expectAutomountRejected(QStringLiteral("unexpected TimeoutIdleSec= rejected"), tampered);
        }
        {
            QString tampered = baseAutomount;
            tampered.replace(QStringLiteral("DirectoryMode=0700"), QStringLiteral("DirectoryMode=0777"));
            expectAutomountRejected(QStringLiteral("unexpected DirectoryMode= rejected"), tampered);
        }
        {
            QString tampered = baseAutomount;
            tampered.replace(QStringLiteral("ConditionPathIsDirectory=/mnt/nas"),
                             QStringLiteral("ConditionPathIsDirectory=/mnt/other"));
            expectAutomountRejected(QStringLiteral("mismatched ConditionPathIsDirectory= rejected"), tampered);
        }
        {
            QString tampered = baseAutomount;
            tampered.replace(QStringLiteral("ConditionPathIsDirectory=/mnt/nas\n"), QString());
            expectAutomountRejected(QStringLiteral("missing ConditionPathIsDirectory= rejected"), tampered);
        }
        {
            QString tampered = baseAutomount;
            tampered.replace(QStringLiteral("Where=/mnt/nas"), QStringLiteral("Where=/mnt/nas\\"));
            expectAutomountRejected(QStringLiteral("line continuation in automount rejected"), tampered);
        }
        // systemd parses Condition*= only out of [Unit]; one emitted under
        // [Automount] is ignored with a log line, silently voiding the guard
        // that stops a trigger being set up over a missing mount point. A
        // round-trip generate/validate pair cannot catch that on its own --
        // both sides agreed on the wrong section once before -- so pin the
        // section placement itself.
        {
            const int conditionAt = baseAutomount.indexOf(QStringLiteral("ConditionPathIsDirectory="));
            const int automountAt = baseAutomount.indexOf(QStringLiteral("[Automount]"));
            const int unitAt = baseAutomount.indexOf(QStringLiteral("[Unit]"));
            check(QStringLiteral("ConditionPathIsDirectory= is emitted inside [Unit], not [Automount]"),
                  conditionAt > unitAt && unitAt >= 0 && automountAt > conditionAt, baseAutomount);
        }
        {
            // Condition under [Automount], absent from [Unit]: systemd would
            // ignore it there, so this layout must fail closed rather than
            // validate.
            QString oldLayout = baseAutomount;
            oldLayout.replace(QStringLiteral("ConditionPathIsDirectory=/mnt/nas\n\n"), QStringLiteral("\n"));
            oldLayout.replace(QStringLiteral("DirectoryMode=0700"),
                              QStringLiteral("DirectoryMode=0700\nConditionPathIsDirectory=/mnt/nas"));
            expectAutomountRejected(QStringLiteral("[Automount]-section condition rejected"), oldLayout);
        }
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
