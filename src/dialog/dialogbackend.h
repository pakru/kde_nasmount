/*
 * DialogBackend — the service menu window's C++ side: everything the shared
 * QML form cannot know on its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replaces the old QWidgets MountDialog. The form itself is now
 * ShareForm.qml, shared verbatim with the KCM, so what is left here is only
 * the service-menu-specific context: the UNC the invocation was for, whether
 * that share is already saved (in which case the window offers removal
 * instead of an add form), the outcome of the asynchronous action, and the
 * credential autofill lookup — which lives here, not in the form, because the
 * form is host-agnostic and the KCM has no smb:// URL to look anything up
 * for.
 *
 * Unprivileged, like the dialog it replaces. Anything it decides is for the
 * user's benefit only — the KAuth helper re-checks everything.
 */

#pragma once

// Full definition, not a forward declaration: Session::MountActions is
// exposed to QML as a pointer Q_PROPERTY, and Qt's metatype system requires
// the pointee to be complete.
#include "mountactions.h"

#include <QObject>
#include <QString>

namespace Dialog::CredentialLookup
{
class Controller;
}

class DialogBackend : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString unc READ unc CONSTANT)
    Q_PROPERTY(QString suggestedUser READ suggestedUser CONSTANT)
    Q_PROPERTY(QString suggestedPath READ suggestedPath CONSTANT)
    /** Empty unless this share is already saved; then the window shows the
     *  existing-share note and a Remove button rather than the add form. */
    Q_PROPERTY(QString existingId READ existingId CONSTANT)
    Q_PROPERTY(QString existingMountPoint READ existingMountPoint CONSTANT)
    Q_PROPERTY(QString existingStateText READ existingStateText CONSTANT)
    Q_PROPERTY(Session::MountActions *actions READ actions CONSTANT)

public:
    /**
     * `urlUser` is the username the smb:// URL actually carried, and
     * `loginUser` the local account name to fall back to. They are separate
     * arguments on purpose (autofill plan §3.1): the first is evidence about
     * which SMB account is meant and constrains the credential lookup, the
     * second is only what the field is pre-filled with and must never
     * constrain it.
     */
    DialogBackend(const QString &unc, const QString &urlUser, const QString &loginUser,
                  QObject *parent = nullptr);

    QString unc() const { return m_unc; }
    QString suggestedUser() const { return m_suggestedUser; }
    QString suggestedPath() const { return m_suggestedPath; }
    QString existingId() const { return m_existingId; }
    QString existingMountPoint() const { return m_existingMountPoint; }
    QString existingStateText() const { return m_existingStateText; }
    Session::MountActions *actions() const { return m_actions; }

    Q_INVOKABLE void removeExisting();

    /**
     * Starts the one credential lookup for this window (autofill plan §5).
     *
     * Called from QML once the window exists, never from the constructor:
     * a result delivered before QML has connected to credentialSuggestion()
     * would be lost, and the window handle this passes to KDE does not exist
     * that early either. Does nothing for an already-saved share — that view
     * has no form to fill — and nothing on a second call.
     */
    Q_INVOKABLE void startCredentialLookup();

    /**
     * Abandons any in-flight lookup. Called when the user submits, edits a
     * credential field, or closes the window: after this, a result that was
     * already on its way can no longer be applied.
     */
    Q_INVOKABLE void cancelCredentialLookup();

Q_SIGNALS:
    /**
     * One eligible credential, once, for the host to hand to the form.
     *
     * The password is a signal argument and never a property (plan §4.2):
     * a property would keep it readable from QML for the window's lifetime
     * and expose it to anything that enumerates the backend's properties.
     */
    void credentialSuggestion(const QString &username, const QString &domain,
                              const QString &password);

private:
    QString m_unc;
    QString m_urlUser;
    QString m_suggestedUser;
    QString m_suggestedPath;
    QString m_existingId;
    QString m_existingMountPoint;
    QString m_existingStateText;
    Session::MountActions *m_actions = nullptr;
    Dialog::CredentialLookup::Controller *m_lookup = nullptr;
};
