/*
 * nasmount-dialog — entry point.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dolphin invokes this with the smb:// URL of the right-clicked folder, via
 * the service menu in servicemenu/nasmount.desktop.in. A saved share is armed
 * at boot by nasmount-boot, which runs as root with no session and no UI —
 * nothing here participates in that.
 *
 * The window is QML so that its form is literally the KCM's form
 * (ShareForm.qml, embedded from src/kcm/ui via dialog.qrc) rather than a
 * second implementation of it. Nothing here is a QWidget of our own -- but
 * the process is still a QApplication, not a QGuiApplication, because the
 * Browse button's QtQuick.Dialogs.FolderDialog only gets KDE's native
 * KFileWidget-backed picker (Places sidebar, bookmarks) when the platform
 * theme plugin can construct one, and that's a QWidget under the hood.
 * Under a bare QGuiApplication it silently falls back to Qt's generic
 * bundled dialog instead.
 */

#include "credentiallookupworker.h"
#include "dialogbackend.h"
#include "nasmountversion.h"
#include "smburl.h"

#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTextStream>

#include <pwd.h>
#include <unistd.h>

namespace
{

/** Startup failures happen before any window exists, and this is launched
 *  from a service menu where stderr is usually invisible — so report them the
 *  only way that always works, and keep the exit code meaningful. */
void reportStartupFailure(const QString &message)
{
    QTextStream(stderr) << message << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    // The private credential-lookup mode is dispatched before anything else
    // (autofill plan §4.1): that invocation is a pipe-to-pipe transport with
    // no user interface at all, so it must not construct a QApplication,
    // connect to a display, or reach the QML engine below. It is deliberately
    // absent from the parser: --help describes the command a user runs, and
    // this is not one.
    if (Dialog::CredentialLookupWorker::isInternalInvocation(argc, argv)) {
        return Dialog::CredentialLookupWorker::run(argc, argv);
    }

    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("nasmount"));
    QGuiApplication::setApplicationName(QStringLiteral("nasmount"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(Nasmount::Version));
    QGuiApplication::setApplicationDisplayName(i18n("Mount as Network Drive"));
    QGuiApplication::setDesktopFileName(QStringLiteral("nasmount"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        i18n("Mounts an SMB share on demand, available again after a reboot."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("url"), i18n("smb:// URL of the share to mount"));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        reportStartupFailure(i18n("Usage: nasmount-dialog smb://host/share"));
        return 2;
    }

    QString unc;
    QString urlUser;
    QString error;
    if (!Dialog::SmbUrl::parse(args.first(), &unc, &urlUser, &error)) {
        reportStartupFailure(error);
        return 2;
    }

    // The two identities stay separate all the way into the backend (autofill
    // plan §3.1). The Linux login is a *guess* — it is what the field is
    // pre-filled with when the URL says nothing — while only a username the
    // URL actually carried is evidence of which SMB account is meant. Merging
    // them here, as this used to, would mean constraining the credential
    // lookup to the local account name and so excluding a saved NAS account
    // that happens to be called something else.
    QString loginUser;
    if (const struct passwd *pw = ::getpwuid(::getuid())) {
        loginUser = QString::fromLocal8Bit(pw->pw_name);
    }

    DialogBackend backend(unc, urlUser, loginUser);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.load(QUrl(QStringLiteral("qrc:/nasmount/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        reportStartupFailure(i18n("Could not load the dialog interface."));
        return 2;
    }
    return app.exec();
}
