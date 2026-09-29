/*
 * nasmount-package-guard - native package pre-removal safety check.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Maintainer scripts call this executable before program files disappear.
 * It is intentionally read-only, accepts no path or state from the caller,
 * and links only the unprivileged core library. Authenticated cleanup remains
 * the job of nasmount-cleanup and the KAuth helper.
 */

#include "nasmountversion.h"
#include "packagestate.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nasmount-package-guard"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(Nasmount::Version));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Prove nasmount package removal is safe"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    const PackageState::Result state = PackageState::classifyHost();
    if (state.classification == PackageState::Classification::Empty) {
        QTextStream(stdout) << state.detail << '\n';
        return 0;
    }

    QTextStream err(stderr);
    err << "ERROR: refusing to remove nasmount: " << state.detail << '\n';
    if (state.classification == PackageState::Classification::Managed) {
        err << "Run nasmount-uninstall as the desktop user who owns the managed shares.\n";
        return 1;
    }
    err << "The state is unsafe or could not be classified; repair it before retrying removal.\n";
    return 2;
}
