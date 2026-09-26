/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kcmnasmount.h"

#include <KPluginFactory>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUrl>

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

void KcmNasmount::openMountPoint(const QString &mountPoint)
{
    // Not Qt's own URL opening (the desktop-services class, or its QML
    // counterpart on the Qt object): under Plasma it hands the URL to KIO's
    // OpenUrlJob inside this process, which examines a local path to pick an
    // application. Examining an automount point triggers the mount, so the
    // GUI thread would block until it completed — or, for a share that cannot
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
