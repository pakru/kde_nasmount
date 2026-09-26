/*
 * nasmount KAuth helper — the privileged half.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs as root, activated on the system bus by KAuth. Everything in the
 * argument map is untrusted: the dialog and KCM are unprivileged and a
 * hostile process can call these actions directly, so every check happens
 * here, freshly, under the root lock, regardless of what a caller claims.
 *
 * Caller identity comes from KAuth::HelperSupport::callerUid(), never from
 * the argument map, so the mount-point allowlist, the uid=/gid= options and
 * the owner-uid marker are scoped to who really called.
 *
 * This file is deliberately thin: caller validation, typed
 * argument decoding, root-lock acquisition, dispatch into kde_nasmount-root, and
 * structured reply conversion. Every privileged filesystem/systemd mutation
 * lives in kde_nasmount-root (durablefs, credentialstore, runtimefiles,
 * systemdops, arming, operations) — nothing here writes a file, starts/stops
 * a unit, or touches a credential directly.
 */

#include "arming.h"
#include "inventory.h"
#include "operations.h"
#include "rootlock.h"
#include "systemdops.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <KAuth/ActionReply>
#include <KAuth/HelperSupport>

#include <QCoreApplication>
#include <QVariantMap>

#include <pwd.h>
#include <unistd.h>

using namespace KAuth;

namespace
{

/**
 * Control-character and per-field size checks shared by every call site that
 * accepts credential fields, so no path can apply a looser bound than another.
 * The same limits
 * are enforced again inside Root::CredentialStore::write() — this is the
 * fast, early rejection; that is the one no call path can skip.
 */
bool validateCredentialFields(const QString &username, const QString &domain, const QString &password,
                              QString *error)
{
    if (UnitSpec::hasControlChars(username) || UnitSpec::hasControlChars(domain)
        || UnitSpec::hasControlChars(password)) {
        *error = QStringLiteral("credential fields contain control characters");
        return false;
    }
    if (username.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes
        || domain.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes
        || password.toUtf8().size() > UnitSpec::MaxCredentialFieldBytes) {
        *error = QStringLiteral("a credential field exceeds %1 bytes").arg(UnitSpec::MaxCredentialFieldBytes);
        return false;
    }
    return true;
}

/**
 * An empty username means guest, so domain and password must also be empty:
 * the helper rejects the request rather than silently discarding them, which
 * would mount a share as guest that the user believed was authenticated.
 * Shared by every action that accepts credential fields alongside a username.
 */
bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password)
{
    return !username.isEmpty() || (domain.isEmpty() && password.isEmpty());
}

ActionReply fail(const QString &message)
{
    ActionReply reply = ActionReply::HelperErrorReply();
    reply.setErrorDescription(message);
    return reply;
}

ActionReply ok(const QString &message = QString(), const QVariantMap &extra = {})
{
    ActionReply reply = ActionReply::SuccessReply();
    if (!message.isEmpty()) {
        reply.addData(QStringLiteral("message"), message);
    }
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
        reply.addData(it.key(), it.value());
    }
    return reply;
}

struct CallerInfo {
    uid_t uid = 0;
    gid_t gid = 0;
    QString home;
};

bool resolveCaller(CallerInfo *info, QString *error)
{
    const int uid = HelperSupport::callerUid();
    if (uid < 0) {
        *error = QStringLiteral("cannot determine the calling user");
        return false;
    }
    const struct passwd *pw = ::getpwuid(static_cast<uid_t>(uid));
    if (!pw) {
        *error = QStringLiteral("uid %1 does not correspond to a real user").arg(uid);
        return false;
    }
    info->uid = pw->pw_uid;
    info->gid = pw->pw_gid;
    info->home = QString::fromLocal8Bit(pw->pw_dir);
    return true;
}

/** Resolves caller + path + unit paths together, the first step of every action. */
bool prepare(const QVariantMap &args, CallerInfo *caller, UnitSpec::MountpointPlan *plan,
            UnitValue::UnitPaths *paths, QString *error)
{
    if (!resolveCaller(caller, error)) {
        return false;
    }
    if (!UnitSpec::validateMountpoint(args.value(QStringLiteral("path")).toString(), caller->home, plan, error)) {
        return false;
    }
    return UnitValue::unitPathsFor(plan->path, paths, error);
}

} // namespace

class NasMountHelper : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    ActionReply definesystem(const QVariantMap &args);
    ActionReply undefinesystem(const QVariantMap &args);
    ActionReply inventory(const QVariantMap &args);
    ActionReply purge(const QVariantMap &args);
};

namespace
{

/**
 * The body of `definesystem`. A fresh create only — an existing Partial half
 * is never repaired; it must be removed first.
 *
 * There is one lifecycle, so the share's mode is fixed by the action itself
 * and never read from `args`. `access` is the one exception:
 * it is a genuine user choice with no second action to encode it in, so it
 * arrives as an argument and is validated here like every other untrusted
 * input.
 */
ActionReply doDefine(const QVariantMap &args)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    QString unc;
    if (!UnitSpec::validateUnc(args.value(QStringLiteral("unc")).toString(), &unc, &error)) {
        return fail(error);
    }
    const QString username = args.value(QStringLiteral("username")).toString();
    const QString domain = args.value(QStringLiteral("domain")).toString();
    const QString password = args.value(QStringLiteral("password")).toString();

