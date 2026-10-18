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

// Writes `content` and requires the validator to refuse it for `reason`: a
// rejection for some other reason would pass a bare "was rejected" check while
// the rule under test never ran.
void expectRejected(const QString &path, const QString &name, const QString &content,
                    const QString &reason)
{
    QStringList targets;
    QString error;
    check(name + QStringLiteral(" (written)"), writeManifest(path, content));
    const bool accepted = Session::validateInstallManifest(path, &targets, &error);
    check(name, !accepted && error.contains(reason),
          accepted ? QStringLiteral("accepted") : error);
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

    // Translation catalogs: the one variable-length family. `valid` is the
    // 18-target Debian/source layout, `nativeDeb` adds the two Debian
    // documents, `fedora` adds the preset and moves the licence.
    const QString ruCatalog = QStringLiteral("/usr/share/locale/ru/LC_MESSAGES/kcm_nasmount.mo\n");

    check(QStringLiteral("write Debian manifest with a ru catalog"),
          writeManifest(path, valid + ruCatalog));
    error.clear();
    check(QStringLiteral("Debian layout with a ru catalog accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("a ru catalog adds one target"), targets.size() == 19,
          QString::number(targets.size()));
    check(QStringLiteral("the catalog path is returned verbatim"),
          targets.contains(QStringLiteral("/usr/share/locale/ru/LC_MESSAGES/kcm_nasmount.mo")));

    check(QStringLiteral("write Debian manifest with three catalogs"),
          writeManifest(path, valid + ruCatalog
                            + QStringLiteral("/usr/share/locale/pt_BR/LC_MESSAGES/kcm_nasmount.mo\n"
                                             "/usr/share/locale/sr@latin/LC_MESSAGES/kcm_nasmount.mo\n")));
    error.clear();
    check(QStringLiteral("ru, pt_BR and sr@latin catalogs accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("three catalogs add three targets"), targets.size() == 21,
          QString::number(targets.size()));

    check(QStringLiteral("write Fedora manifest with a ru catalog"),
          writeManifest(path, fedora + ruCatalog));
    error.clear();
    check(QStringLiteral("Fedora layout with a ru catalog accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("Fedora plus a catalog is 20 targets"), targets.size() == 20,
          QString::number(targets.size()));

    // The Debian document pair and the catalogs are both extra to the fixed
    // set; their arithmetic must add up rather than cancel.
    check(QStringLiteral("write native Debian manifest with a ru catalog"),
          writeManifest(path, nativeDeb + ruCatalog));
    error.clear();
    check(QStringLiteral("native Debian layout with a ru catalog accepted"),
          Session::validateInstallManifest(path, &targets, &error), error);
    check(QStringLiteral("native Debian plus a catalog is 21 targets"), targets.size() == 21,
          QString::number(targets.size()));

    // Every shape the language directory may take, each on its own, so one
    // accepted spelling cannot hide another that was refused.
    const QStringList acceptedLanguages = {
        QStringLiteral("ru"),          QStringLiteral("ast"),         QStringLiteral("pt_BR"),
        QStringLiteral("zh_CN"),       QStringLiteral("sr@latin"),    QStringLiteral("ca@valencia"),
        QStringLiteral("sr_RS@latin"),
    };
    for (const QString &language : acceptedLanguages) {
        const QString line = QStringLiteral("/usr/share/locale/%1/LC_MESSAGES/kcm_nasmount.mo\n").arg(language);
        check(QStringLiteral("write manifest with the %1 catalog").arg(language),
              writeManifest(path, valid + line));
        error.clear();
        check(QStringLiteral("%1 catalog accepted").arg(language),
              Session::validateInstallManifest(path, &targets, &error) && targets.size() == 19, error);
    }

    // CMake writes a blank line where there are no catalogs, between the
    // licence and the preset: zero catalogs must stay valid, blank line or not.
    check(QStringLiteral("write manifest with a blank line and no catalog"),
          writeManifest(path, valid + QStringLiteral("\n\n")));
    error.clear();
    check(QStringLiteral("a manifest with no catalog and blank lines is accepted"),
          Session::validateInstallManifest(path, &targets, &error) && targets.size() == 18, error);

    check(QStringLiteral("write manifest with the catalog first"),
          writeManifest(path, ruCatalog + valid));
    error.clear();
    check(QStringLiteral("catalog position in the manifest does not matter"),
          Session::validateInstallManifest(path, &targets, &error) && targets.size() == 19, error);

    // The uninstall deletes what the manifest lists, so each refusal below is a
    // path that must never reach `rm`.
    const QString unexpected = QStringLiteral("unexpected path");
    expectRejected(path, QStringLiteral("a .. language directory rejected"),
                   valid + QStringLiteral("/usr/share/locale/../LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a traversal inside the language directory rejected"),
                   valid + QStringLiteral("/usr/share/locale/ru/../ru/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("another project's catalog rejected"),
                   valid + QStringLiteral("/usr/share/locale/ru/LC_MESSAGES/kio6.mo\n"), unexpected);
    expectRejected(path, QStringLiteral("a .mo.bak suffix rejected"),
                   valid + QStringLiteral("/usr/share/locale/ru/LC_MESSAGES/kcm_nasmount.mo.bak\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a catalog outside LC_MESSAGES rejected"),
                   valid + QStringLiteral("/usr/share/locale/ru/kcm_nasmount.mo\n"), unexpected);
    expectRejected(path, QStringLiteral("a hyphenated language directory rejected"),
                   valid + QStringLiteral("/usr/share/locale/x-test/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("an upper-case language rejected"),
                   valid + QStringLiteral("/usr/share/locale/RU/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a one-letter language rejected"),
                   valid + QStringLiteral("/usr/share/locale/r/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a four-letter language rejected"),
                   valid + QStringLiteral("/usr/share/locale/russ/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a lower-case territory rejected"),
                   valid + QStringLiteral("/usr/share/locale/pt_br/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("an upper-case variant rejected"),
                   valid + QStringLiteral("/usr/share/locale/sr@Latin/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("an empty variant rejected"),
                   valid + QStringLiteral("/usr/share/locale/sr@/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("an empty language directory rejected"),
                   valid + QStringLiteral("/usr/share/locale//LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a catalog under /usr/local rejected"),
                   valid + QStringLiteral("/usr/local/share/locale/ru/LC_MESSAGES/kcm_nasmount.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a catalog in a different case rejected"),
                   valid + QStringLiteral("/usr/share/locale/ru/LC_MESSAGES/KCM_NASMOUNT.mo\n"),
                   unexpected);
    expectRejected(path, QStringLiteral("a listed catalog twice rejected"),
                   valid + ruCatalog + ruCatalog, QStringLiteral("duplicate"));
    expectRejected(path, QStringLiteral("a catalog alone is an incomplete manifest"),
                   ruCatalog, QStringLiteral("incomplete"));
    // A catalog counts toward the total but never toward a component: swapping
    // a fixed component for one must not balance the exact-size check.
    QString withoutDialog = valid;
    withoutDialog.remove(QStringLiteral("/usr/bin/nasmount-dialog\n"));
    expectRejected(path, QStringLiteral("a catalog cannot stand in for a missing component"),
                   withoutDialog + ruCatalog, QStringLiteral("incomplete"));

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
