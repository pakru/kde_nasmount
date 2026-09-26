/*
 * Tests for Root::DurableFs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every real call site in durablefs.cpp hardcodes uid 0 (root) as the
 * expected owner, so an unprivileged test can never observe the full
 * accept-path of openVerifiedDir()/createAndVerifyDir()/durableReplace() —
 * exactly like verify_test.cpp's inspectDefinition coverage, that needs
 * root and belongs to integration testing. What is covered here:
 * verifyDescriptor() directly (it takes the expected uid as a parameter,
 * so a real accept-path is testable with the caller's own uid), and every
 * fail-closed rejection path of the higher-level functions reachable
 * without root — symlinks, wrong type, invalid names, and idempotent
 * absence.
 */

#include "durablefs.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <fcntl.h>
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

namespace
{
int openDirFd(const QString &path)
{
    return ::open(path.toLocal8Bit().constData(), O_DIRECTORY | O_CLOEXEC);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const uid_t me = ::getuid();

    out << "=== verifyDescriptor: real accept-path, parameterised by caller uid ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        check(QStringLiteral("temp dir created"), tmp.isValid());
        const int dirFd = openDirFd(tmp.path());
        check(QStringLiteral("temp dir opened"), dirFd >= 0);

        QString error;
        check(QStringLiteral("a directory owned by me, default QTemporaryDir mode 0700, matches"),
              Root::DurableFs::verifyDescriptor(dirFd, Root::DurableFs::FileKind::Directory, me, 0700, &error),
              error);

        const QString filePath = tmp.filePath(QStringLiteral("regular"));
        QFile f(filePath);
        check(QStringLiteral("regular file created"), f.open(QIODevice::WriteOnly));
        f.close();
        check(QStringLiteral("mode set to 0600"), ::chmod(filePath.toLocal8Bit().constData(), 0600) == 0);
        const int fileFd = ::open(filePath.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
        check(QStringLiteral("regular file opened"), fileFd >= 0);
        check(QStringLiteral("a regular file owned by me, mode 0600, matches"),
              Root::DurableFs::verifyDescriptor(fileFd, Root::DurableFs::FileKind::Regular, me, 0600, &error),
              error);

        if (fileFd >= 0) {
            ::close(fileFd);
        }
        if (dirFd >= 0) {
            ::close(dirFd);
        }
    }

    out << "=== verifyDescriptor: rejections ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        const int dirFd = openDirFd(tmp.path());
        QString error;

        check(QStringLiteral("directory rejected as a regular file"),
              !Root::DurableFs::verifyDescriptor(dirFd, Root::DurableFs::FileKind::Regular, me, 0700, &error),
              error);
        check(QStringLiteral("wrong expected uid rejected"),
              !Root::DurableFs::verifyDescriptor(dirFd, Root::DurableFs::FileKind::Directory, me + 1, 0700, &error),
              error);
        check(QStringLiteral("wrong expected mode rejected"),
              !Root::DurableFs::verifyDescriptor(dirFd, Root::DurableFs::FileKind::Directory, me, 0755, &error),
              error);
        if (dirFd >= 0) {
            ::close(dirFd);
        }
    }

    out << "=== openSystemRoot ===" << Qt::endl;
    {
        QString error;
        // /etc is root-owned and not group/other-writable on every sane
        // system -- a real accept-path reachable without root.
        const int etcFd = Root::DurableFs::openSystemRoot(QStringLiteral("/etc"), &error);
        check(QStringLiteral("/etc accepted as a safe system root"), etcFd >= 0, error);
        if (etcFd >= 0) {
            ::close(etcFd);
        }

        // /tmp is root-owned but world-writable (mode 1777) -- must be
        // refused even though the owner check alone would pass.
        const int tmpFd = Root::DurableFs::openSystemRoot(QStringLiteral("/tmp"), &error);
        check(QStringLiteral("/tmp rejected: world-writable"), tmpFd < 0, error);
        if (tmpFd >= 0) {
            ::close(tmpFd);
        }

        const int missingFd = Root::DurableFs::openSystemRoot(QStringLiteral("/no-such-path-nasmount-test"), &error);
        check(QStringLiteral("nonexistent path rejected"), missingFd < 0);
    }

    out << "=== openVerifiedDir / createAndVerifyDir: fail-closed without root ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        const int parentFd = openDirFd(tmp.path());
        check(QStringLiteral("parent opened"), parentFd >= 0);

        QString error;
        const int missing = Root::DurableFs::openVerifiedDir(parentFd, QStringLiteral("nope"),
                                                              Root::DurableFs::ArtifactKind::Directory, &error);
        check(QStringLiteral("missing name -> -1, empty error (ENOENT, ordinary absence)"), missing < 0 && error.isEmpty(),
              error);

        // createAndVerifyDir creates the directory (as the test's own uid,
        // since there is no root here), then must still refuse it: the
        // fixed policy demands uid 0, and this test process is not root.
        const int created = Root::DurableFs::createAndVerifyDir(parentFd, QStringLiteral("subdir"),
                                                                 Root::DurableFs::ArtifactKind::Directory, &error);
        check(QStringLiteral("created-but-not-root-owned directory is refused, not silently accepted"),
              created < 0, error);
        check(QStringLiteral("the rejection reason names the uid mismatch"), error.contains(QStringLiteral("uid")),
              error);

