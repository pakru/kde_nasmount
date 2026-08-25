/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "helperinvoke.h"

#include <KAuth/Action>
#include <KAuth/ActionReply>
#include <KAuth/ExecuteJob>

#include <memory>

namespace Session
{

/** Whether KAuth rejected the call before our helper code could run. Kept
 *  next to classifyOutcome() so the two lists cannot drift apart. */
bool rejectedBeforeDispatch(bool execSucceeded, int jobError)
{
    if (execSucceeded) {
        return false;
    }
    switch (jobError) {
    case KAuth::ActionReply::AuthorizationDeniedError:
    case KAuth::ActionReply::UserCancelledError:
    case KAuth::ActionReply::NoSuchActionError:
    case KAuth::ActionReply::InvalidActionError:
    case KAuth::ActionReply::HelperBusyError:
    case KAuth::ActionReply::AlreadyStartedError:
        return true;
    default:
        // -1 is our own helper's HelperErrorReply: it ran. Everything else is
        // indeterminate.
        return false;
    }
}

HelperOutcome classifyOutcome(bool execSucceeded, int jobError)
{
    if (execSucceeded) {
        return HelperOutcome::ConfirmedSuccess;
    }
    switch (jobError) {
    case KAuth::ActionReply::AuthorizationDeniedError:
    case KAuth::ActionReply::UserCancelledError:
    case KAuth::ActionReply::NoSuchActionError:
    case KAuth::ActionReply::InvalidActionError:
    case KAuth::ActionReply::HelperBusyError:
    case KAuth::ActionReply::AlreadyStartedError:
        // KAuth's own machinery rejected the call before our helper code ran.
        return HelperOutcome::ConfirmedFailure;
    case -1:
        // ActionReply::HelperErrorReply()'s fixed errorCode() (actionreply.h)
        // — every explicit refusal our own helper.cpp raises. The helper ran
        // and evaluated the request; it just said no.
        return HelperOutcome::ConfirmedFailure;
    default:
        // NoResponderError, DBusError, BackendError, or anything else this
        // classification does not specifically recognise: indistinguishable
        // from a lost reply after the helper may have already started.
        return HelperOutcome::Unknown;
    }
}

HelperResult invokeHelperAction(const QString &action, const QVariantMap &args)
{
    KAuth::Action act(QStringLiteral("io.github.pakru.nasmount.%1").arg(action));
    act.setHelperId(QStringLiteral("io.github.pakru.nasmount"));
    act.setArguments(args);
    std::unique_ptr<KAuth::ExecuteJob> job(act.execute());
    const bool execSucceeded = job->exec();

    HelperResult result;
    result.outcome = classifyOutcome(execSucceeded, job->error());
    result.rejectedBeforeDispatch = rejectedBeforeDispatch(execSucceeded, job->error());
    if (execSucceeded) {
        result.data = job->data();
        result.message = result.data.value(QStringLiteral("message")).toString();
        result.id = result.data.value(QStringLiteral("id")).toString();
        result.activated = result.data.value(QStringLiteral("activated")).toBool();
    } else {
        result.message = job->errorText().isEmpty() ? QStringLiteral("the operation failed") : job->errorText();
    }
    return result;
}

} // namespace Session
