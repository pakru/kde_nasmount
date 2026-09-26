/*
 * helperinvoke — the one place a KAuth action against the nasmount helper is
 * issued from, shared by mountactions and the model (both on worker threads)
 * and nasmount-cleanup (a plain command-line tool, where a blocking call is
 * fine).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A boolean result cannot represent what KAuth actually tells the caller
 * Authorization denial and an explicit error from our own
 * helper.cpp both mean the call provably did not mutate state — a confirmed
 * failure. A D-Bus timeout or disconnect after the call may have been
 * dispatched means exactly the opposite: the helper may have already acted,
 * and there is no way to tell from this reply alone. Collapsing that into
 * "false" is how a lost acknowledgement turns into a duplicate share or a
 * silently-orphaned definition — the caller must be able to tell the three
 * apart and act (or, for Unknown, deliberately not act) accordingly.
 */

#pragma once

#include <QString>
#include <QVariantMap>

namespace Session
{

enum class HelperOutcome {
    ConfirmedSuccess, ///< the helper ran and reported success
    ConfirmedFailure, ///< rejected before dispatch (policy/invalid action), or the helper ran and explicitly refused
    Unknown,           ///< dispatch may have happened; the acknowledgement was lost (timeout/disconnect/backend error)
};

/**
 * The full result of one helper call. `message` carries the helper's own
 * text on success, or the error description otherwise — human-readable, not
 * meant for programmatic branching. `id` and `activated` are populated only
 * when the helper action returns them; otherwise they stay at their
 * defaults. `data` is the
 * complete raw reply map, for reconciliation fields not yet promoted to a
 * typed field here.
 */
struct HelperResult {
    HelperOutcome outcome = HelperOutcome::Unknown;
    /**
     * True only when KAuth's own machinery rejected the call and our helper
     * code provably never ran — authorization denied, user cancelled, unknown
     * or invalid action, helper busy/already started.
     *
     * `outcome == ConfirmedFailure` alone does not imply this: it also covers
     * our helper.cpp running and returning HelperErrorReply, and a privileged
     * action is not necessarily atomic, so such a refusal can arrive after the
     * helper has already changed system state. Only this flag distinguishes
     * "nothing happened" from "it ran and then said no", which is the
     * difference nasmount-cleanup reports to the user.
     */
    bool rejectedBeforeDispatch = false;
    QString message;
    QString id;             ///< the stable share id, when the helper action returns one
    bool activated = false; ///< whether the call is known to have left the share armed/active
    QVariantMap data;
};

/**
 * Classifies a completed KAuth::ExecuteJob outcome from its raw KJob result,
 * without touching KAuth types directly — a pure function so the
 * classification itself is unit-testable without a live D-Bus transport.
 *
 * `execSucceeded` is `job->exec()`'s own result; `jobError` is `job->error()`
 * when it was false. Authorization denial, user cancellation, an invalid/
 * unknown action id, or an explicit helper-busy/already-started refusal all
 * mean KAuth's own machinery rejected the call before our helper code ran —
 * ConfirmedFailure. `-1` is what our helper.cpp's own
 * `ActionReply::HelperErrorReply()` always carries — also ConfirmedFailure,
 * since the helper ran and explicitly said no. Anything else (no responder,
 * a D-Bus transport error, an opaque backend error, or any other code this
 * classification does not specifically recognise) cannot be distinguished
 * from a lost reply after the helper may have already started — Unknown.
 */
HelperOutcome classifyOutcome(bool execSucceeded, int jobError);

/**
 * Runs `action` against the nasmount helper and blocks the calling thread
 * until the D-Bus call and polkit check complete.
 */
HelperResult invokeHelperAction(const QString &action, const QVariantMap &args);

} // namespace Session
