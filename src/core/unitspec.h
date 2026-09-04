/*
 * unitspec — mount-point validation and CIFS mount options for nasmount.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The helper runs as root and its caller is untrusted, so every function here
 * that validates something is called from the helper. The dialog calls a few of
 * them too, but only to give the user fast feedback — nothing the dialog checks
 * is relied upon for safety.
 */

#pragma once

#include "unitvalue.h"

#include <QString>
#include <QStringList>

#include <sys/types.h>

namespace UnitSpec
{

/** Per-field and whole-file credential limits (design §8.1), centralised here
 *  so define, arm, and the privileged writer they both funnel into apply
 *  exactly the same bound rather than each picking its own. */
constexpr qsizetype MaxCredentialFieldBytes = 4096;
constexpr qsizetype MaxCredentialFileBytes = 3 * MaxCredentialFieldBytes + 256;

/** Roots below which a mount point is permitted, in addition to the caller's home. */
QStringList staticAllowedRoots();

/**
 * Rejects control characters.
 *
 * A newline in any caller-supplied string would let it inject arbitrary
 * directives into a generated unit or credentials file, so this is a hard
 * failure rather than something we try to escape.
 */
bool hasControlChars(const QString &value);

/**
 * Validates a //host/share[/subdir] UNC path.
 *
 * IPv6 literals are deliberately rejected: mount.cifs wants those in the
 * separate ip= option, and silently building //[::1]/share would produce a unit
 * that fails obscurely at boot.
 */
bool validateUnc(const QString &unc, QString *normalised, QString *error);

/**
 * An authorized mount point, split into the allowed root and the components
 * below it.
 *
 * Keeping these separate is the whole point: authorization and the subsequent
 * filesystem operations must use the *same* pathname. Canonicalizing a path to
 * authorize it and then operating on the original lexical path is a bypass — a
 * caller can point a symlink at an allowed root, pass the check, then replace it
 * with a real directory so the operation lands somewhere else entirely.
 */
struct MountpointPlan {
    QString path;       ///< full lexical path, for Where= and messages
    QString root;       ///< the allowed root this lives under
    QStringList suffix; ///< components strictly below root
};

/**
 * Purely lexical authorization of a mount point.
 *
 * Deliberately does NOT canonicalize: the result must describe the same
 * pathname that openMountpointNoFollow() will walk. Symlinks are handled there,
 * by refusing to traverse them at all.
 */
bool validateMountpoint(const QString &rawPath, const QString &homeDir,
                        MountpointPlan *plan, QString *error);

/**
 * Creates and opens the mount point without ever following a symlink.
 *
 * Opens the authorized root, then walks only the relative suffix with
 * mkdirat()/openat() and O_NOFOLLOW, so the walk provably cannot leave the root
 * it was authorized against. Finishes with fchown()/fchmod() on the resulting
 * descriptor rather than on a name.
 *
 * Also verifies, race-free on descriptors, that the target is not already a
 * mount point (mount id via statx, falling back to st_dev), that it is empty,
 * and — if it already existed — that the caller already owns it. Every failure
 * is fatal; none of these checks may be skipped.
 *
 * Returns an open fd the caller must close, or -1 on failure.
 */
int openMountpointNoFollow(const MountpointPlan &plan, uid_t uid, gid_t gid, QString *error);

/**
 * Boot's counterpart to openMountpointNoFollow() (design §10.1, plan §4.1.5):
 * verifies every component of an *already-existing* mount point without ever
 * creating, chowning or chmoding anything.
 *
 * Walks only the relative suffix, O_NOFOLLOW at each component; a missing
 * component, a symlink, a wrong owner (checked against `expectedUid`, the
 * marker's recorded owner — never a live account's current uid, in case a
 * recycled/renumbered account has since diverged), a mount-boundary crossing,
 * or a non-empty final directory are all fatal, exactly like
 * openMountpointNoFollow()'s checks, but ENOENT gets its own distinguishable
 * message: boot must leave the share unarmed and report it, never invent the
 * missing path (a user's home may be encrypted/remote and not yet mounted, or
 * a removable backing filesystem for /media may be absent — creating the
 * apparent path on the underlying root filesystem would arm onto the wrong
 * place and could hide later-mounted data).
 *
 * Returns an open fd the caller must close, or -1 on failure.
 */
int openMountpointNoCreate(const MountpointPlan &plan, uid_t expectedUid, gid_t expectedGid, QString *error);

/**
 * The fixed CIFS option list for one owner, credential path and access mode.
 * An empty `credPath` selects `guest`.
 *
 * The access mode chooses `ro` and the file_mode/dir_mode pair, and nothing
 * else: every security-load-bearing term (nosuid, nodev, forceuid, forcegid,
 * nounix) is the same for all three modes and is not configurable.
 */
QString mountOptions(uid_t uid, gid_t gid, const QString &credPath, UnitValue::AccessMode access);

// ---------------------------------------------------------------------------
// Marker-v2 unit generation and the restricted-template validator (plan
// phase 1.2). Generation and validation share these same fixed-value
// functions, so there is exactly one definition of "what a share unit looks
// like" for a given marker + mount point: generation emits it, validation
// re-derives it and compares. That is what makes "any functional deviation
// becomes Tampered" true instead of aspirational.
// ---------------------------------------------------------------------------

/** Root directory for credential files: /etc/nasmount. A share's credential
 *  is persistent and root-owned; there is no second location. Design §5. */
QString credentialDirectory();

/** `<credentialDirectory()>/<id>.cred`. `id` must satisfy
 *  UnitValue::isValidShareId(); asserted, not re-validated. */
QString credentialPathFor(const QString &id);

/**
 * The complete, fixed `Options=` value for one marker (design §6.2):
 * identical safety and ownership options in every case, differing only in the
 * leading `guest` or `credentials=<path>` term and in the access mode's `ro`
 * and permission bits. The credential path is fully determined by the id; a
 * caller can never steer it independently, which is what makes "credential
 * path inconsistent with marker" a structural impossibility here rather than
 * a check performed elsewhere.
 *
 * This takes the whole marker rather than the four fields it reads, because
 * its two callers -- buildMountUnitContent() and validateMountUnitBody() --
 * are the generation and validation halves of the same template. Passing
 * loose fields would let one of them forward a stale or defaulted access mode
 * while the other forwarded the real one, which is exactly the
 * generation/validation split this function exists to make impossible.
 */
QString mountOptionsFor(const UnitValue::Marker &marker);

/**
 * Builds the complete `.mount` unit content for `marker` at `mountPoint`,
 * mounting `unc`. Both must already be validated (validateUnc /
 * validateMountpoint); this only encodes them. Fails only on an encoding
 * rejection (control characters, stray quote, trailing backslash — plan
 * §1.1's UnitValue::encodeUnitValue), which validated input never triggers.
 */
bool buildMountUnitContent(const UnitValue::Marker &marker, const QString &unc,
                           const QString &mountPoint, QString *content, QString *error);

/**
 * Builds the complete `.automount` unit content for `marker` at
 * `mountPoint`, including `ConditionPathIsDirectory=` (plan §1.2.3) so
 * systemd itself refuses to start if the path has vanished since validation.
 * That directive is emitted in `[Unit]`, the only section systemd parses a
 * `Condition*=` out of — in `[Automount]` it is ignored with a log line and
 * the guard silently does nothing, which matters most for a System share
 * armed by nasmount-boot before login.
 */
bool buildAutomountUnitContent(const UnitValue::Marker &marker, const QString &mountPoint,
                               QString *content, QString *error);

/**
 * Strictly parses `content`'s `[Mount]` section and validates it against the
 * restricted template for `marker` at `canonicalMountPoint`. Rejects a
 * duplicate or unknown functional directive, any line continuation anywhere
 * in the file, a non-`cifs` `Type=`, a `Where=` that disagrees with
 * `canonicalMountPoint`, an invalid `What=`, and an `Options=` that is not
 * *exactly* what mountOptionsFor() computes for this marker — which is what
 * makes a wrong/foreign/absent credential path, or a guest unit carrying
 * one, fail here rather than needing a separate check. `[Unit]`-section
 * descriptive text (Description=, the marker comment) is not inspected here;
 * the marker itself is validated by UnitValue::parseMarker(). On success,
 * `*what` receives the recovered, decoded UNC — the one value this cannot
 * derive in advance.
 */
bool validateMountUnitBody(const QString &content, const UnitValue::Marker &marker,
                           const QString &canonicalMountPoint, QString *what, QString *error);

/**
 * The `.automount` counterpart: validates `Where=`, `TimeoutIdleSec=` and
 * `DirectoryMode=` from `[Automount]`, plus `ConditionPathIsDirectory=` from
 * `[Unit]` where buildAutomountUnitContent() emits it, against the fixed
 * values that function would have produced for the same marker and mount
 * point. Every value here is fully determined — there is no per-share free
 * variable — so any mismatch at all is a rejection. `[Unit]`'s `Description=`
 * is permitted but not inspected.
 */
bool validateAutomountUnitBody(const QString &content, const UnitValue::Marker &marker,
                               const QString &canonicalMountPoint, QString *error);

} // namespace UnitSpec
