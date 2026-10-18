/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cleanupvalidation.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>

namespace Session
{

bool validateInstallManifest(const QString &manifestPath, QStringList *targets, QString *error)
{
    targets->clear();
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read install manifest %1").arg(manifestPath);
        return false;
    }
    if (file.size() > 64 * 1024) {
        *error = QStringLiteral("install manifest is unexpectedly large");
        return false;
    }

    // This list decides what a privileged uninstall deletes, so every
    // alternative names one exact path, except the translation catalogs. They
    // are the one family with a variable number of members (one per shipped
    // language); its alternative admits only this project's catalog name,
    // inside a locale directory spelled ll, ll_CC or ll@variant, and nothing a
    // path could traverse with: the language part cannot contain '/' or '.'.
    // The pattern is spelled again below to count catalogs, and in
    // uninstall.sh's own allowlist. The two here can only disagree towards
    // rejection: a path allowed but not counted fails the exact-size check,
    // and a path counted but not allowed never gets that far.
    static const QRegularExpression allowed(QStringLiteral(
        "^/usr/(?:"
        "bin/nasmount(?:-(?:dialog|cleanup|uninstall))?|"
        "(?:lib/[^/]+/libexec/kf6/kauth|lib/kf6/kauth/libexec|libexec/kf6/kauth)/nasmount-helper|"
        "(?:lib/[^/]+/libexec|lib64/libexec|libexec)/nasmount-(?:boot|package-guard)|"
        "(?:lib/[^/]+|lib64)/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount\\.so|"
        "lib/systemd/system/nasmount-boot\\.service|"
        "lib/systemd/system-preset/90-nasmount\\.preset|"
        "share/applications/(?:kcm_nasmount|nasmount-manager)\\.desktop|"
        "share/polkit-1/actions/io\\.github\\.pakru\\.nasmount\\.policy|"
        "share/dbus-1/system\\.d/io\\.github\\.pakru\\.nasmount\\.conf|"
        "share/dbus-1/system-services/io\\.github\\.pakru\\.nasmount\\.service|"
        "share/kio/servicemenus/nasmount\\.desktop|"
        "share/locale/[a-z]{2,3}(?:_[A-Z]{2})?(?:@[a-z]+)?/LC_MESSAGES/kcm_nasmount\\.mo|"
        "share/nasmount/(?:cleanup-manifest\\.txt|package-family)|"
        "share/(?:doc|licenses)/nasmount/LICENSE|"
        "share/doc/nasmount/(?:changelog\\.Debian\\.gz|copyright)"
        ")$"));