    // Absent must mean read-write, not fail: across an upgrade a still-running
    // old front end calls this new helper and will not send the key at all
    // (AGENTS.md upgrade rule 5), and read-write is exactly what it asked for.
    // A *present* but unrecognised value is a hard failure — defaulting a typo
    // would give the user the opposite of the access they picked, silently.
    UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite;
    if (args.contains(QStringLiteral("access"))) {
        const QString requested = args.value(QStringLiteral("access")).toString();
        if (!UnitValue::accessModeFromString(requested, &access)) {
            return fail(QStringLiteral("unrecognised access mode"));
        }
    }
    // Define takes domain/password and writes the credential itself: there is
    // no later step that could supply them, so both are
    // validated here. validateCredentialFields() covers the username's own
    // control characters too.
    if (!validateCredentialFields(username, domain, password, &error)) {
        return fail(error);
    }
    if (!guestFieldsConsistent(username, domain, password)) {
        return fail(
            QStringLiteral("a share with no username is guest, and cannot also have a password or domain"));
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state == Verify::Definition::Partial) {
        return fail(QStringLiteral("cannot define %1: a broken definition already exists here — remove it first")
                        .arg(plan.path));
    }
    if (def.state != Verify::Definition::None) {
        return fail(QStringLiteral("cannot define %1: %2")
                        .arg(plan.path,
                             def.detail.isEmpty() ? QStringLiteral("a definition already exists") : def.detail));
    }

    // The full descriptor walk (not only at arm): without it, define() would
    // happily write a unit whose Where= is a non-empty directory or an
    // ancestor of another mount's Where=. That unit is `static` and never
    // armed by define alone, but systemd still loads it, and
    // systemd.mount(5) makes any mount unit automatically a dependency of
    // another mount unit nested beneath it in the filesystem — so a
    // mistakenly-placed, never-armed definition can still break unrelated,
    // already-working mounts the moment daemon-reload runs.
    const int mpFd = UnitSpec::openMountpointNoFollow(plan, caller.uid, caller.gid, &error);
    if (mpFd < 0) {
        return fail(error);
    }
    ::close(mpFd);

    Root::Operations::DefineInput input;
    input.ownerUid = caller.uid;
    input.ownerGid = caller.gid;
    input.unc = unc;
    input.mountPoint = plan.path;
    input.unitName = paths.unitName;
    input.username = username;
    input.access = access;
    input.domain = domain;
    input.password = password;
    input.mountPlan = plan; // needed for the immediate-arm path walk

    const Root::Operations::DefineOutput result = Root::Operations::define(input);
    if (!result.ok) {
        return fail(result.error);
    }
    // The client indexes its Store record by this id -- it is never allowed
    // to invent its own. `activated` reflects the real immediate-arm result.
    return ok(QStringLiteral("Defined %1 -> %2").arg(unc, plan.path),
             {{QStringLiteral("id"), result.shareId}, {QStringLiteral("activated"), result.activated}});
}

} // namespace

ActionReply NasMountHelper::definesystem(const QVariantMap &args)
{
    return doDefine(args);
}

namespace
{

/**
 * The body of `undefinesystem`. Accepts a complete pair or an owned one-sided
 * Partial, so a half left by an interrupted Add can still be removed through
 * the UI rather than by hand. Owner, id and authentication kind come from the
 * validated marker, never from `args`.
 */
ActionReply doUndefine(const QVariantMap &args)
{
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    UnitSpec::MountpointPlan plan;
    UnitValue::UnitPaths paths;
    if (!prepare(args, &caller, &plan, &paths, &error)) {
        return fail(error);
    }

    const auto def = Verify::inspectDefinition(paths, caller.uid, plan.path);
    if (def.state != Verify::Definition::Pair && def.state != Verify::Definition::Partial) {
        return fail(QStringLiteral("cannot undefine %1: %2")
                        .arg(plan.path,
                             def.detail.isEmpty() ? QStringLiteral("no definition owned by you exists") : def.detail));
    }
    Root::Operations::RemovalInput input;
    input.ownerUid = def.ownerUid;
    input.ownerGid = def.ownerGid;
    input.shareId = def.id;
    input.authentication = def.authentication;
    input.mountPoint = plan.path;
    input.unitName = paths.unitName;
    input.what = def.what;

    const Root::Operations::RemovalOutput result = Root::Operations::remove(input);
    if (!result.ok) {
        return fail(result.error);
    }
    return ok(QStringLiteral("Undefined %1").arg(plan.path));
}

} // namespace

ActionReply NasMountHelper::undefinesystem(const QVariantMap &args)
{
    return doUndefine(args);
}

ActionReply NasMountHelper::inventory(const QVariantMap &args)
{
    // Read-only, caller-scoped: no argument is ever
    // read from `args` -- every record is derived from what is actually on
    // disk for the resolved caller uid, never from anything the caller
    // claims.
    Q_UNUSED(args);
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    if (!resolveCaller(&caller, &error)) {
        return fail(error);
    }

    QString buildError;
    const QList<Root::Inventory::ShareRecord> records = Root::Inventory::buildFor(caller.uid, &buildError);
    if (!buildError.isEmpty()) {
        return fail(buildError);
    }
    return ok(QStringLiteral("%1 share(s)").arg(records.size()),
             {{QStringLiteral("shares"), Root::Inventory::toJson(records)}});
}

ActionReply NasMountHelper::purge(const QVariantMap &args)
{
    // The action accepts no paths, ids, or mode supplied by the caller. Its
    // scope is derived exclusively from the authenticated caller and the
    // verified root-owned artifacts on disk.
    Q_UNUSED(args);
    QString error;
    auto lock = Root::RootLock::acquire(&error);
    if (!lock) {
        return fail(error);
    }

    CallerInfo caller;
    if (!resolveCaller(&caller, &error)) {
        return fail(error);
    }
    const Root::Operations::PurgeOutput result = Root::Operations::purge(caller.uid);
    if (!result.ok) {
        return fail(result.error);
    }
    return ok(QStringLiteral("purged %1 managed share(s)").arg(result.removedShares));
}

KAUTH_HELPER_MAIN("io.github.pakru.nasmount", NasMountHelper)

#include "helper.moc"
