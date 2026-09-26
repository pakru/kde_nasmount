/*
 * mountactions — the operation controller for Add and Delete.
 * There is no in-place Edit and no per-share runtime verb: changing a share's
 * UNC, mount point, credentials or authentication kind is Delete then Add
 * again, and a share is armed at boot
 * and mounts on first access rather than being armed or mounted by hand.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every public method here returns immediately and reports completion via
 * `finished()`. Nothing on the calling (GUI) thread blocks: the per-user lock
 * acquisition and the KAuth call (KAuth::ExecuteJob::exec()) run on a worker
 * thread, because both are unbounded waits on another process — a slow
 * polkit prompt would otherwise freeze System Settings.
 *
 * The per-user lock (Session::UserLock) is acquired first thing on the
 * worker thread and held across the Store snapshot read, the KAuth call and
 * the checked Store commit that follows — all of it, not just the KAuth
 * call — and is released only once the worker lambda returns.
 * The GUI-thread continuation only ever Q_EMITs finished(); it does not
 * itself touch Store.
 *
 * There is one lifecycle and therefore no mode routing: every share is
 * defined with a root-owned credential and armed at boot, so the helper
 * action names are fixed (definesystem/undefinesystem) rather than chosen
 * per share. An existing definition's properties are re-derived from the
 * validated marker by the helper itself — Store is never authoritative for
 * them.
 */

#pragma once

#include <QObject>
#include <QString>

namespace Session
{

/**
 * Guest and authenticated inputs must not be mixed: an empty
 * username means guest, which the helper represents as no credential at
 * all, so a non-empty password or domain alongside it cannot be honoured —
 * silently discarding them would surprise a caller who meant to
 * authenticate but mistyped the username. Exposed for testing.
 */
bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password);

class MountActions : public QObject
{
    Q_OBJECT

public:
    explicit MountActions(QObject *parent = nullptr);

    /** The only create there is: a share is boot-armed with a root-owned
     *  credential. Requires authentication; a polkit prompt is expected.
     *
     *  `access` is one of "readwrite", "readonly" or "readwrite-executable"
     *  — the same closed vocabulary the marker and the helper use, via
     *  UnitValue::accessModeToString(). A string rather than an enum because
     *  this is a QML boundary; it is passed through untouched and the helper
     *  remains the authoritative validator, since anything arriving here is
     *  untrusted. An empty string means "not specified" and the helper's
     *  read-write default applies.
     *
     *  Because there is no in-place Edit, this is the only opportunity to
     *  choose the access mode for a share. */
    Q_INVOKABLE void addShare(const QString &unc, const QString &rawMountPoint, const QString &username,
                              const QString &domain, const QString &password, const QString &access);

    /** Removes the definition and the local record. */
    Q_INVOKABLE void deleteShare(const QString &id);

    /**
     * Removes a Store record with no backing unit at all (Definition::None) —
     * nothing for the helper to act on, so this is a local KConfig removal
     * with no KAuth call.
     */
    Q_INVOKABLE void removeOrphanedRecord(const QString &id);

    /**
     * Path-based removal for a Pair or either-half Partial definition
     * discovered only by scanning the unit tree — no Store record exists
     * for it at all, so there is no id to key off. Authentication is
     * derived fresh from the validated marker, exactly like deleteShare().
     */
    Q_INVOKABLE void removeOrphanByPath(const QString &mountPoint);

Q_SIGNALS:
    void started(const QString &id, const QString &kind);
    void finished(const QString &id, const QString &kind, bool success, const QString &message);
};

} // namespace Session
