/*
 * kcm_nasmount - System Settings module: list, add and remove
 * generated network-mount definitions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An "action module": every change (Add or Delete) takes
 * effect immediately through Session::MountActions rather than being staged
 * behind Apply/OK, so buttons() is NoAdditionalButton. There is no in-place
 * Edit: changing a share means removing it and adding it again.
 *
 * openMountPoint() asks the file manager over D-Bus to show a mount point
 * rather than opening it here: looking at an automount point *is* the mount
 * trigger, and any in-process look would block System Settings until the
 * mount completed or timed out.
 *
 * The credential lookup after the Add dialog's Share Browse runs the same
 * child the service menu does: nasmount-dialog in its private mode, started
 * by absolute path. This module never links KIO or talks to the password
 * service itself; only that child does, and it can be abandoned when it is
 * slow or waiting on a wallet prompt.
 */

#pragma once

#include "mountactions.h"
#include "mountmodel.h"

#include <KQuickConfigModule>

namespace Session::CredentialLookup
{
class Controller;
}

class KcmNasmount : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(Session::MountModel *shareModel READ shareModel CONSTANT)
    Q_PROPERTY(Session::MountActions *actions READ actions CONSTANT)

public:
    KcmNasmount(QObject *parent, const KPluginMetaData &metaData);
    ~KcmNasmount() override;

    Session::MountModel *shareModel() const;
    Session::MountActions *actions() const;

    /** Shows `mountPoint` in the file manager (org.freedesktop.FileManager1
     *  ShowFolders), asynchronously; reports a failure through openFailed().
     *  Never stats the path in this process. */
    Q_INVOKABLE void openMountPoint(const QString &mountPoint);

    /**
     * Asks KDE's password service, through the child process, for a login it
     * already holds for the share the Add form just browsed to, replacing any
     * lookup still running. `shareInput` is the Share field's text and
     * `urlUser` the picked URL's own user - the only evidence of which account
     * is meant, so it alone constrains the answer - or empty. A miss is the
     * ordinary case and says nothing; NASMOUNT_DEBUG_LOOKUP=1 says why.
     */
    Q_INVOKABLE void startCredentialLookup(const QString &shareInput, const QString &urlUser);

    /** Abandons the lookup in flight, if any, so an answer already on its way
     *  can no longer be applied. Safe when there is none. */
    Q_INVOKABLE void cancelCredentialLookup();

Q_SIGNALS:
    void openFailed(const QString &message);

    /** One eligible login, once per lookup. Arguments and never properties,
     *  which would keep the password readable from QML. */
    void credentialSuggestion(const QString &username, const QString &domain, const QString &password);

private:
    Session::MountModel *m_model;
    Session::MountActions *m_actions;
    Session::CredentialLookup::Controller *m_lookup = nullptr;
};