    QSet<QString> seen;
    while (!file.atEnd()) {
        const QByteArray raw = file.readLine();
        if (raw.size() > 4096) {
            *error = QStringLiteral("install manifest contains an overlong line");
            return false;
        }
        const QString path = QString::fromUtf8(raw).trimmed();
        if (path.isEmpty()) {
            continue;
        }
        if (!allowed.match(path).hasMatch()) {
            *error = QStringLiteral("install manifest contains an unexpected path: %1").arg(path);
            return false;
        }
        if (seen.contains(path)) {
            *error = QStringLiteral("install manifest contains a duplicate path: %1").arg(path);
            return false;
        }
        seen.insert(path);
        targets->append(path);
    }
    static const QStringList requiredPatterns = {
        QStringLiteral("^/usr/bin/nasmount-dialog$"),
        QStringLiteral("^/usr/bin/nasmount-cleanup$"),
        QStringLiteral("^/usr/bin/nasmount-uninstall$"),
        QStringLiteral("^/usr/bin/nasmount$"),
        QStringLiteral("^/usr/(?:lib/[^/]+/libexec/kf6/kauth|lib/kf6/kauth/libexec|libexec/kf6/kauth)/nasmount-helper$"),
        QStringLiteral("^/usr/(?:lib/[^/]+/libexec|lib64/libexec|libexec)/nasmount-boot$"),
        QStringLiteral("^/usr/(?:lib/[^/]+/libexec|lib64/libexec|libexec)/nasmount-package-guard$"),
        QStringLiteral("^/usr/(?:lib/[^/]+|lib64)/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount\\.so$"),
        QStringLiteral("^/usr/lib/systemd/system/nasmount-boot\\.service$"),
        QStringLiteral("^/usr/share/applications/kcm_nasmount\\.desktop$"),
        QStringLiteral("^/usr/share/applications/nasmount-manager\\.desktop$"),
        QStringLiteral("^/usr/share/polkit-1/actions/io\\.github\\.pakru\\.nasmount\\.policy$"),
        QStringLiteral("^/usr/share/dbus-1/system\\.d/io\\.github\\.pakru\\.nasmount\\.conf$"),
        QStringLiteral("^/usr/share/dbus-1/system-services/io\\.github\\.pakru\\.nasmount\\.service$"),
        QStringLiteral("^/usr/share/kio/servicemenus/nasmount\\.desktop$"),
        QStringLiteral("^/usr/share/nasmount/cleanup-manifest\\.txt$"),
        QStringLiteral("^/usr/share/nasmount/package-family$"),
        QStringLiteral("^/usr/share/(?:doc|licenses)/nasmount/LICENSE$"),
    };
    // Catalogs are counted apart from requiredPatterns so they can neither
    // satisfy nor disturb it: zero is as valid as ten, because a build with no
    // language yet (or KF_SKIP_PO_PROCESSING) installs none. They still enter
    // the exact-size check below, so a manifest cannot trade a missing
    // component for a catalog. A listed catalog is only text here: dpkg
    // path-exclude and rpm %_install_langs may leave one off the disk, and
    // that is not an error, so nothing in this file looks at the filesystem.
    static const QRegularExpression catalog(QStringLiteral(
        "^/usr/share/locale/[a-z]{2,3}(?:_[A-Z]{2})?(?:@[a-z]+)?/LC_MESSAGES/kcm_nasmount\\.mo$"));
    qsizetype catalogs = 0;
    for (const QString &target : *targets) {
        catalogs += catalog.match(target).hasMatch() ? 1 : 0;
    }
    const bool hasPreset = targets->contains(QStringLiteral("/usr/lib/systemd/system-preset/90-nasmount.preset"));
    const bool hasDebianChangelog = targets->contains(QStringLiteral("/usr/share/doc/nasmount/changelog.Debian.gz"));
    const bool hasDebianCopyright = targets->contains(QStringLiteral("/usr/share/doc/nasmount/copyright"));
    if (hasDebianChangelog != hasDebianCopyright
        || targets->size() != requiredPatterns.size() + (hasPreset ? 1 : 0)
               + (hasDebianChangelog ? 2 : 0) + catalogs) {
        *error = QStringLiteral("install manifest is incomplete");
        return false;
    }
    for (const QString &pattern : requiredPatterns) {
        int matches = 0;
        const QRegularExpression required(pattern);
        for (const QString &target : *targets) {
            matches += required.match(target).hasMatch() ? 1 : 0;
        }
        if (matches != 1) {
            *error = QStringLiteral("install manifest is missing or duplicates an installed component");
            return false;
        }
    }

    const auto containsPattern = [targets](const QString &pattern) {
        const QRegularExpression expression(pattern);
        for (const QString &target : *targets) {
            if (expression.match(target).hasMatch()) {
                return true;
            }
        }
        return false;
    };
    const bool debHelper = containsPattern(QStringLiteral(
        "^/usr/lib/(?:[^/]+/libexec/kf6/kauth|kf6/kauth/libexec)/nasmount-helper$"));
    const bool debBoot = containsPattern(QStringLiteral("^/usr/lib/[^/]+/libexec/nasmount-boot$"));
    const bool debGuard = containsPattern(QStringLiteral("^/usr/lib/[^/]+/libexec/nasmount-package-guard$"));
    const bool debPlugin = containsPattern(QStringLiteral("^/usr/lib/[^/]+/qt6/plugins/.+/kcm_nasmount\\.so$"));
    const bool rpmHelper = targets->contains(QStringLiteral("/usr/libexec/kf6/kauth/nasmount-helper"));
    const bool rpmBoot = targets->contains(QStringLiteral("/usr/lib64/libexec/nasmount-boot"));
    const bool rpmGuard = targets->contains(QStringLiteral("/usr/lib64/libexec/nasmount-package-guard"));
    const bool rpmPlugin = targets->contains(
        QStringLiteral("/usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_nasmount.so"));
    const bool completeDebLayout = debHelper && debBoot && debGuard && debPlugin;
    const bool completeRpmLayout = rpmHelper && rpmBoot && rpmGuard && rpmPlugin;
    const bool hasFedoraLicense = targets->contains(QStringLiteral("/usr/share/licenses/nasmount/LICENSE"));
    if (completeDebLayout == completeRpmLayout || (hasPreset && (!completeRpmLayout || !hasFedoraLicense))
        || (hasDebianChangelog && !completeDebLayout)) {
        *error = QStringLiteral("install manifest mixes or incompletely describes distribution layouts");
        return false;
    }
    return true;
}

} // namespace Session
