/*
 * nasmount-cleanup — authenticated uninstall coordinator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This remains an unprivileged command-line client: it validates the exact
 * install manifest before asking the helper to purge privileged state, then
 * removes the caller's configuration only after that purge is confirmed.
 *
 * Exit codes are load-bearing, because nasmount-uninstall removes the package
 * afterwards and has to tell the user what actually happened to their shares.
 * A single failure code cannot express that: Root::Operations::purge() deletes
 * shares in a loop and returns early on the first failure, so "the purge
 * failed" is compatible with several shares already being gone; and
 * HelperOutcome::Unknown means the helper may have run and only the
 * acknowledgement was lost. Only HelperResult::rejectedBeforeDispatch proves
 * the helper never ran — ConfirmedFailure alone does not, because it also
 * covers our own helper refusing after it has already removed shares. Hence:
 *
 *   0  success
 *   1  usage or preflight error -- nothing was attempted
 *   2  the helper refused before dispatch -- nothing was removed
 *   3  indeterminate: state may be unchanged, partly removed, or fully
 *      removed, and the caller must not claim otherwise
 */

#include "cleanupvalidation.h"
#include "helperinvoke.h"
#include "nasmountversion.h"
#include "store.h"
#include "userlock.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nasmount-cleanup"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(Nasmount::Version));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Authenticated nasmount uninstall cleanup"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption manifestOption(QStringLiteral("manifest"), QStringLiteral("validated install manifest"),
                                      QStringLiteral("path"));
    parser.addOption(manifestOption);
    QCommandLineOption validateOnlyOption(QStringLiteral("validate-only"),
                                          QStringLiteral("validate the manifest without requesting cleanup"));
    parser.addOption(validateOnlyOption);
    parser.process(app);

    QTextStream err(stderr);
    if (!parser.isSet(manifestOption)) {
        err << "ERROR: --manifest is required\n";
        return 1;
    }

    QString error;
    QStringList targets;
    if (!Session::validateInstallManifest(parser.value(manifestOption), &targets, &error)) {
        err << "ERROR: " << error << '\n';
        return 1;
    }
    if (parser.isSet(validateOnlyOption)) {
        QTextStream(stdout) << "validated " << targets.size() << " installed paths\n";
        return 0;
    }

    auto lock = Session::UserLock::acquire(&error);
    if (!lock) {
        err << "ERROR: " << error << '\n';
        return 1;
    }

    const Session::HelperResult purge = Session::invokeHelperAction(QStringLiteral("purge"), {});
    // Not `outcome == ConfirmedFailure`: that also covers the helper running
    // and refusing, and Root::Operations::purge() deletes shares in a loop and
    // can fail after several are already gone — or after removing everything
    // and then failing its daemon-reload. Only a pre-dispatch rejection proves
    // nothing changed.
    if (purge.rejectedBeforeDispatch) {
        err << "ERROR: privileged purge was refused: " << purge.message << '\n';
        err << "No share, credential, or runtime record was removed.\n";
        return 2;
    }
    if (purge.outcome != Session::HelperOutcome::ConfirmedSuccess) {
        // The helper ran and refused, or the acknowledgement was lost. Either
        // way the purge is not atomic across shares, so saying "nothing was
        // removed" here would be a guess presented as a fact.
        err << "ERROR: privileged purge was not confirmed: " << purge.message << '\n';
        err << "Some shares, credentials, or runtime records may already have been "
               "removed. Re-run this command to reconcile.\n";
        return 3;
    }
    if (!Store::purgeApplicationData(&error)) {
        err << "ERROR: privileged data was purged, but user data cleanup failed: " << error << '\n';
        return 3;
    }

    QTextStream(stdout) << purge.message << "; removed local configuration\n";
    return 0;
}
