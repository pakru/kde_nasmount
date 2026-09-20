/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dialogbackend.h"
#include "credentiallookup.h"
#include "mountactions.h"
#include "smburl.h"
#include "store.h"

#include <QGuiApplication>
#include <QWindow>

DialogBackend::DialogBackend(const QString &unc, const QString &urlUser, const QString &loginUser,
                             QObject *parent)
    : QObject(parent)
    , m_unc(unc)
    , m_urlUser(urlUser)
    , m_suggestedUser(urlUser.isEmpty() ? loginUser : urlUser)
    , m_actions(new Session::MountActions(this))
{
    // Prefer whatever was saved for this share last time; only fall back to a
    // suggested path for a share that is genuinely new.
    for (const Store::Share &s : Store::shares()) {
        if (s.unc == unc) {
            m_existingId = s.id;
            m_existingMountPoint = s.mountPoint;
            m_suggestedUser = s.username;
            break;
        }
    }
    if (!m_existingId.isEmpty()) {
        m_existingStateText = Dialog::SmbUrl::describeState(m_existingMountPoint);
        m_suggestedPath = m_existingMountPoint;
    } else {
        m_suggestedPath = Dialog::SmbUrl::suggestMountpoint(unc);
    }
}

void DialogBackend::removeExisting()
{
    if (!m_existingId.isEmpty()) {
        m_actions->deleteShare(m_existingId);
    }
}

void DialogBackend::startCredentialLookup()
{
    // An already-saved share opens the removal view, which has no credential
    // fields at all; looking anything up for it would be a wallet prompt with
    // nowhere to put the answer (plan §5).
    if (!m_existingId.isEmpty() || m_lookup) {
        return;
    }

    const QUrl target = Dialog::SmbUrl::authLookupTarget(m_unc);
    if (!target.isValid()) {
        return;
    }

    m_lookup = new Dialog::CredentialLookup::Controller(this);
    connect(m_lookup, &Dialog::CredentialLookup::Controller::candidateReady, this,
            &DialogBackend::credentialSuggestion);
    // missed() is deliberately not connected to anything user-visible: a
    // lookup that found nothing is the ordinary case, the form was usable
    // throughout, and an error box here would turn a silent convenience into
    // an interruption (plan §5).

    Dialog::CredentialLookup::Request request;
    request.target = target;
    // Only the URL's own username, never the local login (plan §3.1).
    request.username = m_urlUser;
    request.windowId = 0;

    // Parenting metadata is passed only where it is actually a window handle
    // of the kind KDE expects. kpasswdserver's windowId is an X11 XID; under
    // Wayland winId() returns something else entirely, and handing that over
    // as if it were an XID would be a lie that lands on an unrelated window
    // id (plan §4.1). Under Wayland the prompt simply appears unparented,
    // which is KDE's own behaviour for a caller that cannot provide one.
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        const QList<QWindow *> windows = QGuiApplication::topLevelWindows();
        if (!windows.isEmpty()) {
            request.windowId = static_cast<qulonglong>(windows.first()->winId());
        }
    }
    // userTime stays 0: the supported way to obtain it is KWindowSystem's
    // KUserTimestamp, and a whole framework dependency — in both packages,
    // both container builds and both workflows — is not worth a focus hint on
    // a prompt that usually does not appear at all. 0 is the documented
    // "unknown" value.

    m_lookup->start(request);
}

void DialogBackend::cancelCredentialLookup()
{
    if (m_lookup) {
        m_lookup->cancel();
    }
}
