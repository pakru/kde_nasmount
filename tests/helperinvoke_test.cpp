/*
 * Tests for helperinvoke's outcome classification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * KAuth::ExecuteJob itself is not mockable — it is a concrete class with no
 * injectable transport. classifyOutcome() is the actual decision surface
 * invokeHelperAction() reduces every call to (job->exec()'s bool plus
 * job->error()'s int), so exercising it directly with the codes KAuth's own
 * predefined ActionReply constants use *is* the transport-fake test: each
 * case below stands in for "the D-Bus call came back looking like this".
 */

#include "helperinvoke.h"

#include <KAuth/ActionReply>

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

static QString outcomeName(Session::HelperOutcome o)
{
    switch (o) {
    case Session::HelperOutcome::ConfirmedSuccess:
        return QStringLiteral("ConfirmedSuccess");
    case Session::HelperOutcome::ConfirmedFailure:
        return QStringLiteral("ConfirmedFailure");
    case Session::HelperOutcome::Unknown:
        return QStringLiteral("Unknown");
    }
    return QStringLiteral("?");
}

static void expect(const QString &label, bool execSucceeded, int jobError, Session::HelperOutcome want)
{
    const Session::HelperOutcome got = Session::classifyOutcome(execSucceeded, jobError);
    check(label, got == want, QStringLiteral("got %1, want %2").arg(outcomeName(got), outcomeName(want)));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== classifyOutcome: success ===" << Qt::endl;
    expect(QStringLiteral("job->exec() true -> ConfirmedSuccess regardless of a stray error code"), true,
          KAuth::ActionReply::DBusError, Session::HelperOutcome::ConfirmedSuccess);

    out << "=== classifyOutcome: rejected before/without dispatch -> ConfirmedFailure ===" << Qt::endl;
    expect(QStringLiteral("AuthorizationDeniedError (polkit denial)"), false,
          KAuth::ActionReply::AuthorizationDeniedError, Session::HelperOutcome::ConfirmedFailure);
    expect(QStringLiteral("UserCancelledError (auth prompt cancelled)"), false,
          KAuth::ActionReply::UserCancelledError, Session::HelperOutcome::ConfirmedFailure);
    expect(QStringLiteral("NoSuchActionError (bad action id)"), false, KAuth::ActionReply::NoSuchActionError,
          Session::HelperOutcome::ConfirmedFailure);
    expect(QStringLiteral("InvalidActionError (malformed action)"), false, KAuth::ActionReply::InvalidActionError,
          Session::HelperOutcome::ConfirmedFailure);
    expect(QStringLiteral("HelperBusyError (helper explicitly refused: busy)"), false,
          KAuth::ActionReply::HelperBusyError, Session::HelperOutcome::ConfirmedFailure);
    expect(QStringLiteral("AlreadyStartedError (helper explicitly refused: already running)"), false,
          KAuth::ActionReply::AlreadyStartedError, Session::HelperOutcome::ConfirmedFailure);

    out << "=== classifyOutcome: our own helper.cpp's explicit refusal -> ConfirmedFailure ===" << Qt::endl;
    // ActionReply::HelperErrorReply()'s fixed errorCode() (actionreply.h) --
    // every fail() call in helper.cpp produces exactly this.
    expect(QStringLiteral("errorCode() == -1 (HelperErrorReply)"), false, -1, Session::HelperOutcome::ConfirmedFailure);

    out << "=== classifyOutcome: transport-level, indistinguishable from a lost reply -> Unknown ===" << Qt::endl;
    expect(QStringLiteral("DBusError (the literal transport-error case)"), false, KAuth::ActionReply::DBusError,
          Session::HelperOutcome::Unknown);
    expect(QStringLiteral("NoResponderError (helper responder not wired)"), false,
          KAuth::ActionReply::NoResponderError, Session::HelperOutcome::Unknown);
    expect(QStringLiteral("BackendError (opaque backend failure)"), false, KAuth::ActionReply::BackendError,
          Session::HelperOutcome::Unknown);
    expect(QStringLiteral("an unrecognised code entirely (defensive default)"), false, 12345,
          Session::HelperOutcome::Unknown);

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
