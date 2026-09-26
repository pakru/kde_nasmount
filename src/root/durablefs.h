/*
 * durablefs — descriptor-based, crash-durable filesystem primitives for the
 * privileged kde_nasmount-root library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every real call site in this library operates on root-owned trees
 * (/etc/nasmount, /run/nasmount) that a hostile local user may have already
 * planted a symlink or a foreign object inside. Nothing here ever trusts a
 * pathname: every component is opened O_NOFOLLOW and verified on its
 * descriptor before use, so a swapped path component after the check is
 * simply a different, already-opened fd, not something these calls can be
 * tricked into following.
 *
 * Every artifact this library writes has exactly one fixed owner/mode for
 * its kind — callers name *what* they are writing
 * (ArtifactKind), never a mode or uid, so a call site cannot accidentally
 * loosen a permission.
 */

#pragma once

#include <QByteArray>
#include <QString>

#include <sys/types.h>

namespace Root::DurableFs
{

enum class FileKind { Regular, Directory };

/**
 * The fixed artifact kinds this library ever writes, each with exactly one
 * root-owned mode:
 *
 *   Directory       — nasmount's own private root/subdirectories, holding
 *                     SensitiveFile content (credentials, the root lock):
 *                     0700
 *   PublicDirectory — a root-owned directory whose entries are meant to be
 *                     openable by any local process, holding only
 *                     PublicRecord content: 0755
 *   UnitFile        — generated .mount/.automount unit files: 0644
 *   SensitiveFile   — credentials and the root lock: 0600
 *   PublicRecord    — non-secret runtime records an unprivileged verifier
 *                     must be able to read back (e.g. a recorded automount
 *                     instance id — see runtimefiles.h): 0644
 *
 * A Directory never holds a PublicRecord and a PublicDirectory never holds a
 * SensitiveFile — keeping the two trees separate makes "is anything in here
 * readable by a non-owner" a property of which root a file lives under,
 * never a per-file exception inside a tree that is otherwise private.
 */
enum class ArtifactKind { Directory, PublicDirectory, UnitFile, SensitiveFile, PublicRecord };

mode_t modeFor(ArtifactKind kind);

/**
 * Pure descriptor check, exposed for testing with the caller's own uid —
 * every real call site in this library passes uid 0: `fd` must stat as
 * `expectedType`, owned by `expectedUid`, with mode exactly `expectedMode`
 * (not "at least as strict as" — a mode that has drifted at all is already
 * suspicious and is rejected rather than silently tolerated).
 */
bool verifyDescriptor(int fd, FileKind expectedType, uid_t expectedUid, mode_t expectedMode, QString *error);

/**
 * Opens a well-known absolute system directory this library does not own
 * and must not impose its own fixed artifact mode on (e.g. `/etc`, `/run` —
 * the roots `/etc/nasmount` and `/run/nasmount` are created below).
 * Verifies, on the descriptor: root-owned, a directory, and not
 * group/other-writable — a looser check than openVerifiedDir()'s, since a
 * system directory's own mode (typically 0755) is not this library's to
 * dictate. Everything created below it uses the exact, fixed policy.
 */
int openSystemRoot(const QString &absolutePath, QString *error);

/**
 * Opens an existing single-component directory name below the already
 * verified `parentFd` (from openSystemRoot() or a previous call here) with
 * O_DIRECTORY|O_NOFOLLOW and verifies it on the descriptor: root-owned, a
 * regular directory, mode exactly `modeFor(dirKind)` (`Directory` or
 * `PublicDirectory` — passing a file kind here is a caller bug). Fails
 * closed — returns -1 — on a symlink, wrong type, wrong owner, wrong mode,
 * or ENOENT; the caller distinguishes "missing" from "unsafe" via `error`
 * being empty only in the ENOENT case (no directory-listing side channel is
 * needed here, so both fail the same way operationally).
 */
int openVerifiedDir(int parentFd, const QString &name, ArtifactKind dirKind, QString *error);

/**
 * Like openVerifiedDir(), but creates the directory (mkdirat, root-owned,
 * mode `modeFor(dirKind)`) first if the name is entirely absent. Never
 * "repairs" an existing object that fails verification — that always fails
 * closed exactly like openVerifiedDir(), never silently chowns/chmods a
 * symlink or a foreign file into shape.
 */
int createAndVerifyDir(int parentFd, const QString &name, ArtifactKind dirKind, QString *error);

/**
 * Reads an existing regular file below `dirFd`, opened O_NOFOLLOW and
 * verified root-owned with mode exactly `modeFor(kind)`, and bounded to
 * `maxBytes` (checked via the descriptor's stat before any byte is read, so
 * an oversized file is rejected rather than partially read). `kind` lets
 * this read back a live 0644 unit file (e.g. to back it up before an
 * overwrite) as well as a 0600 credential/runtime/manifest/backup file —
 * whatever mode the file actually being read is supposed to have.
 */
bool readFileBounded(int dirFd, const QString &name, qint64 maxBytes, ArtifactKind kind, QByteArray *content,
                     QString *error);

/**
 * Durable create-or-replace: writes `content` to an
 * unpredictable O_CREAT|O_EXCL|O_NOFOLLOW temp file in the same directory
 * (so the later rename is atomic and same-filesystem), fchown/fchmod per
 * `kind`, fsyncs the temp file, renames it over `name`, then fsyncs the
 * directory. Never truncates the live file in place — a failure at any step
 * before the rename leaves the previous `name` byte-for-byte untouched.
 */
bool durableReplace(int dirFd, const QString &name, const QByteArray &content, ArtifactKind kind, QString *error);

/**
 * Checked unlink: unlinkat() then fsync the directory. `allowMissing`
 * decides whether ENOENT is itself success — an idempotent, expected
 * absence — or a reported failure; callers must be explicit
 * about which is expected rather than treating every ENOENT as fine.
 */
bool durableUnlink(int dirFd, const QString &name, bool allowMissing, QString *error);

/**
 * Recursively removes every entry below `parentFd`/`name` (regular files
 * only, each required to match `modeFor(entryKind)` exactly — this refuses
 * to recurse into a further subdirectory, which nothing this project ever
 * creates below its own root does — and refuses a symlink, device, foreign
 * owner, or drifted mode instead of turning a cleanup operation into an
 * unreviewed deletion primitive), fsyncing the directory after its contents
 * are gone, then removes `name` itself — verified as `dirKind` — from
 * `parentFd` and fsyncs `parentFd`. Used for full uninstall purge.
 */
bool durableRemoveTree(int parentFd, const QString &name, ArtifactKind dirKind, ArtifactKind entryKind,
                       QString *error);

} // namespace Root::DurableFs
