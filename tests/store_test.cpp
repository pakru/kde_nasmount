/*
 * Tests for Store's checked, compare-and-swap commit API (plan §1.6).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Deliberately never calls readPassword/writePassword/removePassword or
 * removeShare (which itself calls removePassword): those open a real KWallet
 * over D-Bus, which is not available — and must not be required — for an
 * unprivileged, offline unit test. What is covered here is everything
 * reachable through KConfig alone: checked insert/update, generation-conflict
 * rejection standing in for two racing cooperating clients (plan §1.6.7), and
 * corrupt-record visibility (plan §1.6.5).
 *
 * XDG_CONFIG_HOME is redirected to a fresh temporary directory before any
 * KConfig object is touched, so this never reads or writes the real user's
 * nasmountrc.
 */

#include "store.h"
#include "userlock.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <atomic>

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
    // Deliberately NOT QStandardPaths::setTestModeEnabled(true): that
    // redirects to a fixed ~/.qttest path that persists across separate test
    // runs instead of honouring XDG_CONFIG_HOME, which reintroduces the
    // exact cross-run contamination a fresh QTemporaryDir is meant to avoid.
    QTemporaryDir configHome;
    if (!configHome.isValid() || !qputenv("XDG_CONFIG_HOME", configHome.path().toLocal8Bit())) {
        QTextStream(stdout) << "cannot isolate XDG_CONFIG_HOME for testing" << Qt::endl;
        return 1;
    }

    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== commitShare: fresh insert and read-back ===" << Qt::endl;
    {
        Store::Share share;
        share.id = QStringLiteral("0000000000000000000000000000001");
        share.unc = QStringLiteral("//host/Share");
        share.mountPoint = QStringLiteral("/mnt/one");
        share.username = QStringLiteral("alice");

        QString error;
        check(QStringLiteral("insert (expectedGeneration=0) succeeds"),
              Store::commitShare(share, 0, &error) == Store::CommitResult::Ok, error);

        const Store::Snapshot snap = Store::snapshotById(share.id);
        check(QStringLiteral("record exists after insert"), snap.exists);
        check(QStringLiteral("record is not corrupt"), !snap.corrupt);
        check(QStringLiteral("fields round-trip"),
              snap.share.unc == share.unc && snap.share.mountPoint == share.mountPoint
                  && snap.share.username == share.username);
        check(QStringLiteral("generation is 1 after one commit"), snap.generation == 1, QString::number(snap.generation));
    }

    out << "=== commitShare: generation-conflict on a duplicate insert (plan §1.6.7) ===" << Qt::endl;
    {
        // Simulates two cooperating clients that both believe the id is
        // unused: the second commitShare(..., 0, ...) must not silently
        // overwrite the first's record.
        Store::Share first;
        first.id = QStringLiteral("0000000000000000000000000000002");
        first.unc = QStringLiteral("//host/First");
        first.mountPoint = QStringLiteral("/mnt/first");
        QString error;
        check(QStringLiteral("first client's insert succeeds"), Store::commitShare(first, 0, &error) == Store::CommitResult::Ok, error);

        Store::Share second = first;
        second.unc = QStringLiteral("//host/Second");
        second.mountPoint = QStringLiteral("/mnt/second");
        const Store::CommitResult secondResult = Store::commitShare(second, 0, &error);
        check(QStringLiteral("second client's insert at the same id is rejected"),
              secondResult == Store::CommitResult::GenerationConflict, error);

        const Store::Snapshot snap = Store::snapshotById(first.id);
        check(QStringLiteral("the first client's record survives untouched"), snap.share.unc == first.unc,
              snap.share.unc);
    }

    out << "=== commitShare: generation-conflict on a stale update (plan §1.6.7) ===" << Qt::endl;
    {
        Store::Share share;
        share.id = QStringLiteral("0000000000000000000000000000003");
        share.unc = QStringLiteral("//host/Stale");
        share.mountPoint = QStringLiteral("/mnt/stale");
        QString error;
        check(QStringLiteral("insert succeeds"), Store::commitShare(share, 0, &error) == Store::CommitResult::Ok, error);

        // Client A reads generation 1 and commits an update -- generation
        // becomes 2.
        const Store::Snapshot readByA = Store::snapshotById(share.id);
        Store::Share updatedByA = readByA.share;
        updatedByA.domain = QStringLiteral("A-was-here");
        check(QStringLiteral("client A's update (generation 1 -> 2) succeeds"),
              Store::commitShare(updatedByA, readByA.generation, &error) == Store::CommitResult::Ok, error);

        // Client B is still holding its own earlier read at generation 1 and
        // tries to commit against it -- must be rejected, not silently
        // clobber A's change.
        Store::Share updatedByB = readByA.share;
        updatedByB.domain = QStringLiteral("B-was-here");
        const Store::CommitResult bResult = Store::commitShare(updatedByB, readByA.generation, &error);
        check(QStringLiteral("client B's stale-generation update is rejected"),
              bResult == Store::CommitResult::GenerationConflict, error);

        const Store::Snapshot final_ = Store::snapshotById(share.id);
        check(QStringLiteral("A's change is the one that stuck"), final_.share.domain == QStringLiteral("A-was-here"),
              final_.share.domain);
        check(QStringLiteral("generation is now 2"), final_.generation == 2, QString::number(final_.generation));
    }

    out << "=== commitShare: an update against a since-deleted record is rejected ===" << Qt::endl;
    {
        Store::Share share;
        share.id = QStringLiteral("0000000000000000000000000000004");
        share.unc = QStringLiteral("//host/Deleted");
        share.mountPoint = QStringLiteral("/mnt/deleted");
        QString error;
        Store::commitShare(share, 0, &error);
        const Store::Snapshot snap = Store::snapshotById(share.id);

        // Directly remove the group to simulate a concurrent removeShare()
        // without touching KWallet.
        {
            auto config = KSharedConfig::openConfig(QStringLiteral("nasmountrc"));
            KConfigGroup root = config->group(QStringLiteral("Shares"));
            root.deleteGroup(share.id);
            config->sync();
        }

        Store::Share updated = snap.share;
        updated.domain = QStringLiteral("too-late");
        const Store::CommitResult result = Store::commitShare(updated, snap.generation, &error);
        check(QStringLiteral("update against a vanished record is rejected, not treated as a fresh insert"),
              result == Store::CommitResult::GenerationConflict, error);
    }

    out << "=== snapshotById: absent id ===" << Qt::endl;
    {
        const Store::Snapshot snap = Store::snapshotById(QStringLiteral("does-not-exist"));
        check(QStringLiteral("exists is false"), !snap.exists);
        check(QStringLiteral("corrupt is false for an absent record (nothing to be corrupt)"), !snap.corrupt);
    }

    out << "=== shareSnapshots: corrupt records are surfaced, not skipped (plan §1.6.5) ===" << Qt::endl;
    {
        // Write a group missing MountPoint directly, bypassing the checked
        // API, to simulate a hand-edited or partially-written config file.
        auto config = KSharedConfig::openConfig(QStringLiteral("nasmountrc"));
        KConfigGroup root = config->group(QStringLiteral("Shares"));
        KConfigGroup g = root.group(QStringLiteral("corrupt-one"));
        g.writeEntry("Unc", QStringLiteral("//host/OnlyUnc"));
        // MountPoint deliberately omitted.
        g.writeEntry("Generation", 1LL);
        config->sync();

        const QList<Store::Snapshot> all = Store::shareSnapshots();
        bool found = false;
        bool corrupt = false;
        for (const Store::Snapshot &s : all) {
            if (s.share.id == QStringLiteral("corrupt-one")) {
                found = true;
                corrupt = s.corrupt;
            }
        }
        check(QStringLiteral("the corrupt group is present in shareSnapshots()"), found);
        check(QStringLiteral("it is flagged corrupt, not silently skipped"), corrupt);

        const QList<Store::Share> wellFormedOnly = Store::shares();
        bool leaked = false;
        for (const Store::Share &s : wellFormedOnly) {
            if (s.id == QStringLiteral("corrupt-one")) {
                leaked = true;
            }
        }
        check(QStringLiteral("shares() still filters the corrupt one out for callers that want only well-formed records"),
              !leaked);
    }

    out << "=== Access: round-trips, and a record without the key is read-write ===" << Qt::endl;
    {
        QString error;
        const struct {
            const char *id;
            const char *access;
        } cases[] = {
            {"00000000000000000000000000000a01", "readwrite"},
            {"00000000000000000000000000000a02", "readonly"},
            {"00000000000000000000000000000a03", "readwrite-executable"},
        };
        for (const auto &c : cases) {
            Store::Share share;
            share.id = QString::fromLatin1(c.id);
            share.unc = QStringLiteral("//host/Share");
            share.mountPoint = QStringLiteral("/mnt/") + QString::fromLatin1(c.id);
            share.username = QStringLiteral("alice");
            share.access = QString::fromLatin1(c.access);
            check(QStringLiteral("%1: insert succeeds").arg(QLatin1String(c.access)),
                  Store::commitShare(share, 0, &error) == Store::CommitResult::Ok, error);

            const Store::Snapshot snap = Store::snapshotById(share.id);
            check(QStringLiteral("%1: access round-trips").arg(QLatin1String(c.access)),
                  snap.share.access == share.access, snap.share.access);
        }

        // Every record written before 0.1.4 looks like this: no Access key at
        // all. It must read back as read-write, which is what those shares
        // are -- and it must NOT be treated as a corrupt group, or every
        // pre-upgrade row in the KCM breaks on first launch after the update.
        auto config = KSharedConfig::openConfig(QStringLiteral("nasmountrc"));
        KConfigGroup root = config->group(QStringLiteral("Shares"));
        KConfigGroup g = root.group(QStringLiteral("00000000000000000000000000000a04"));
        g.writeEntry("Unc", QStringLiteral("//host/Legacy"));
        g.writeEntry("MountPoint", QStringLiteral("/mnt/legacy"));
        g.writeEntry("Username", QStringLiteral("alice"));
        // Access deliberately omitted, exactly as 0.1.0-0.1.3 wrote it.
        g.writeEntry("Generation", 1LL);
        config->sync();

        const Store::Snapshot legacy =
            Store::snapshotById(QStringLiteral("00000000000000000000000000000a04"));
        check(QStringLiteral("a record with no Access key still exists"), legacy.exists);
        check(QStringLiteral("a record with no Access key is not corrupt"), !legacy.corrupt);
        check(QStringLiteral("a record with no Access key reads back as readwrite"),
              legacy.share.access == QStringLiteral("readwrite"), legacy.share.access);
    }

    out << "=== UserLock: a second acquirer blocks until the first releases (plan §1.6.7) ===" << Qt::endl;
    {
        QString err1;
        auto first = Session::UserLock::acquire(&err1);
        check(QStringLiteral("first lock acquired"), first != nullptr, err1);

        std::atomic<bool> secondAcquired{false};
        QThread worker;
        QObject::connect(&worker, &QThread::started, [&]() {
            QString err2;
            auto second = Session::UserLock::acquire(&err2);
            secondAcquired = true;
            worker.quit();
            // second released here as the lambda returns.
        });
        worker.start();

        QThread::msleep(200);
        check(QStringLiteral("second acquirer is still blocked while the first holds the lock"), !secondAcquired.load());

        first.reset();
        check(QStringLiteral("second acquirer proceeds once the first releases"), worker.wait(5000));
        check(QStringLiteral("second did in fact acquire"), secondAcquired.load());
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
