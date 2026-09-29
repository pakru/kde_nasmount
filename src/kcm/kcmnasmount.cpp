/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kcmnasmount.h"
#include "credentiallookup.h"
#include "shareaddress.h"

#include <KPluginFactory>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QUrl>
#include <QWindow>

K_PLUGIN_CLASS_WITH_JSON(KcmNasmount, "kcm_nasmount_plugin.json")

KcmNasmount::KcmNasmount(QObject *parent, const KPluginMetaData &metaData)
    : KQuickConfigModule(parent, metaData)
    , m_model(new Session::MountModel(this))
    , m_actions(new Session::MountActions(this))
{
    setButtons(KAbstractConfigModule::NoAdditionalButton);
    // Every action already took effect by the time it reports finished(); the
    // only thing left to do is re-read what actually happened.
    connect(m_actions, &Session::MountActions::finished, this,
           [this](const QString &, const QString &, bool, const QString &) { m_model->refresh(); });
}

KcmNasmount::~KcmNasmount() = default;

Session::MountModel *KcmNasmount::shareModel() const
{
    return m_model;
}

Session::MountActions *KcmNasmount::actions() const
{
    return m_actions;
}

void KcmNasmount::startCredentialLookup(const QString &shareInput, const QString &urlUser)
{
    using namespace Session::CredentialLookup;

    // One lookup at a time, and each is a fresh controller: a controller makes
    // one attempt for one share, and a new pick is a new question.
    cancelCredentialLookup();

    // The address as the form would submit it. A refusal means there is no
    // share to ask about, which is not something to tell the user.
    QString unc;
    QString error;
    if (!Session::ShareAddress::resolveShareInput(shareInput, urlUser, &unc, &error)) {
        debugReport(QStringLiteral("this address has no lookup target"));
        return;
    }
    const QUrl target = Session::ShareAddress::authLookupTarget(unc);
    if (!target.isValid()) {
        debugReport(QStringLiteral("this share has no lookup target"));
        return;
    }

    m_lookup = new Controller(this);
    m_lookup->setProgram(QStringLiteral(NASMOUNT_DIALOG_PROGRAM));
    connect(m_lookup, &Controller::candidateReady, this, &KcmNasmount::credentialSuggestion);
    // Finding nothing is the ordinary case: manual entry was available
    // throughout, so a miss reaches no interface, only the opt-in diagnostic.
    connect(m_lookup, &Controller::missed, this,
            [](const QString &reason) { debugReport(QStringLiteral("no credential applied - ") + reason); });
    connect(m_lookup, &Controller::candidateReady, this,
            []() { debugReport(QStringLiteral("a stored credential was applied")); });

    Request request;
    request.target = target;
    // Only the picked URL's own user, never the local login or anything typed:
    // only the URL is evidence of which SMB account is meant.
    request.username = urlUser;

    // kpasswdserver's windowId is an X11 XID, and under Wayland winId()
    // returns something else entirely - passing that would name an unrelated
    // window. Under Wayland the prompt appears unparented. userTime stays 0,
    // its documented "unknown" value, as in the service menu.
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        QWindow *window = QGuiApplication::focusWindow();
        if (!window) {
            const QList<QWindow *> windows = QGuiApplication::topLevelWindows();
            window = windows.isEmpty() ? nullptr : windows.first();
        }
        if (window) {
            request.windowId = static_cast<qulonglong>(window->winId());
        }
    }

    m_lookup->start(request);
}

void KcmNasmount::cancelCredentialLookup()
{
    if (!m_lookup) {
        return;
    }
    m_lookup->cancel();
    m_lookup->deleteLater();
    m_lookup = nullptr;
}

void KcmNasmount::openMountPoint(const QString &mountPoint)
{
    // Not Qt's own URL opening (the desktop-services class, or its QML
    // counterpart on the Qt object): under Plasma it hands the URL to KIO's
    // OpenUrlJob inside this process, which examines a local path to pick an
    // application. Examining an automount point triggers the mount, so the
    // GUI thread would block until it completed - or, for a share that cannot
    // mount, until the unit's TimeoutSec. The file manager
    // does its own access asynchronously, in its own process. There is
    // deliberately no fallback that would examine the path here.
    //
    // fromLocalFile() percent-encodes spaces, '#' and '?' correctly, which a
    // string concatenation would not.
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("/org/freedesktop/FileManager1"),
                                                       QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("ShowFolders"));
    call << QStringList{QUrl::fromLocalFile(mountPoint).toString()} << QString();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            Q_EMIT openFailed(QStringLiteral("No file manager answered the request to open the folder (%1)")
                                  .arg(reply.error().message()));
        }
    });
}

#include "kcmnasmount.moc"
