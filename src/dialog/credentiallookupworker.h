/*
 * credentiallookupworker — the child half of credential autofill, and the
 * only code here that calls KDE's password service (autofill plan §4.1).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * It is a second invocation of nasmount-dialog rather than an installed
 * helper, which keeps the installed file set — and so the cleanup manifest
 * and both package payload lists — unchanged. Its restrictions (exact
 * argument, pipes only) stop an accidental invocation dumping a credential
 * into a terminal; they are not a boundary against the user themselves.
 */

#pragma once

namespace Dialog::CredentialLookupWorker
{

/** True when `argv` is *exactly* the lookup flag and nothing else. Checked
 *  before any GUI machinery exists, so this process never constructs a
 *  QApplication or opens a display connection. */
bool isInternalInvocation(int argc, char **argv);

/**
 * Reads one request from stdin, asks the password service, writes one reply
 * to stdout, exits. Refuses before reading anything unless both are pipes, so
 * a credential cannot land in a terminal or a file by accident. Every
 * non-candidate outcome is an ordinary miss to the parent.
 */
int run(int argc, char **argv);

} // namespace Dialog::CredentialLookupWorker
