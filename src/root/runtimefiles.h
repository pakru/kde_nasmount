/*
 * runtimefiles — root-owned runtime records under /run/nasmount-ids: the
 * automount instance id recorded at arm time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This tree is deliberately a top-level root, not a subdirectory of a
 * private 0700 one: a nested subdirectory would inherit the parent's
 * traversal bit, making any exception below it unreachable regardless of the
 * file's own mode. /run/nasmount-ids is created 0755
 * (ArtifactKind::PublicDirectory) holding only 0644
 * ArtifactKind::PublicRecord files, so the "readable by any process"
 * property is structural rather than a per-file exception inside an
 * otherwise-private tree.
 *
 * Writes only; reading the recorded automount id back is unprivileged (any
 * process may read a file under /run/nasmount-ids to compute
 * Verify::ActivationTrust) and stays in kde_nasmount-core as
 * Verify::readRecordedAutomountId(). Everything here is root-only to write:
 * only the privileged helper (and the boot coordinator) ever creates,
 * moves, or removes one of these records.
 */

#pragma once

#include <QString>

#include <cstdint>

namespace Root::RuntimeFiles
{

/**
 * Durably records `id` as the trusted automount instance for `unitName`
 * — the only proof, later, that an active trigger is the one this tool
 * started. Overwrites any previous
 * record for the same unit.
 */
bool writeAutomountId(const QString &unitName, uint64_t id, QString *error);

/** Durably removes a recorded automount id. Missing records are accepted so
 *  recovery can repeat the operation after a crash. */
bool removeAutomountId(const QString &unitName, QString *error);

} // namespace Root::RuntimeFiles
