/*
 * systemdops — privileged systemd command execution for kde_nasmount-root.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every call is bounded (output capped, a timeout that kills a runaway
 * process rather than leaking it) and exit-status checked. Every privileged
 * `systemctl` invocation funnels through here, so there is exactly one place
 * that can drift.
 */

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace Root::SystemdOps
{

/**
 * Runs `program` with `args`, bounded to a bundled timeout and output size.
 * `*output` receives combined stderr+stdout, trimmed. Returns the exit code,
 * or -1 if the process could not be started or timed out (in which case it
 * is killed rather than left running).
 */
using CommandRunner = std::function<int(const QString &program, const QStringList &args, QString *output)>;

/** The production runner: a real QProcess. */
int runCommand(const QString &program, const QStringList &args, QString *output);

/**
 * Overrides the runner every function below dispatches through — for tests
 * only. Not thread-safe to change
 * concurrently with use; set once before any call, e.g. at the top of a
 * test's main(). Pass an empty std::function to restore runCommand().
 */
void setCommandRunner(CommandRunner runner);

bool daemonReload(QString *error);
bool start(const QString &unit, QString *error);
bool stop(const QString &unit, QString *error);

} // namespace Root::SystemdOps
