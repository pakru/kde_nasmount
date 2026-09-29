/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dialogbackend.h"
#include "credentiallookup.h"
#include "mountactions.h"
#include "shareaddress.h"
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
    // A saved share opens the removal view: a lookup would be a wallet
    // prompt with nowhere to put the answer.
    if (!m_existingId.isEmpty() || m_lookup) {
        return;
    }

    const QUrl target = Session::ShareAddress::authLookupTarget(m_unc);
    if (!target.isValid()) {
        Session::CredentialLookup::debugReport(QStringLiteral("this share has no lookup target"));
        return;
    }

    m_lookup = new Session::CredentialLookup::Controller(this);
    connect(m_lookup, &Session::CredentialLookup::Controller::candidateReady, this,
            &DialogBackend::credentialSuggestion);
    // missed() reaches nothing user-visible: finding nothing is the ordinary
    // case, and an error box would turn a silent convenience into an
    // interruption. The opt-in diagnostic above is the exception,
    // because "nothing happened and nothing said why" cannot be debugged.
    connect(m_lookup, &Session::CredentialLookup::Controller::missed, this,
            [](const QString &reason) { Session::CredentialLookup::debugReport(QStringLiteral("no credential applied - ") + reason); });
    connect(m_lookup, &Session::CredentialLookup::Controller::candidateReady, this,
            []() { Session::CredentialLookup::debugReport(QStringLiteral("a stored credential was applied")); });

    Session::CredentialLookup::Request request;
    request.target = target;
    // Only the URL's own username, never the local login: only the URL is
    // evidence of which SMB account is meant.
    request.username = m_urlUser;
    request.windowId = 0;

    // kpasswdserver's windowId is an X11 XID, and under Wayland winId()
    // returns something else entirely - passing that would name an unrelated
    // window. Under Wayland the prompt appears unparented.
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        const QList<QWindow *> windows = QGuiApplication::topLevelWindows();
        if (!windows.isEmpty()) {
            request.windowId = static_cast<qulonglong>(windows.first()->winId());
        }
    }
    // userTime stays 0, its documented "unknown" value: the supported source
    // is KWindowSystem, and a framework dependency across both packages and
    // both workflows is not worth a focus hint on a rare prompt.

    m_lookup->start(request);
}

void DialogBackend::cancelCredentialLookup()
{
    if (m_lookup) {
        m_lookup->cancel();
    }
}
