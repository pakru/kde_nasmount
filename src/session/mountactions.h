/*
 * mountactions - the operation controller for Add and Delete.
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
 * thread, because both are unbounded waits on another process - a slow
 * polkit prompt would otherwise freeze System Settings.
 *
 * The per-user lock (Session::UserLock) is acquired first thing on the
 * worker thread and held across the Store snapshot read, the KAuth call and
 * the checked Store commit that follows - all of it, not just the KAuth
 * call - and is released only once the worker lambda returns.
 * The GUI-thread continuation only ever Q_EMITs finished(); it does not
 * itself touch Store.
 *
 * There is one lifecycle and therefore no mode routing: every share is
 * defined with a root-owned credential and armed at boot, so the helper
 * action names are fixed (definesystem/undefinesystem) rather than chosen
 * per share. An existing definition's properties are re-derived from the
 * validated marker by the helper itself - Store is never authoritative for
 * them.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVariantMap>

#include <sys/types.h>

namespace Session
{

/**
 * Guest and authenticated inputs must not be mixed: an empty
 * username means guest, which the helper represents as no credential at
 * all, so a non-empty password or domain alongside it cannot be honoured -
 * silently discarding them would surprise a caller who meant to
 * authenticate but mistyped the username. Exposed for testing.
 */
bool guestFieldsConsistent(const QString &username, const QString &domain, const QString &password);

/**
 * The local path a folder picker's URL names, or empty when it names anything
 * else (a network location, or a file URL carrying a host).
 *
 * QUrl::toLocalFile() and nothing hand-rolled: a URL's string form keeps `%`
 * and `#` percent-encoded, so cutting the scheme off it turns a folder named
 * "100%" into "100%25" - a different directory that the helper would then
 * create and mount on. Exposed for testing.
 */
QString localPathFromUrl(const QUrl &url);

class MountActions : public QObject
{
    Q_OBJECT

public:
    explicit MountActions(QObject *parent = nullptr);

    /** The only create there is: a share is boot-armed with a root-owned
     *  credential. Requires authentication; a polkit prompt is expected.
     *
     *  `shareInput` is the Add field's text in either spelling, smb://host/share or
     *  //host/share. It is resolved by ShareAddress::resolveShareInput()
     *  before anything else happens, and only the resulting //host/share is
     *  ever passed to the helper or written to Store: that form is what the
     *  unit, the helper and the drift comparison all use. A refused address
     *  is reported through finished() without a KAuth call.
     *
     *  `access` is one of "readwrite", "readonly" or "readwrite-executable"
     *  - the same closed vocabulary the marker and the helper use, via
     *  UnitValue::accessModeToString(). A string rather than an enum because
     *  this is a QML boundary; it is passed through untouched and the helper
     *  remains the authoritative validator, since anything arriving here is
     *  untrusted. An empty string means "not specified" and the helper's
     *  read-write default applies.
     *
     *  Because there is no in-place Edit, this is the only opportunity to
     *  choose the access mode for a share. */
    Q_INVOKABLE void addShare(const QString &shareInput, const QString &rawMountPoint, const QString &username,
                              const QString &domain, const QString &password, const QString &access);

    /** Removes the definition and the local record. */
    Q_INVOKABLE void deleteShare(const QString &id);

    /**
     * Removes a Store record with no backing unit at all (Definition::None) -
     * nothing for the helper to act on, so this is a local KConfig removal
     * with no KAuth call.
     */
    Q_INVOKABLE void removeOrphanedRecord(const QString &id);

    /**
     * Path-based removal for a Pair or either-half Partial definition
     * discovered only by scanning the unit tree - no Store record exists
     * for it at all, so there is no id to key off. Authentication is
     * derived fresh from the validated marker, exactly like deleteShare().
     */
    Q_INVOKABLE void removeOrphanByPath(const QString &mountPoint);

    /** The smb:// display form of a //host/share UNC
     *  (ShareAddress::displayUrl()). Here because ShareForm reaches C++ only
     *  through the actions object its host injects, which keeps the form
     *  host-agnostic. */
    Q_INVOKABLE QString displayUrl(const QString &unc) const;

    /** The user an smb:// address names, or empty
     *  (ShareAddress::userInShareInput()): lets the Add form fill an empty
     *  Username from the address as it is typed. */
    Q_INVOKABLE QString userInShareInput(const QString &text) const;

    /** The local path a folder picked in the mount point's Browse dialog
     *  names, or empty for anything that is not a local folder
     *  (Session::localPathFromUrl()). */
    Q_INVOKABLE QString localPathFromUrl(const QUrl &url) const;

    /** Why the Share field's text cannot be submitted, or empty
     *  (ShareAddress::shareInputProblem()). Cheap enough for a binding. */
    Q_INVOKABLE QString shareInputProblem(const QString &input, const QString &username) const;

    /** Why the Mount point field's text cannot be a mount point, or empty
     *  (Session::mountPointProblem()). Lexical only, so a binding can call it
     *  on every keystroke; checkMountPoint() covers the rest. */
    Q_INVOKABLE QString mountPointProblem(const QString &raw) const;

    /**
     * Starts the checks that touch the system - unit files, the folder
     * itself - for `raw`, off the GUI thread; the answer arrives as
     * mountPointChecked() carrying the same `raw`.
     *
     * One check runs at a time, and at most one waits behind it: a newer
     * request replaces the waiting one. Typing quickly therefore costs one
     * worker and no queue, and a walk that hangs on a stale network home
     * holds only its own thread, on a pool of its own that addShare() and
     * deleteShare() never wait for. A check may be answered after the text
     * has changed; the receiver compares `raw` and drops what is stale.
     */
    Q_INVOKABLE void checkMountPoint(const QString &raw);

    /** A folder picked in the Share browser, as Share field text:
     *  { input, user, error } (ShareAddress::browsedShareInput()). */
    Q_INVOKABLE QVariantMap browsedShareInput(const QUrl &picked) const;

    /** Where the Share browser should open for the field's current text
     *  (ShareAddress::browseStartUrl()). */
    Q_INVOKABLE QUrl browseStartUrl(const QString &shareInput) const;

    /**
     * The password-service key the Share field's text belongs to, as a
     * string: the server and the first path component, or empty when the
     * text is not a usable share address. Two addresses with the same key
     * would be given the same stored login, so the form compares them to
     * decide whether an imported credential still belongs to the share.
     */
    Q_INVOKABLE QString lookupTargetOf(const QString &shareInput) const;

Q_SIGNALS:
    void started(const QString &id, const QString &kind);
    void finished(const QString &id, const QString &kind, bool success, const QString &message);

    /** The answer to checkMountPoint(): empty `problem` means nothing was
     *  found. `raw` is the text that was checked, not the text now. */
    void mountPointChecked(const QString &raw, const QString &problem);

private:
    void startMountPointCheck(const QString &raw);

    QString m_homeDir;  ///< the caller's passwd home: what the helper authorizes against
    uid_t m_uid;
    QThreadPool m_checkPool;
    bool m_checkRunning = false;
    bool m_checkPending = false;
    QString m_pendingRaw;
};

} // namespace Session
