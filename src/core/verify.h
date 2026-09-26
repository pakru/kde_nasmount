/*
 * verify — the axis model: inspects what is actually on disk and
 * in the kernel, never what a caller claims.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything here is read-only and needs no privilege beyond what the normal
 * permissions on /etc/systemd/system, /proc/self/mountinfo and statx(2) already
 * grant — unit files are world-readable, which is what lets the KCM and dialog
 * list mounts without a KAuth round trip. The helper calls the same functions
 * again under its lock immediately before acting; a frontend-computed axis
 * value is never itself an authorisation input.
 */

#pragma once

#include "unitvalue.h"

#include <QList>
#include <QString>

#include <cstdint>
#include <sys/types.h>

namespace Verify
{

enum class Definition { None, Pair, Partial, Tampered, NotOurs };
enum class AutomountState { Inactive, Active, Indeterminate };
enum class MountState { Absent, Present, Indeterminate };
enum class VerificationState { NotApplicable, Match, Mismatch, Indeterminate };

/**
 * The full result of inspecting a definition: not just "does it exist" but
 * everything a caller needs to act on it without re-deriving anything from
 * Store or a filename. Every field below `state` is populated
 * only from a *validated* marker — a full agreeing pair for `Pair`, or the
 * single surviving half's own marker for an owned `Partial` (so removal can
 * use its existing id without trusting a caller-supplied one).
 * They are left at their defaults for `None`, `Tampered` and `NotOurs`,
 * whose fields cannot be trusted.
 */
struct DefinitionCheck {
    Definition state = Definition::None;
    QString id;    ///< the stable share id; valid for Pair and owned Partial
    uid_t ownerUid = 0;
    gid_t ownerGid = 0;
    UnitValue::AuthenticationKind authentication = UnitValue::AuthenticationKind::Credentials;
    UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite;
    QString canonicalMountPoint; ///< the Where= this was checked against
    QString mountUnitName;       ///< "<unitName>.mount"
    QString automountUnitName;   ///< "<unitName>.automount"
    QString what;                ///< the .mount unit's own What=, valid when state == Pair
    QString reason;               ///< machine-readable, stable across releases (e.g. "marker-mismatch")
    QString detail;               ///< human-readable, for NeedsAttention / errors
};

/**
 * Inspects the (up to) two unit files for `paths`, applying every safety and
 * marker rule: regular file, root-owned, not group/world-writable, not
 * a symlink (opened O_NOFOLLOW so this is race-free), no drop-in directory, a
 * complete marker block agreeing on every field between both halves, and
 * the restricted-template body (Where=, Type=, Options=) validated against
 * that marker and `canonicalMountPoint`.
 */
DefinitionCheck inspectDefinition(const UnitValue::UnitPaths &paths, uid_t expectedUid,
                                  const QString &canonicalMountPoint);

/** One managed unit half found while scanning, already marker-validated and
 *  owned by the scanned uid. Exposed for testing pairScannedHalves() without
 *  root; production code only reaches this via enumerateOwnedUnits(). */
struct ScannedHalf {
    QString baseName; ///< escaped unit name, no .mount/.automount suffix
    bool isMount = false; ///< true for a .mount half, false for .automount
    UnitValue::Marker marker;
    QString where; ///< this half's own Where=, decoded
    QString what;  ///< .mount only; empty for .automount
};

/**
 * One base name's worth of owned unit(s), after pairing. `state` is one of
 * `Pair`, `Partial` or `Tampered` — enumeration only ever reports units it
 * has already proven this uid owns, so `None` and `NotOurs` never appear
 * here. A `Tampered` entry's other fields are not populated: internal
 * disagreement (the two halves of one base name disagree) or a collision
 * (two different base names claim the same id or the same mount point) both
 * mean the entry cannot be trusted, and it must be surfaced, never silently
 * resolved by picking one.
 */
struct OwnedUnit {
    QString mountPoint;
    QString unitName; ///< escaped base name
    QString id;
    uid_t ownerUid = 0; ///< the marker's recorded owner -- cleared to 0 for Tampered, like every other field
    gid_t ownerGid = 0;
    UnitValue::AuthenticationKind authentication = UnitValue::AuthenticationKind::Credentials;
    /** The marker's recorded access mode -- reset to ReadWrite along with the
     *  other marker-derived fields when this entry is demoted to Tampered, so
     *  an untrusted mode is never surfaced. */
    UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite;
    Definition state = Definition::None;
    QString detail;
    /** The `.mount` half's own What=, when a `.mount` half is part of this
     *  entry (Pair, or Partial with the surviving half being `.mount`). The
     *  unprivileged model and checked mutations use it for runtime
     *  correlation. Empty when only the `.automount` half survives, or for
     *  Tampered, where nothing here can be trusted. */
    QString what;
};

/**
 * Pure grouping, pairing and collision logic over already-scanned halves for
 * one uid — no filesystem access, so this is fully unit-testable. Groups by
 * `baseName` into `Pair` (both halves present and agreeing) or `Partial`
 * (exactly one half); a pair whose halves disagree on the marker or on
 * `where` becomes `Tampered`. A second pass then flags any two *different*
 * base names that claim the same `id` or the same canonical mount point as
 * `Tampered` — an invariant violation enumeration must surface, not resolve.
 */
QList<OwnedUnit> pairScannedHalves(const QList<ScannedHalf> &halves);

/**
 * Scans /etc/systemd/system for every *.mount and *.automount carrying this
 * tool's complete marker with owner-uid == `uid`, and returns one entry per
 * base name via pairScannedHalves() — including either-half `Partial` pairs,
 * which a .mount-only scan would miss entirely.
 *
 * Used by the KCM model and the privileged inventory, which must enumerate
 * every unit this uid owns — not the Store snapshot — so deleting a Store
 * record cannot hide an armed unit. Unprivileged: unit files are
 * world-readable.
 */
QList<OwnedUnit> enumerateOwnedUnits(uid_t uid);

/**
 * Like enumerateOwnedUnits(), but across every owner — used only by
 * nasmount-boot, which has no single caller uid to scope to
 * and must arm every share on the host, not one user's. Same
 * pairing/collision rules from pairScannedHalves(), applied globally, which
 * for the id/mount-point collision check is if anything more correct than
 * the uid-scoped caller doing it once per user.
 */
QList<OwnedUnit> enumerateManagedUnits();

/** One line of /proc/self/mountinfo, escapes already decoded. */
struct MountEntry {
    QString mountPoint;
    QString filesystemType;
    QString mountSource; ///< the "What" — e.g. //host/share
};

/** Exposed for testing; production code should call currentMounts(). */
QList<MountEntry> parseMountinfo(const QString &content);

/** Verdict for whatever currently occupies `canonicalMountPoint`. */
struct MountClassification {
    MountState mount;
    VerificationState verification;
};

/**
 * Pure classification of already-parsed mountinfo entries against one
 * mount point: our own CIFS mount (`Present`, `Match` or
 * `Mismatch` depending on `What=`), the ordinary idle-autofs resting state
 * (`Absent`, `NotApplicable`), some other filesystem occupying the path
 * entirely (a present foreign mount — `Present`, `Mismatch`, never
 * `Absent`), or nothing at all (`Absent`, `NotApplicable`). Exposed for
 * testing without a real mount; production code reaches this only through
 * inspectRuntime().
 */
MountClassification classifyMountEntries(const QList<MountEntry> &entries, const QString &canonicalMountPoint,
                                         const QString &expectedWhat);

/**
 * Reads and parses /proc/self/mountinfo for the calling process. Returns
 * false on a read failure — the caller must not mistake that for "no mounts
 * exist"; `*entries` is left unmodified on failure.
 */
bool currentMounts(QList<MountEntry> *entries);

/**
 * Whether an `Active` automount's *specific running instance* is the one
 * this tool started, not merely whether systemd currently reports the unit
 * active. `NotApplicable` when automount is not Active — there is nothing to
 * trust or distrust. A missing, unreadable or mismatched recorded id is
 * `Untrusted`, never silently treated as absent: "the trigger
 * is armed" and "this is the instance we recorded" are different claims, and
 * conflating them is what let an adopted/foreign automount be treated as
 * safe to act on.
 */
enum class ActivationTrust { NotApplicable, Trusted, Untrusted };

struct RuntimeSnapshot {
    AutomountState automount = AutomountState::Indeterminate;
    MountState mount = MountState::Indeterminate;
    VerificationState verification = VerificationState::NotApplicable;
    ActivationTrust activationTrust = ActivationTrust::NotApplicable;
};

/**
 * Combines `systemctl show` (PID 1's bookkeeping — the only way to tell
 * whether the automount is still armed once its trigger has already fired,
 * since the autofs mountinfo entry is superseded by the real one at that
 * point) with /proc/self/mountinfo (the caller's own mount namespace)
 * to compute the Automount, Mount, Verification and ActivationTrust axes.
 *
 * `expectedWhat` is the unit's What= (the UNC path); correlation is
 * best-effort by design — it proves "a CIFS mount of the right
 * share", never "the mount this helper created". A mount actually present at
 * `mountPoint` under any filesystem other than `cifs` or `autofs` is a
 * present foreign mount (`Mismatch`), not `Absent` — autofs
 * alone means "armed but not yet triggered", the ordinary resting state. A
 * failure reading mountinfo, or a disagreement between mountinfo and
 * systemd's own bookkeeping for the `.mount` unit, is `Indeterminate` for
 * both `mount` and `verification` — never silently
 * treated as "nothing mounted".
 */
RuntimeSnapshot inspectRuntime(const QString &unitName, const QString &mountPoint, const QString &expectedWhat);

/**
 * statx(AT_NO_AUTOMOUNT, STATX_MNT_ID_UNIQUE) on `path`. Returns 0 (never a
 * valid id) if the kernel does not support the extended id (pre-6.8) or the
 * call fails for any other reason — callers must treat 0 as "unknown",
 * failing closed exactly like Indeterminate.
 *
 * AT_NO_AUTOMOUNT is what makes this safe to call on an armed automount
 * trigger without mounting it as a side effect of merely looking.
 */
uint64_t uniqueMountId(const QString &path);

/**
 * The automount unique id recorded at `arm` time, under root-owned, world-
 * readable /run/nasmount-ids/ — the one place live-activation identity is
 * kept, because only an automount's unique mount id can prove which instance
 * this tool started. Returns 0
 * if absent/unreadable.
 *
 * Writing/removing the record is root-only and lives in
 * Root::RuntimeFiles::writeAutomountId()/removeAutomountId() instead — this
 * header only ever reads it back, unprivileged.
 */
uint64_t readRecordedAutomountId(const QString &unitName);

} // namespace Verify
