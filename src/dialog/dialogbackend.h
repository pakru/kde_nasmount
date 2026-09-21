/*
 * DialogBackend — the service menu window's C++ side: everything the shared
 * QML form cannot know on its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replaces the old QWidgets MountDialog. The form itself is now
 * ShareForm.qml, shared verbatim with the KCM, so what is left here is only
 * the service-menu-specific context: the UNC, whether that share is already
 * saved, the outcome of the asynchronous action, and the credential lookup —
 * which lives here because the form is host-agnostic and the KCM has no
 * smb:// URL to look anything up for.
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
    /** `urlUser` is what the smb:// URL carried, `loginUser` the local
     *  fallback. Separate on purpose (plan §3.1): only the first is evidence
     *  of which SMB account is meant, so only it constrains the lookup. */
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

    /** Starts the one lookup for this window (plan §5). Called from QML once
     *  the window exists, never from the constructor, or a fast result would
     *  arrive before QML connected. Does nothing for a saved share. */
    Q_INVOKABLE void startCredentialLookup();

    /** Abandons any in-flight lookup, so a result already on its way can no
     *  longer be applied. Called on submit, on a credential edit, and on
     *  close. */
    Q_INVOKABLE void cancelCredentialLookup();

Q_SIGNALS:
    /** One eligible credential, once. The password is a signal argument and
     *  never a property (plan §4.2), which would keep it readable from QML
     *  for the window's lifetime. */
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
