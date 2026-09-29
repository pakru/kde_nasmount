/*
 * mountpointcheck - what the Add form can tell the user about a mount point
 * before anything is sent to the privileged helper.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything here is a courtesy. The helper re-validates the path and walks
 * it on descriptors under the root lock, and that walk is what decides; these
 * checks exist so a certain failure is explained while the user is still
 * typing rather than after an authentication prompt. So each check answers a
 * question the helper will also ask, with the helper's own functions where
 * they exist, and answers "nothing to report" whenever it cannot tell.
 *
 * The checks come in two speeds, and the split is the point of this file.
 * mountPointProblem() is lexical: no process, no filesystem access, cheap
 * enough to sit in a binding that re-evaluates on every keystroke. Everything
 * else - naming the unit files with systemd-escape, looking for them in
 * /etc/systemd/system, walking the folder - can block, and belongs on a worker
 * thread (MountActions::checkMountPoint()). The folder walk is the one that
 * must also never trigger an automount: the path may sit on another share's
 * automount point, and looking at it from System Settings would mount that
 * share and stall until it did.
 */

#pragma once

#include "unitspec.h"

#include <QString>

#include <functional>

#include <sys/types.h>

namespace Session
{

/** Whether a unit file exists at this path; injected so a test needs no
 *  /etc/systemd/system. */
using UnitFileExists = std::function<bool(const QString &)>;

/**
 * The mount point exactly as addShare() sends it: trimmed, a leading `~` or
 * `~/` replaced by `homeDir`, then QDir::cleanPath().
 *
 * `~` is expanded here because a person typing a path in a terminal's habit
 * means their home folder, and the helper would refuse the literal text as a
 * relative path - so expansion gives a meaning only to input that could never
 * have been saved. It is the caller's own passwd home, the same directory the
 * helper authorizes against, so an expanded path can only ever land where the
 * unexpanded absolute one could. `~user` is left as typed: another account's
 * home is not this caller's to name.
 *
 * The one function for both the live check and the submit, so they cannot
 * disagree about which path is meant.
 */
QString canonicalMountPoint(const QString &raw, const QString &homeDir);

/**
 * Why this Add-form text cannot be a mount point, worded for display, or empty
 * when it can. Lexical only (see the file comment); the helper's own
 * UnitSpec::validateMountpoint() applied to canonicalMountPoint(), so what
 * passes here is what the helper's first check accepts.
 */
QString mountPointProblem(const QString &raw, const QString &homeDir);

/**
 * Another definition already at, or above, this mount point: a nasmount unit
 * for the same path (the helper refuses to overwrite it), or for a folder the
 * path sits inside (a mount point that would be walked into, mounting it).
 * Empty when there is none, and when it cannot tell - a machine without
 * systemd-escape is not a refusal.
 *
 * Blocks: it runs a process per path examined. Worker threads only.
 */
QString unitCollisionProblem(const UnitSpec::MountpointPlan &plan, const UnitFileExists &unitFileExists);

/**
 * The whole background check for one Add-form text: nothing when the text is
 * not even a lexically valid mount point (mountPointProblem() reports that),
 * else a unit collision, else UnitSpec::previewMountpointProblem() for `uid`.
 * `unitFileExists` defaults to looking at the real file. Blocks; worker
 * threads only.
 */
QString mountPointFsProblem(const QString &raw, const QString &homeDir, uid_t uid,
                            const UnitFileExists &unitFileExists = UnitFileExists());

} // namespace Session