        QDir(tmp.path()).mkdir(QStringLiteral("plain"));
        QString error2;
        const int plainDir = Root::DurableFs::openVerifiedDir(parentFd, QStringLiteral("plain"),
                                                               Root::DurableFs::ArtifactKind::Directory, &error2);
        check(QStringLiteral("an existing but non-root-owned directory is refused"), plainDir < 0, error2);
        check(QStringLiteral("(this refusal has a non-empty reason, unlike plain ENOENT)"), !error2.isEmpty());

        check(QStringLiteral("a symlink planted at the name is refused, never followed"), [&] {
            QFile::link(tmp.path(), tmp.filePath(QStringLiteral("linked")));
            QString linkError;
            const int r = Root::DurableFs::openVerifiedDir(parentFd, QStringLiteral("linked"),
                                                            Root::DurableFs::ArtifactKind::Directory, &linkError);
            return r < 0 && !linkError.isEmpty();
        }());

        check(QStringLiteral("a name containing '/' is rejected outright"), [&] {
            QString pathError;
            return Root::DurableFs::openVerifiedDir(parentFd, QStringLiteral("a/b"),
                                                     Root::DurableFs::ArtifactKind::Directory, &pathError) < 0;
        }());

        if (parentFd >= 0) {
            ::close(parentFd);
        }
    }

    out << "=== durableReplace: leaves no stray temp file when it cannot succeed ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        const int dirFd = openDirFd(tmp.path());
        QString error;

        // fchown(fd, 0, 0) is certain to fail for a non-root process
        // (EPERM), so this exercises the abandon-and-cleanup path, not the
        // full success path (which needs root).
        const bool result = Root::DurableFs::durableReplace(dirFd, QStringLiteral("target"), QByteArrayLiteral("hi"),
                                                            Root::DurableFs::ArtifactKind::SensitiveFile, &error);
        check(QStringLiteral("durableReplace fails (cannot chown to root as this uid)"), !result, error);

        const QStringList entries = QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        bool leaked = false;
        for (const QString &entry : entries) {
            if (entry.startsWith(QStringLiteral(".nasmount-tmp-"))) {
                leaked = true;
            }
        }
        check(QStringLiteral("no stray temp file left behind after the failure"), !leaked,
              entries.join(QStringLiteral(", ")));
        check(QStringLiteral("the target file was never created"), !QFile::exists(tmp.filePath(QStringLiteral("target"))));

        check(QStringLiteral("a name containing '/' is rejected before any syscall"), [&] {
            QString pathError;
            return !Root::DurableFs::durableReplace(dirFd, QStringLiteral("a/b"), QByteArrayLiteral("x"),
                                                    Root::DurableFs::ArtifactKind::SensitiveFile, &pathError);
        }());

        if (dirFd >= 0) {
            ::close(dirFd);
        }
    }

    out << "=== durableUnlink ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        const int dirFd = openDirFd(tmp.path());

        QString error;
        check(QStringLiteral("allowMissing=true on an absent file succeeds"),
              Root::DurableFs::durableUnlink(dirFd, QStringLiteral("absent"), true, &error), error);
        check(QStringLiteral("allowMissing=false on an absent file fails"),
              !Root::DurableFs::durableUnlink(dirFd, QStringLiteral("absent"), false, &error));

        QFile f(tmp.filePath(QStringLiteral("present")));
        check(QStringLiteral("file created"), f.open(QIODevice::WriteOnly));
        f.close();
        check(QStringLiteral("unlinking a file in a directory this test owns succeeds"),
              Root::DurableFs::durableUnlink(dirFd, QStringLiteral("present"), false, &error), error);
        check(QStringLiteral("it is actually gone"), !QFile::exists(tmp.filePath(QStringLiteral("present"))));

        if (dirFd >= 0) {
            ::close(dirFd);
        }
    }

    out << "=== durableRemoveTree ===" << Qt::endl;
    {
        QTemporaryDir tmp;
        const int parentFd = openDirFd(tmp.path());
        QString error;

        check(QStringLiteral("removing an already-absent tree is idempotent success"),
              Root::DurableFs::durableRemoveTree(parentFd, QStringLiteral("never-existed"),
                                                 Root::DurableFs::ArtifactKind::Directory,
                                                 Root::DurableFs::ArtifactKind::SensitiveFile, &error),
              error);

        QDir(tmp.path()).mkdir(QStringLiteral("mytree"));
        // Owned by this test's own uid, not root -- durableRemoveTree's
        // first step (openVerifiedDir) must refuse it rather than delete
        // through a directory it cannot prove is ours.
        const bool removed = Root::DurableFs::durableRemoveTree(parentFd, QStringLiteral("mytree"),
                                                                 Root::DurableFs::ArtifactKind::Directory,
                                                                 Root::DurableFs::ArtifactKind::SensitiveFile, &error);
        check(QStringLiteral("a non-root-owned directory is refused, not deleted through"), !removed, error);
        check(QStringLiteral("the directory still exists"), QDir(tmp.path()).exists(QStringLiteral("mytree")));

        if (parentFd >= 0) {
            ::close(parentFd);
        }
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
