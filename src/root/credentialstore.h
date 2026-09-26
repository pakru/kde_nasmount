/*
 * credentialstore — the root-owned mount.cifs credentials file, through one
 * safe writer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Credential files live below /etc/nasmount, one per share, named by its
 * stable id. UnitSpec::credentialDirectory()/credentialPathFor()
 * in kde_nasmount-core compute the *string* embedded in a generated unit's
 * Options=; this is the privileged counterpart that actually creates,
 * durably replaces, and removes the file those strings point at, through
 * the same descriptor-verified primitives every other root-owned artifact
 * uses.
 */

#pragma once

#include "unitvalue.h"

#include <QString>

namespace Root::CredentialStore
{

/**
 * Durably writes (or replaces) the credentials file for `id`:
 * `username=`, and — when non-empty — `password=`/`domain=` lines, matching
 * what `mount.cifs(8)` expects. Validates `id` (UnitValue::isValidShareId())
 * and every field's control characters and byte limit
 * (UnitSpec::MaxCredentialFieldBytes) before forming any filename or writing
 * any byte — the same bound the helper checks early, enforced again here so
 * no call path can skip it. Never truncates a working credential in place: a
 * failure here leaves the previous file, if any, untouched (durablefs's
 * durableReplace() contract).
 */
bool write(const QString &id, const QString &username, const QString &domain,
          const QString &password, QString *error);

/**
 * Removes the credentials file for `id`. `allowMissing` distinguishes
 * an idempotent, expected absence from a reported failure — a caller
 * that expects the file to exist should pass false.
 */
bool remove(const QString &id, bool allowMissing, QString *error);

/**
 * Validates that the credentials file for `id` exists and is healthy
 * — a regular, root-owned, mode-0600 file reached through the verified
 * credential-directory descriptor, within the size bound — without reading
 * or returning its content. Used by arming/boot to confirm health before
 * starting a trigger; a missing, symlinked, or wrong-mode
 * file must leave the share unarmed, never silently skipped.
 */
bool healthy(const QString &id, QString *error);

/**
 * Asserts no credential file exists for `id` — used by guest operations,
 * which must never create or delete a credential based only on caller input:
 * an empty username must not become a way to delete someone's credential.
 * Returns false (with `error` empty) if a credential file unexpectedly
 * exists; `error` is set only for an actual I/O failure while checking.
 */
bool assertAbsent(const QString &id, QString *error);

} // namespace Root::CredentialStore
