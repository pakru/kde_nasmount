/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cleanupvalidation.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

namespace
{
int failures = 0;

void check(const QString &name, bool condition, const QString &detail = {})
{
    QTextStream out(stdout);
    out << (condition ? "PASS: " : "FAIL: ") << name;
    if (!detail.isEmpty()) {
        out << " (" << detail << ')';
    }
    out << Qt::endl;
    failures += condition ? 0 : 1;
}

bool writeManifest(const QString &path, const QString &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content.toUtf8()) >= 0;
}

QString commonManifest(const QString &helper, const QString &boot, const QString &guard,
                       const QString &plugin)
{
    return QStringLiteral(
               "/usr/bin/nasmount-dialog\n"
               "/usr/bin/nasmount-cleanup\n"
               "/usr/bin/nasmount-uninstall\n"
               "/usr/bin/nasmount\n")
        + helper + QLatin1Char('\n') + boot + QLatin1Char('\n') + guard + QLatin1Char('\n')
        + plugin + QStringLiteral(
                       "\n/usr/lib/systemd/system/nasmount-boot.service\n"
                       "/usr/share/applications/kcm_nasmount.desktop\n"
                       "/usr/share/applications/nasmount-manager.desktop\n"
                       "/usr/share/polkit-1/actions/io.github.pakru.nasmount.policy\n"
                       "/usr/share/dbus-1/system.d/io.github.pakru.nasmount.conf\n"
                       "/usr/share/dbus-1/system-services/io.github.pakru.nasmount.service\n"
                       "/usr/share/kio/servicemenus/nasmount.desktop\n"
                       "/usr/share/nasmount/cleanup-manifest.txt\n"
                       "/usr/share/nasmount/package-family\n"
                       "/usr/share/doc/nasmount/LICENSE\n");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    check(QStringLiteral("temporary directory"), dir.isValid());
    const QString path = dir.filePath(QStringLiteral("install_manifest.txt"));

    const QString valid = commonManifest(
        QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/kf6/kauth/nasmount-helper"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/nasmount-boot"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/nasmount-package-guard"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount.so"));
    check(QStringLiteral("write valid manifest"), writeManifest(path, valid));
    QStringList targets;
    QString error;
    check(QStringLiteral("complete allowlist accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("all Debian/source targets returned"), targets.size() == 18,
          QString::number(targets.size()));

    const QString nativeDeb = commonManifest(
                                  QStringLiteral("/usr/lib/kf6/kauth/libexec/nasmount-helper"),
                                  QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/nasmount-boot"),
                                  QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/nasmount-package-guard"),
                                  QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount.so"))
        + QStringLiteral("/usr/share/doc/nasmount/changelog.Debian.gz\n"
                         "/usr/share/doc/nasmount/copyright\n");
    check(QStringLiteral("write native Debian manifest"), writeManifest(path, nativeDeb));
    error.clear();
    check(QStringLiteral("Debian package additions accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("all native Debian targets returned"), targets.size() == 20,
          QString::number(targets.size()));

    QString fedora = commonManifest(
        QStringLiteral("/usr/libexec/kf6/kauth/nasmount-helper"),
        QStringLiteral("/usr/lib64/libexec/nasmount-boot"),
        QStringLiteral("/usr/lib64/libexec/nasmount-package-guard"),
        QStringLiteral("/usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount.so"));
    fedora.replace(QStringLiteral("/usr/share/doc/nasmount/LICENSE"),
                   QStringLiteral("/usr/share/licenses/nasmount/LICENSE"));
    fedora += QStringLiteral("/usr/lib/systemd/system-preset/90-nasmount.preset\n");
    check(QStringLiteral("write Fedora manifest"), writeManifest(path, fedora));
    error.clear();
    check(QStringLiteral("complete Fedora layout accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("all Fedora targets returned"), targets.size() == 19,
          QString::number(targets.size()));

    const QString mixed = commonManifest(
        QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/kf6/kauth/nasmount-helper"),
        QStringLiteral("/usr/lib64/libexec/nasmount-boot"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/nasmount-package-guard"),
        QStringLiteral("/usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount.so"));
    check(QStringLiteral("write mixed-layout manifest"), writeManifest(path, mixed));
    error.clear();
    check(QStringLiteral("mixed distribution layout rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    check(QStringLiteral("write malicious manifest"), writeManifest(path, QStringLiteral("/etc/passwd\n")));
    error.clear();
    check(QStringLiteral("unexpected target rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    check(QStringLiteral("write duplicate manifest"),
          writeManifest(path, QStringLiteral("/usr/bin/nasmount-dialog\n/usr/bin/nasmount-dialog\n")));
    error.clear();
    check(QStringLiteral("duplicate target rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    check(QStringLiteral("write incomplete manifest"),
          writeManifest(path, QStringLiteral("/usr/bin/nasmount-cleanup\n")));
    error.clear();
    check(QStringLiteral("incomplete allowlisted manifest rejected"),
          !Session::validateInstallManifest(path, &targets, &error));

    return failures == 0 ? 0 : 1;
}
