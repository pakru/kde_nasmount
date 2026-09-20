/*
 * Tests for Root::Arming (plan §2.5, §4.1).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * armShare()/arm()/disarm() all write below
 * /etc/nasmount and /etc/systemd/system and call real systemctl, so — like
 * every other kde_nasmount-root test file — a real accept path needs root and
 * belongs to VM integration testing. evaluateArmPrecheck() is the one piece
 * of this module that is pure decision logic over an already-computed
 * snapshot (design §6.4's "never bless an unrecorded trigger"); it is
 * exposed specifically so this file can exercise its full decision table
 * without systemctl or mountinfo access.
 */

#include "arming.h"

#include <QCoreApplication>
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

    out << "=== evaluateStopPrecheck: safe-stop correlation table ===" << Qt::endl;
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Inactive;
        snap.mount = Verify::MountState::Absent;
        QString error;
        check(QStringLiteral("inactive runtime -> AlreadyInactive"),
              Root::Arming::evaluateStopPrecheck(snap, &error)
                  == Root::Arming::StopPrecheck::AlreadyInactive);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Active;
        snap.mount = Verify::MountState::Absent;
        snap.activationTrust = Verify::ActivationTrust::Trusted;
        QString error;
        check(QStringLiteral("trusted active trigger -> ShouldStop"),
              Root::Arming::evaluateStopPrecheck(snap, &error)
                  == Root::Arming::StopPrecheck::ShouldStop);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Active;
        snap.mount = Verify::MountState::Absent;
        snap.activationTrust = Verify::ActivationTrust::Untrusted;
        QString error;
        check(QStringLiteral("untrusted active trigger -> CorrelationMismatch"),
              Root::Arming::evaluateStopPrecheck(snap, &error)
                  == Root::Arming::StopPrecheck::CorrelationMismatch);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Inactive;
        snap.mount = Verify::MountState::Present;
        snap.verification = Verify::VerificationState::Match;
        QString error;
        check(QStringLiteral("matching live mount -> ShouldStop"),
              Root::Arming::evaluateStopPrecheck(snap, &error)
                  == Root::Arming::StopPrecheck::ShouldStop);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Inactive;
        snap.mount = Verify::MountState::Present;
        snap.verification = Verify::VerificationState::Mismatch;
        QString error;
        check(QStringLiteral("foreign live mount -> CorrelationMismatch"),
              Root::Arming::evaluateStopPrecheck(snap, &error)
                  == Root::Arming::StopPrecheck::CorrelationMismatch);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Indeterminate;
        snap.mount = Verify::MountState::Absent;
        QString error;
        check(QStringLiteral("indeterminate runtime -> Indeterminate"),
              Root::Arming::evaluateStopPrecheck(snap, &error)
                  == Root::Arming::StopPrecheck::Indeterminate);
    }

    out << "=== evaluateArmPrecheck: the never-bless-an-unrecorded-trigger decision table ===" << Qt::endl;
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Inactive;
        snap.mount = Verify::MountState::Absent;
        QString error;
        check(QStringLiteral("inactive automount, nothing mounted -> ReadyToArm"),
              Root::Arming::evaluateArmPrecheck(snap, &error) == Root::Arming::ArmPrecheck::ReadyToArm);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Active;
        snap.mount = Verify::MountState::Absent;
        snap.activationTrust = Verify::ActivationTrust::Trusted;
        QString error;
        check(QStringLiteral("active automount with a trusted (matching) id -> AlreadyArmed, idempotent"),
              Root::Arming::evaluateArmPrecheck(snap, &error) == Root::Arming::ArmPrecheck::AlreadyArmed);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Active;
        snap.mount = Verify::MountState::Absent;
        snap.activationTrust = Verify::ActivationTrust::Untrusted;
        QString error;
        check(QStringLiteral("active automount with an untrusted/unrecorded id -> Blocked, never blessed"),
              Root::Arming::evaluateArmPrecheck(snap, &error) == Root::Arming::ArmPrecheck::Blocked);
        check(QStringLiteral("error explains why"), !error.isEmpty());
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Inactive;
        snap.mount = Verify::MountState::Present;
        snap.verification = Verify::VerificationState::Mismatch;
        QString error;
        check(QStringLiteral("a live (foreign) mount already occupies the path -> Blocked"),
              Root::Arming::evaluateArmPrecheck(snap, &error) == Root::Arming::ArmPrecheck::Blocked);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Indeterminate;
        snap.mount = Verify::MountState::Absent;
        QString error;
        check(QStringLiteral("indeterminate automount state -> Blocked, fails closed"),
              Root::Arming::evaluateArmPrecheck(snap, &error) == Root::Arming::ArmPrecheck::Blocked);
    }
    {
        Verify::RuntimeSnapshot snap;
        snap.automount = Verify::AutomountState::Inactive;
        snap.mount = Verify::MountState::Indeterminate;
        QString error;
        check(QStringLiteral("indeterminate mount state -> Blocked, fails closed"),
              Root::Arming::evaluateArmPrecheck(snap, &error) == Root::Arming::ArmPrecheck::Blocked);
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
