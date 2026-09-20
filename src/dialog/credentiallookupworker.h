/*
 * credentiallookupworker — the child half of credential autofill: the only
 * code in this project that calls KDE's password service (autofill plan §4.1).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This runs as a second, short-lived invocation of nasmount-dialog rather
 * than as an installed helper of its own: it needs no privileges and no
 * install path, and compiling it into the existing binary keeps the installed
 * file set — and therefore the cleanup manifest, both package payload lists,
 * and Session::validateInstallManifest() — completely unchanged.
 *
 * It is an internal transport, not a command. Running it by hand would only
 * show a user their own saved password, which their own session already lets
 * them do, so its restrictions (exact argument, pipes only) are there to stop
 * an accident — a stray invocation dumping a credential into a terminal or a
 * file — not to establish a security boundary against the user themselves.
 */

#pragma once

namespace Dialog::CredentialLookupWorker
{

/**
 * True when `argv` is *exactly* the private lookup mode: the flag, alone,
 * with no other option and no positional argument.
 *
 * Checked before any GUI machinery exists, so the lookup process never
 * constructs a QApplication, never opens a display connection, and never
 * loads a QML window.
 */
bool isInternalInvocation(int argc, char **argv);

/**
 * Runs the whole child: reads one request from stdin, asks the password
 * service, writes one reply to stdout, exits.
 *
 * Refuses, before reading anything, unless stdin and stdout are both pipes —
 * a terminal or a regular file as the response destination means this was not
 * started by the parent, and a credential must not land there by accident.
 *
 * Returns the process exit code. A refusal or a failed lookup is not an error
 * the user ever sees: to the parent, every non-candidate outcome is the same
 * ordinary miss, and the form stays exactly as the user left it.
 */
int run(int argc, char **argv);

} // namespace Dialog::CredentialLookupWorker
