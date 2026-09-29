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
#include "mountpointcheck.h"
#include "unitvalue.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <pwd.h>
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

    out << "=== localPathFromUrl ===" << Qt::endl;
    {
        // The folder picker hands back a URL. Its string form keeps '%' and
        // '#' percent-encoded, so every name below must come back exactly as
        // it was spelled, not as the URL spells it.
        const QStringList paths = {
            QStringLiteral("/home/u/plain"),
            QStringLiteral("/home/u/My Share"),
            QStringLiteral("/home/u/100%"),
            QStringLiteral("/home/u/100%25"),
            QStringLiteral("/home/u/a#b"),
            QStringLiteral("/home/u/why?"),
        };
        for (const QString &path : paths) {
            check(QStringLiteral("round trip: %1").arg(path),
                  Session::localPathFromUrl(QUrl::fromLocalFile(path)) == path,
                  Session::localPathFromUrl(QUrl::fromLocalFile(path)));
        }

        // The regression itself: cutting "file://" off the string form.
        const QUrl percent = QUrl::fromLocalFile(QStringLiteral("/home/u/100%"));
        check(QStringLiteral("the string form is not the path (why toString() is not used)"),
              QString(percent.toString()).replace(QStringLiteral("file://"), QString())
                  != QStringLiteral("/home/u/100%"));

        check(QStringLiteral("an smb:// pick is not a local folder"),
              Session::localPathFromUrl(QUrl(QStringLiteral("smb://nas/Media"))).isEmpty());
        check(QStringLiteral("a file URL naming another host is not a local folder"),
              Session::localPathFromUrl(QUrl(QStringLiteral("file://otherhost/share/dir"))).isEmpty());
        check(QStringLiteral("an empty URL is not a local folder"),
              Session::localPathFromUrl(QUrl()).isEmpty());

        // Through the invokable QML calls, which must agree with the function.
        Session::MountActions actions;
        check(QStringLiteral("the invokable agrees with the function"),
              actions.localPathFromUrl(QUrl::fromLocalFile(QStringLiteral("/home/u/100%")))
                  == QStringLiteral("/home/u/100%"));
    }

    const struct passwd *pw = ::getpwuid(::getuid());
    const QString home = QString::fromLocal8Bit(pw->pw_dir);
    const uid_t uid = ::getuid();

    out << "=== canonicalMountPoint: the one path both the check and the submit mean ===" << Qt::endl;
    {
        auto expectCanonical = [&](const QString &label, const QString &raw, const QString &expected) {
            const QString got = Session::canonicalMountPoint(raw, home);
            check(label, got == expected, got);
        };
        expectCanonical(QStringLiteral("a trailing slash goes"), QStringLiteral("/mnt/nas/"), QStringLiteral("/mnt/nas"));
        expectCanonical(QStringLiteral("doubled and dotted segments go"), QStringLiteral("/mnt//nas/./x"),
                        QStringLiteral("/mnt/nas/x"));
        expectCanonical(QStringLiteral("surrounding whitespace goes"), QStringLiteral("  /mnt/nas \n"),
                        QStringLiteral("/mnt/nas"));
        expectCanonical(QStringLiteral("~/ is the caller's home"), QStringLiteral("~/NAS"), home + QStringLiteral("/NAS"));
        expectCanonical(QStringLiteral("~// and a trailing slash still normalise"), QStringLiteral("~//NAS/"),
                        home + QStringLiteral("/NAS"));
        expectCanonical(QStringLiteral("~ with whitespace around it"), QStringLiteral("  ~/NAS  "),
                        home + QStringLiteral("/NAS"));
        expectCanonical(QStringLiteral("~ alone is the home folder itself"), QStringLiteral("~"), home);
        expectCanonical(QStringLiteral("~/.. is resolved after expansion, not before"), QStringLiteral("~/a/../b"),
                        home + QStringLiteral("/b"));
        expectCanonical(QStringLiteral("~name is another account's home and stays as typed"),
                        QStringLiteral("~bob/NAS"), QStringLiteral("~bob/NAS"));
        expectCanonical(QStringLiteral("a ~ that is not the first character is an ordinary name"),
                        QStringLiteral("/data/~/x"), QStringLiteral("/data/~/x"));
        expectCanonical(QStringLiteral("nothing stays nothing"), QString(), QString());
        check(QStringLiteral("no known home means ~ is left alone rather than guessed"),
              Session::canonicalMountPoint(QStringLiteral("~/NAS"), QString()) == QStringLiteral("~/NAS"));
    }

    out << "=== mountPointProblem: lexical, the helper's own rule ===" << Qt::endl;
    {
        auto expectProblem = [&](const QString &label, const QString &raw, const QString &mention) {
            const QString problem = Session::mountPointProblem(raw, home);
            check(label, problem.contains(mention) && problem.at(0).isUpper(), problem);
        };
        auto expectFine = [&](const QString &label, const QString &raw) {
            const QString problem = Session::mountPointProblem(raw, home);
            check(label, problem.isEmpty(), problem);
        };
        expectProblem(QStringLiteral("nothing typed"), QString(), QStringLiteral("Choose a folder"));
        expectProblem(QStringLiteral("only whitespace typed"), QStringLiteral("   "), QStringLiteral("Choose a folder"));
        expectProblem(QStringLiteral("a relative path"), QStringLiteral("NAS/data"), QStringLiteral("absolute path"));
        expectProblem(QStringLiteral("outside every allowed root"), QStringLiteral("/tmp/nas"),
                      QStringLiteral("must be below one of"));
        expectProblem(QStringLiteral("an allowed root itself"), QStringLiteral("/mnt"), QStringLiteral("directly onto"));
        expectProblem(QStringLiteral("the home folder itself"), home, QStringLiteral("directly onto"));
        expectProblem(QStringLiteral("~ alone is the home folder itself"), QStringLiteral("~"),
                      QStringLiteral("directly onto"));
        expectProblem(QStringLiteral("~name is not expanded, and says so"), QStringLiteral("~bob/NAS"),
                      QStringLiteral("Only ~ for your own home"));
        expectProblem(QStringLiteral("a path that climbs out of home"), home + QStringLiteral("/a/../../etc"),
                      QStringLiteral("must be below one of"));
        expectProblem(QStringLiteral("a control character"), home + QStringLiteral("/a\nb"),
                      QStringLiteral("control characters"));
        expectProblem(QStringLiteral("a trailing backslash"), home + QStringLiteral("/a\\"),
                      QStringLiteral("backslash"));
        expectFine(QStringLiteral("a folder in home"), home + QStringLiteral("/NAS"));
        expectFine(QStringLiteral("a folder in home, typed as ~/"), QStringLiteral("~/NAS"));
        expectFine(QStringLiteral("a folder under /mnt"), QStringLiteral("/mnt/nas"));
        expectFine(QStringLiteral("a folder under /media"), QStringLiteral("/media/nas/data"));
        // The helper's whitespace rule can never fire: addShare() trims first,
        // so the form must not claim a refusal the submit would not make.
        expectFine(QStringLiteral("surrounding spaces are trimmed, not refused"),
                   QStringLiteral("  ") + home + QStringLiteral("/NAS  "));
        // The property that matters: whatever this accepts, the helper's
        // authorization accepts for the path addShare() then sends.
        const QStringList samples = {QStringLiteral("~/NAS"), QStringLiteral("~/NAS/"), QStringLiteral("/mnt/a//b"),
                                     QStringLiteral(" /media/x "), QStringLiteral("~bob/x"), QStringLiteral("/etc/x"),
                                     home + QStringLiteral("/a/./b")};
        for (const QString &raw : samples) {
            UnitSpec::MountpointPlan plan;
            QString error;
            const bool helperAccepts = UnitSpec::validateMountpoint(Session::canonicalMountPoint(raw, home), home,
                                                                    &plan, &error);
            check(QStringLiteral("agrees with the helper's authorization: '%1'").arg(raw),
                  Session::mountPointProblem(raw, home).isEmpty() == helperAccepts,
                  Session::mountPointProblem(raw, home));
        }
    }

    out << "=== unitCollisionProblem / mountPointFsProblem ===" << Qt::endl;
    {
        // The unit names come from systemd-escape; a machine without it is
        // "cannot tell", not a failure of this test.
        UnitValue::UnitPaths probe;
        QString probeError;
        const bool haveEscape = UnitValue::unitPathsFor(home + QStringLiteral("/probe"), &probe, &probeError);
        if (!haveEscape) {
            out << "  SKIP  no systemd-escape here, so the unit collision checks are not exercised   " << probeError
                << Qt::endl;
        }
        if (haveEscape) {
            UnitSpec::MountpointPlan plan;
            plan.root = home;
            plan.suffix = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
            plan.path = home + QStringLiteral("/a/b/c");

            auto unitPath = [](const QString &path, bool automount) {
                UnitValue::UnitPaths paths;
                QString error;
                UnitValue::unitPathsFor(path, &paths, &error);
                return automount ? paths.automountUnitPath : paths.mountUnitPath;
            };
            auto existsOnly = [](const QString &wanted) {
                return Session::UnitFileExists([wanted](const QString &path) { return path == wanted; });
            };

            check(QStringLiteral("no units: no collision"),
                  Session::unitCollisionProblem(plan, existsOnly(QString())).isEmpty());
            check(QStringLiteral("a .mount for the same path collides"),
                  Session::unitCollisionProblem(plan, existsOnly(unitPath(plan.path, false)))
                      == QStringLiteral("Another mount already uses this folder"));
            check(QStringLiteral("an .automount for the same path collides"),
                  Session::unitCollisionProblem(plan, existsOnly(unitPath(plan.path, true)))
                      == QStringLiteral("Another mount already uses this folder"));
            const QString parent = Session::unitCollisionProblem(plan, existsOnly(unitPath(home + QStringLiteral("/a/b"), true)));
            check(QStringLiteral("a mount on the parent folder means inside another mount"),
                  parent.contains(QStringLiteral("inside another mount")) && parent.contains(home + QStringLiteral("/a/b")),
                  parent);
            check(QStringLiteral("a mount further up the path too"),
                  Session::unitCollisionProblem(plan, existsOnly(unitPath(home + QStringLiteral("/a"), false)))
                      .contains(QStringLiteral("inside another mount")));
            check(QStringLiteral("the allowed root itself is not examined"),
                  Session::unitCollisionProblem(plan, existsOnly(unitPath(home, false))).isEmpty());
            check(QStringLiteral("a mount below the path is the folder walk's business, not this one's"),
                  Session::unitCollisionProblem(plan, existsOnly(unitPath(home + QStringLiteral("/a/b/c/d"), false))).isEmpty());
            check(QStringLiteral("a sibling's mount is not a collision"),
                  Session::unitCollisionProblem(plan, existsOnly(unitPath(home + QStringLiteral("/a/b/other"), false))).isEmpty());

            const auto always = Session::UnitFileExists([](const QString &) { return true; });
            check(QStringLiteral("text that is not a mount point at all is left to the lexical check"),
                  Session::mountPointFsProblem(QStringLiteral("/tmp/x"), home, uid, always).isEmpty()
                      && Session::mountPointFsProblem(QString(), home, uid, always).isEmpty());
            check(QStringLiteral("a valid path with a unit reports the collision first"),
                  Session::mountPointFsProblem(QStringLiteral("~/a/b/c"), home, uid, always)
                      == QStringLiteral("Another mount already uses this folder"));
        }

        // The folder itself, end to end, with ~ expanded on the way.
        QTemporaryDir tmp(home + QStringLiteral("/.nasmount-test-check-XXXXXX"));
        check(QStringLiteral("temp dir created"), tmp.isValid());
        const QString leaf = QFileInfo(tmp.path()).fileName();
        const auto none = Session::UnitFileExists([](const QString &) { return false; });
        QFile file(tmp.filePath(QStringLiteral("data")));
        check(QStringLiteral("fixture file written"), file.open(QIODevice::WriteOnly) && file.write("x") == 1);
        file.close();
        QDir().mkpath(tmp.filePath(QStringLiteral("empty")));

        check(QStringLiteral("a non-empty folder is reported"),
              Session::mountPointFsProblem(tmp.path(), home, uid, none).contains(QStringLiteral("not empty")));
        check(QStringLiteral("... also when typed with ~"),
              Session::mountPointFsProblem(QStringLiteral("~/") + leaf, home, uid, none).contains(QStringLiteral("not empty")));
        check(QStringLiteral("an empty folder is fine"),
              Session::mountPointFsProblem(tmp.filePath(QStringLiteral("empty")), home, uid, none).isEmpty());
        check(QStringLiteral("a folder that does not exist yet is fine"),
              Session::mountPointFsProblem(tmp.filePath(QStringLiteral("new/deeper")), home, uid, none).isEmpty());
        check(QStringLiteral("a folder owned by someone else is reported"),
              Session::mountPointFsProblem(tmp.filePath(QStringLiteral("empty")), home, uid + 1, none)
                  .contains(QStringLiteral("another user")));
    }

    out << "=== lookupTargetOf: which stored login an address would get ===" << Qt::endl;
    {
        Session::MountActions actions;
        auto expectTarget = [&](const QString &label, const QString &input, const QString &expected) {
            const QString got = actions.lookupTargetOf(input);
            check(label, got == expected, got);
        };
        expectTarget(QStringLiteral("a share"), QStringLiteral("smb://nas.local/DATA"),
                     QStringLiteral("smb://nas.local/DATA"));
        expectTarget(QStringLiteral("a folder inside it is the same target"), QStringLiteral("smb://nas.local/DATA/Films"),
                     QStringLiteral("smb://nas.local/DATA"));
        expectTarget(QStringLiteral("the // spelling is the same target"), QStringLiteral("//nas.local/DATA"),
                     QStringLiteral("smb://nas.local/DATA"));
        expectTarget(QStringLiteral("the host's letter case does not matter"), QStringLiteral("smb://NAS.local/DATA"),
                     QStringLiteral("smb://nas.local/DATA"));
        expectTarget(QStringLiteral("a space in the share is encoded once"), QStringLiteral("smb://nas.local/Media Library"),
                     QStringLiteral("smb://nas.local/Media%20Library"));
        check(QStringLiteral("another share is another target"),
              actions.lookupTargetOf(QStringLiteral("smb://nas.local/DATA"))
                  != actions.lookupTargetOf(QStringLiteral("smb://nas.local/OTHER")));
        check(QStringLiteral("another server is another target"),
              actions.lookupTargetOf(QStringLiteral("smb://nas.local/DATA"))
                  != actions.lookupTargetOf(QStringLiteral("smb://other.local/DATA")));
        expectTarget(QStringLiteral("a server alone has no target"), QStringLiteral("smb://nas.local"), QString());
        expectTarget(QStringLiteral("the bare prefix has no target"), QStringLiteral("smb://"), QString());
        expectTarget(QStringLiteral("text that is not an address has no target"), QStringLiteral("nas/DATA"), QString());
        expectTarget(QStringLiteral("an address naming a user is not usable here"),
                     QStringLiteral("smb://alice@nas.local/DATA"), QString());
    }

    out << "=== checkMountPoint: one at a time, newest waiting request wins ===" << Qt::endl;
    {
        QTemporaryDir tmp(home + QStringLiteral("/.nasmount-test-async-XXXXXX"));
        check(QStringLiteral("temp dir created"), tmp.isValid());
        QFile file(tmp.filePath(QStringLiteral("data")));
        check(QStringLiteral("fixture file written"), file.open(QIODevice::WriteOnly) && file.write("x") == 1);
        file.close();
        QDir().mkpath(tmp.filePath(QStringLiteral("one")));
        QDir().mkpath(tmp.filePath(QStringLiteral("two")));
        QDir().mkpath(tmp.filePath(QStringLiteral("three")));

        Session::MountActions actions;
        QStringList answered;
        QStringList problems;
        QObject::connect(&actions, &Session::MountActions::mountPointChecked, &actions,
                         [&](const QString &raw, const QString &problem) {
                             answered << raw;
                             problems << problem;
                         });
        auto waitFor = [&](int count) {
            QElapsedTimer timer;
            timer.start();
            while (answered.size() < count && timer.elapsed() < 15000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(2);
            }
        };

        // The requests all arrive before the event loop can deliver the first
        // answer, so what happens to the middle one is deterministic.
        const QString first = tmp.filePath(QStringLiteral("one"));
        const QString middle = tmp.filePath(QStringLiteral("two"));
        const QString last = tmp.filePath(QStringLiteral("three"));
        actions.checkMountPoint(first);
        actions.checkMountPoint(middle);
        actions.checkMountPoint(last);
        waitFor(2);
        // Give a stray third answer, which would be the bug, time to show up.
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 300) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(QStringLiteral("the first and the last are answered, the replaced one is not"),
              answered == QStringList({first, last}), answered.join(QStringLiteral(", ")));
        check(QStringLiteral("each answer carries the text it is for and is empty for a fine folder"),
              problems == QStringList({QString(), QString()}), problems.join(QStringLiteral(" | ")));

        answered.clear();
        problems.clear();
        actions.checkMountPoint(tmp.path());
        waitFor(1);
        check(QStringLiteral("a later request is answered with what it found"),
              answered == QStringList({tmp.path()}) && problems.value(0).contains(QStringLiteral("not empty")),
              problems.join(QStringLiteral(" | ")));
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
