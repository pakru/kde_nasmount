/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unitspec.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace UnitSpec
{

QStringList staticAllowedRoots()
{
    return {QStringLiteral("/mnt"), QStringLiteral("/media")};
}

bool hasControlChars(const QString &value)
{
    for (const QChar c : value) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7F) {
            return true;
        }
    }
    return false;
}

bool validateUnc(const QString &unc, QString *normalised, QString *error)
{
    if (hasControlChars(unc)) {
        *error = QStringLiteral("UNC path contains control characters");
        return false;
    }
    // mount.cifs does not escape a comma in the share or subfolder, so the kernel would read
    // what follows as further mount options, ahead of or after the fixed Options=. No share
    // with a comma in this part ever mounted, so refusing it loses nothing.
    if (unc.contains(QLatin1Char(','))) {
        *error = QStringLiteral("UNC path may not contain a comma");
        return false;
    }

    QString value = unc;
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }

    static const QRegularExpression re(QStringLiteral("^//([A-Za-z0-9._-]+)/([^/].*)$"));
    const auto match = re.match(value);
    if (!match.hasMatch()) {
        *error = QStringLiteral("not a valid UNC path: %1 (expected //host/share)").arg(unc);
        return false;
    }

    const QStringList shareParts = match.captured(2).split(QLatin1Char('/'));
    if (shareParts.contains(QStringLiteral(".."))) {
        *error = QStringLiteral("share path may not contain '..'");
        return false;
    }

    *normalised = value;
    return true;
}

bool validateMountpoint(const QString &rawPath, const QString &homeDir,
                        MountpointPlan *plan, QString *error)
{
    if (hasControlChars(rawPath)) {
        *error = QStringLiteral("mount point contains control characters");
        return false;
    }
    if (!rawPath.startsWith(QLatin1Char('/'))) {
        *error = QStringLiteral("mount point must be an absolute path");
        return false;
    }
    // A value ending in a backslash triggers systemd's line-continuation
    // handling and swallows the next line of the unit; trailing whitespace is
    // stripped by systemd but is part of the real filename, so Where= would no
    // longer match the directory we created.
    if (rawPath.endsWith(QLatin1Char('\\'))) {
        *error = QStringLiteral("mount point may not end with a backslash");
        return false;
    }
    if (rawPath != rawPath.trimmed()) {
        *error = QStringLiteral("mount point may not begin or end with whitespace");
        return false;
    }

    const QString clean = QDir::cleanPath(rawPath);

    QStringList allowed = staticAllowedRoots();
    if (!homeDir.isEmpty()) {
        allowed << QDir::cleanPath(homeDir);
    }

    // Lexical containment only. No canonicalization: the pathname authorized here
    // is exactly the one openMountpointNoFollow() will walk, and it refuses to
    // traverse a symlink at any component, so there is no window in which a
    // swapped directory entry can redirect us outside this root.
    for (const QString &root : allowed) {
        if (clean == root) {
            *error = QStringLiteral("refusing to mount directly onto %1").arg(root);
            return false;
        }
        if (!clean.startsWith(root + QLatin1Char('/'))) {
            continue;
        }
        const QStringList suffix =
            clean.mid(root.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (suffix.isEmpty()) {
            *error = QStringLiteral("refusing to mount directly onto %1").arg(root);
            return false;
        }
        // cleanPath() already collapses "..", so a surviving one means something
        // cleanPath could not resolve; refuse rather than reason about it.
        if (suffix.contains(QStringLiteral(".."))) {
            *error = QStringLiteral("mount point may not contain '..'");
            return false;
        }
        plan->path = clean;
        plan->root = root;
        plan->suffix = suffix;
        return true;
    }

    *error = QStringLiteral("mount point must be below one of: %1")
                 .arg(allowed.join(QStringLiteral(", ")));
    return false;
}

namespace
{

/** Returns the mount id of a descriptor, or 0 if the kernel cannot report one. */
uint64_t mountIdOf(int fd)
{
#if defined(STATX_MNT_ID)
    struct statx sx {};
    if (::statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &sx) == 0 && (sx.stx_mask & STATX_MNT_ID)) {
        return sx.stx_mnt_id;
    }
#endif
    return 0;
}

/** Race-free emptiness test on an already-open directory descriptor. */
bool directoryIsEmpty(int dirFd, bool *empty, QString *error)
{
    const int dupFd = ::dup(dirFd);
    if (dupFd < 0) {
        *error = QStringLiteral("cannot duplicate directory descriptor: %1")
                     .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    DIR *dir = ::fdopendir(dupFd);
    if (!dir) {
        *error = QStringLiteral("cannot read directory: %1")
                     .arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(dupFd);
        return false;
    }

    *empty = true;
    errno = 0;
    while (struct dirent *entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..") {
            *empty = false;
            break;
        }
        errno = 0;
    }
    // readdir() returns nullptr for both end-of-directory and error; only errno
    // distinguishes them, so an I/O error must not be mistaken for "empty".
    const int savedErrno = errno;
    ::closedir(dir);
    if (*empty && savedErrno != 0) {
        *error = QStringLiteral("cannot enumerate directory: %1")
                     .arg(QString::fromLocal8Bit(strerror(savedErrno)));
        return false;
    }
    return true;
}

} // namespace

int openMountpointNoFollow(const MountpointPlan &plan, uid_t uid, gid_t gid, QString *error)
{
    if (plan.suffix.isEmpty()) {
        *error = QStringLiteral("no mount point components below %1").arg(plan.root);
        return -1;
    }

    // Start from the authorized root and never look above it. Because every
    // component below is opened O_NOFOLLOW, the walk cannot leave this root - so
    // the path we operate on is necessarily the path validateMountpoint()
    // authorized.
    int parentFd = ::open(plan.root.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parentFd < 0) {
        *error = QStringLiteral("cannot open %1: %2")
                     .arg(plan.root, QString::fromLocal8Bit(strerror(errno)));
        return -1;
    }

    for (int i = 0; i < plan.suffix.size(); ++i) {
        const QByteArray component = plan.suffix.at(i).toLocal8Bit();
        const bool isLast = (i == plan.suffix.size() - 1);

        bool created = false;
        if (::mkdirat(parentFd, component.constData(), 0700) == 0) {
            created = true;
        } else if (errno != EEXIST) {
            *error = QStringLiteral("cannot create %1 in %2: %3")
                         .arg(plan.suffix.at(i), plan.path, QString::fromLocal8Bit(strerror(errno)));
            ::close(parentFd);
            return -1;
        }

        const int childFd = ::openat(parentFd, component.constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (childFd < 0) {
            *error = QStringLiteral("refusing to traverse %1 in %2: not a directory, or it is a symlink (%3)")
                         .arg(plan.suffix.at(i), plan.path, QString::fromLocal8Bit(strerror(errno)));
            ::close(parentFd);
            return -1;
        }

        auto bail = [&](const QString &message) {
            *error = message;
            ::close(childFd);
            ::close(parentFd);
            return -1;
        };

        if (created && ::fchown(childFd, uid, gid) != 0) {
            return bail(QStringLiteral("cannot set owner on %1: %2")
                            .arg(plan.suffix.at(i), QString::fromLocal8Bit(strerror(errno))));
        }

        struct stat childStat {};
        struct stat parentStat {};
        if (::fstat(childFd, &childStat) != 0 || ::fstat(parentFd, &parentStat) != 0) {
            return bail(QStringLiteral("cannot stat %1: %2")
                            .arg(plan.path, QString::fromLocal8Bit(strerror(errno))));
        }

        // Mount ids, not st_dev: a bind mount from the same filesystem shares the
        // parent's device number. Checked at EVERY component, so the walk cannot
        // wander into a root-managed backup or removable filesystem partway down
        // and start creating directories inside it.
        const uint64_t childMnt = mountIdOf(childFd);
        const uint64_t parentMnt = mountIdOf(parentFd);
        const bool crossesMount = (childMnt && parentMnt)
            ? (childMnt != parentMnt)
            : (childStat.st_dev != parentStat.st_dev);

        // An existing directory we did not create belongs to somebody. Requiring
        // the caller to already own every component stops root from traversing a
        // foreign ancestor on their behalf: without this, a mode-0711 root-owned
        // /mnt/admin-storage lets any user ask for
        // /mnt/admin-storage/them/mountpoint and have root create - and hand them
        // - writable storage inside a tree they cannot write to themselves.
        if (!created && childStat.st_uid != uid) {
            return bail(QStringLiteral("Path '%1' is owned by uid=%2 mismatched with current user uid=%3. Change owner of this dir.")
                            .arg(plan.path).arg(childStat.st_uid).arg(uid));
        }

        if (isLast) {
            if (crossesMount) {
                return bail(QStringLiteral("%1 is already a mount point").arg(plan.path));
            }

            bool empty = false;
            QString emptyError;
            if (!directoryIsEmpty(childFd, &empty, &emptyError)) {
                return bail(QStringLiteral("%1: %2").arg(plan.path, emptyError));
            }
            if (!empty) {
                return bail(QStringLiteral("%1 is not empty - refusing to shadow existing data")
                                .arg(plan.path));
            }

            if (::fchown(childFd, uid, gid) != 0) {
                return bail(QStringLiteral("cannot set owner on %1: %2")
                                .arg(plan.path, QString::fromLocal8Bit(strerror(errno))));
            }
            // 0700 so another local user cannot walk into this share through a
            // mount point under /mnt or /media.
            if (::fchmod(childFd, 0700) != 0) {
                return bail(QStringLiteral("cannot set mode on %1: %2")
                                .arg(plan.path, QString::fromLocal8Bit(strerror(errno))));
            }

            ::close(parentFd);
            return childFd;
        }

        // Intermediate components must stay on the same filesystem as the
        // authorized root; crossing a mount here means we are no longer inside
        // the tree that was checked.
        if (crossesMount) {
            return bail(QStringLiteral("%1 crosses a mount point at %2")
                            .arg(plan.path, plan.suffix.at(i)));
        }

        ::close(parentFd);
        parentFd = childFd;
    }

    ::close(parentFd);
    *error = QStringLiteral("internal error walking mount point");
    return -1;
}

int openMountpointNoCreate(const MountpointPlan &plan, uid_t expectedUid, gid_t expectedGid, QString *error)
{
    if (plan.suffix.isEmpty()) {
        *error = QStringLiteral("no mount point components below %1").arg(plan.root);
        return -1;
    }

    int parentFd = ::open(plan.root.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parentFd < 0) {
        *error = QStringLiteral("cannot open %1: %2")
                     .arg(plan.root, QString::fromLocal8Bit(strerror(errno)));
        return -1;
    }

    for (int i = 0; i < plan.suffix.size(); ++i) {
        const QByteArray component = plan.suffix.at(i).toLocal8Bit();
        const bool isLast = (i == plan.suffix.size() - 1);

        const int childFd = ::openat(parentFd, component.constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (childFd < 0) {
            *error = (errno == ENOENT)
                ? QStringLiteral("%1 does not exist").arg(plan.path)
                : QStringLiteral("refusing to traverse %1 in %2: not a directory, or it is a symlink (%3)")
                      .arg(plan.suffix.at(i), plan.path, QString::fromLocal8Bit(strerror(errno)));
            ::close(parentFd);
            return -1;
        }

        auto bail = [&](const QString &message) {
            *error = message;
            ::close(childFd);
            ::close(parentFd);
            return -1;
        };

        struct stat childStat {};
        struct stat parentStat {};
        if (::fstat(childFd, &childStat) != 0 || ::fstat(parentFd, &parentStat) != 0) {
            return bail(QStringLiteral("cannot stat %1: %2")
                            .arg(plan.path, QString::fromLocal8Bit(strerror(errno))));
        }

        // Every component must already belong to the recorded owner -- nothing
        // here is ever created, so there is no "we just made this, it is
        // necessarily ours" case the way openMountpointNoFollow() has. Boot
        // checks the recorded gid too, stricter than the interactive walk's uid-only check,
        // since nothing here can self-correct a drifted group the way a fresh
        // fchown() would.
        if (childStat.st_uid != expectedUid || childStat.st_gid != expectedGid) {
            return bail(QStringLiteral("%1 is owned by uid %2 gid %3, not the recorded owner %4:%5")
                            .arg(plan.path)
                            .arg(childStat.st_uid)
                            .arg(childStat.st_gid)
                            .arg(expectedUid)
                            .arg(expectedGid));
        }

        const uint64_t childMnt = mountIdOf(childFd);
        const uint64_t parentMnt = mountIdOf(parentFd);
        const bool crossesMount = (childMnt && parentMnt)
            ? (childMnt != parentMnt)
            : (childStat.st_dev != parentStat.st_dev);

        if (isLast) {
            if (crossesMount) {
                return bail(QStringLiteral("%1 is already a mount point").arg(plan.path));
            }
            bool empty = false;
            QString emptyError;
            if (!directoryIsEmpty(childFd, &empty, &emptyError)) {
                return bail(QStringLiteral("%1: %2").arg(plan.path, emptyError));
            }
            if (!empty) {
                return bail(QStringLiteral("%1 is not empty - refusing to arm onto existing data")
                                .arg(plan.path));
            }
            ::close(parentFd);
            return childFd;
        }

        if (crossesMount) {
            return bail(QStringLiteral("%1 crosses a mount point at %2")
                            .arg(plan.path, plan.suffix.at(i)));
        }

        ::close(parentFd);
        parentFd = childFd;
    }

    ::close(parentFd);
    *error = QStringLiteral("internal error walking mount point");
    return -1;
}

namespace
{

#ifndef STATX_ATTR_AUTOMOUNT
#define STATX_ATTR_AUTOMOUNT 0x00001000
#endif

/** What previewMountpointProblem() learns about one path, without opening it. */
struct Examined {
    mode_t mode = 0;
    uid_t uid = 0;
    uint64_t mountId = 0; ///< 0 when the kernel cannot report one
    dev_t device = 0;
    bool automountTrigger = false;
};

/**
 * statx() with AT_NO_AUTOMOUNT: describes `name` in `dirFd` (or `name` alone
 * when it is absolute) without mounting it. `followLinks` is for the allowed
 * root only, which the real walk opens by plain path; every component below
 * it is examined as itself.
 */
bool examine(int dirFd, const QByteArray &name, bool followLinks, Examined *info, int *error)
{
    struct statx sx {};
    unsigned mask = STATX_TYPE | STATX_UID;
#if defined(STATX_MNT_ID)
    mask |= STATX_MNT_ID;
#endif
    const int flags = AT_NO_AUTOMOUNT | (followLinks ? 0 : AT_SYMLINK_NOFOLLOW);
    if (::statx(dirFd, name.constData(), flags, mask, &sx) != 0) {
        *error = errno;
        return false;
    }
    info->mode = sx.stx_mode;
    info->uid = sx.stx_uid;
    info->device = makedev(sx.stx_dev_major, sx.stx_dev_minor);
#if defined(STATX_MNT_ID)
    info->mountId = (sx.stx_mask & STATX_MNT_ID) ? sx.stx_mnt_id : 0;
#endif
    info->automountTrigger = (sx.stx_attributes_mask & STATX_ATTR_AUTOMOUNT)
        && (sx.stx_attributes & STATX_ATTR_AUTOMOUNT);
    return true;
}

/** Closes a descriptor on every way out of the preview walk. */
struct FdGuard {
    int fd = -1;
    ~FdGuard()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    void reset(int next)
    {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = next;
    }
};

} // namespace

QString previewMountpointProblem(const MountpointPlan &plan, uid_t uid)
{
    if (plan.suffix.isEmpty()) {
        return QString();
    }

    // The root, examined and opened like the real walk opens it -- by plain
    // path, following a link such as /media -> /run/media -- but without
    // O_DIRECTORY, which would mount it were it an automount trigger.
    const QByteArray rootPath = plan.root.toLocal8Bit();
    Examined root;
    int err = 0;
    if (!examine(AT_FDCWD, rootPath, /*followLinks=*/true, &root, &err) || !S_ISDIR(root.mode)
        || root.automountTrigger) {
        return QString();
    }
    FdGuard dir;
    dir.reset(::open(rootPath.constData(), O_PATH | O_CLOEXEC));
    if (dir.fd < 0) {
        return QString();
    }

    QString walked = plan.root;
    for (int i = 0; i < plan.suffix.size(); ++i) {
        const QString &component = plan.suffix.at(i);
        const QByteArray name = component.toLocal8Bit();
        const bool isLast = (i == plan.suffix.size() - 1);
        walked = QDir::cleanPath(walked + QLatin1Char('/') + component);

        Examined info;
        if (!examine(dir.fd, name, /*followLinks=*/false, &info, &err)) {
            // ENOENT: everything from here on is created by the helper, which
            // is fine. Any other errno is one this cannot interpret.
            return QString();
        }
        if (S_ISLNK(info.mode)) {
            return QStringLiteral("%1 is a symbolic link; choose the folder it points to").arg(walked);
        }
        if (!S_ISDIR(info.mode)) {
            return QStringLiteral("%1 is a file, not a folder").arg(walked);
        }

        // Mount ids, as in the real walk, so a bind mount from the same
        // filesystem is caught too. An automount that has not fired yet is an
        // autofs mount of its own, so it differs from its parent as well; the
        // attribute covers a trigger that is not a mount at all.
        const bool crossesMount = info.automountTrigger
            || ((info.mountId && root.mountId) ? (info.mountId != root.mountId) : (info.device != root.device));
        if (crossesMount) {
            return isLast ? QStringLiteral("Something is already mounted on %1").arg(walked)
                          : QStringLiteral("%1 is on another filesystem or mount; choose a folder that is "
                                           "not inside it").arg(walked);
        }
        if (info.uid != uid) {
            return QStringLiteral("%1 belongs to another user").arg(walked);
        }

        if (isLast) {
            // Safe to open for reading now: it is on the root's own mount and
            // not a trigger, so opening it cannot mount anything.
            FdGuard last;
            last.reset(::openat(dir.fd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (last.fd < 0) {
                return QString();
            }
            bool empty = false;
            QString ignored;
            if (directoryIsEmpty(last.fd, &empty, &ignored) && !empty) {
                return QStringLiteral("%1 is not empty; choose an empty folder, so the mount does not hide "
                                      "its files").arg(walked);
            }
            return QString();
        }

        // Descend by O_PATH without O_DIRECTORY: neither triggers an
        // automount, and the component was just shown to be a plain directory
        // on the same mount.
        const int next = ::openat(dir.fd, name.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            return QString();
        }
        dir.reset(next);
    }
    return QString();
}

QString mountOptions(uid_t uid, gid_t gid, const QString &credPath, UnitValue::AccessMode access)
{
    QStringList opts;
    opts << (credPath.isEmpty() ? QStringLiteral("guest")
                                : QStringLiteral("credentials=%1").arg(credPath));
    // nosuid/nodev are not optional, and are not affected by the access mode.
    // A caller may point this at their own Samba server; root CIFS mounts
    // permit setuid execution by default, so without these the server can
    // present a root-owned setuid binary for the caller to run. That is why
    // ReadWriteExecutable can hand out the execute bit safely: nosuid still
    // stands, so +x never becomes set-user-ID execution.
    // forceuid/forcegid stop a server-supplied owner overriding uid=/gid=.
    opts << QStringLiteral("nosuid")
         << QStringLiteral("nodev")
         << QStringLiteral("forceuid")
         << QStringLiteral("forcegid")
         << QStringLiteral("uid=%1").arg(uid)
         << QStringLiteral("gid=%1").arg(gid)
         // file_mode/dir_mode only govern when the server is not supplying its
         // own POSIX modes. There is no "forcemode" counterpart to forceuid, so a
         // hostile server negotiating Unix extensions could report 0777 and
         // defeat the modes below; nounix removes that possibility and keeps
         // permission checking on the client. It is also what makes the access
         // mode meaningful at all -- without it the server could hand back
         // writable modes for a read-only share.
         << QStringLiteral("nounix")
         << QStringLiteral("iocharset=utf8");

    // Exactly three shapes, and the terms are not interchangeable.
    //
    // `ro` and the permission bits are both required for ReadOnly, and
    // neither is redundant: `ro` sets MS_RDONLY, which the kernel enforces
    // against every user including root, while the modes are what let Dolphin
    // grey out paste/rename up front instead of letting a copy fail partway
    // through. dir_mode=0500 rather than 0400 because a directory still needs
    // +x to be traversable at all.
    //
    // ReadWriteExecutable differs from the default only in the execute bit.
    // file_mode applies to every regular file the server does not supply a
    // POSIX mode for (and nounix above guarantees it never does), so this
    // marks *all* files executable, not only real programs. That is why it is
    // opt-in and separately labelled rather than folded into read-write.
    //
    // `rw` is deliberately never emitted for ReadWrite. It would be
    // semantically identical to emitting nothing, and it would change the
    // generated bytes for every share written by 0.1.0-0.1.3 -- turning all
    // of them Tampered on upgrade. Do not "make it explicit".
    switch (access) {
    case UnitValue::AccessMode::ReadWrite:
        opts << QStringLiteral("file_mode=0600") << QStringLiteral("dir_mode=0700");
        break;
    case UnitValue::AccessMode::ReadOnly:
        opts << QStringLiteral("ro") << QStringLiteral("file_mode=0400")
             << QStringLiteral("dir_mode=0500");
        break;
    case UnitValue::AccessMode::ReadWriteExecutable:
        opts << QStringLiteral("file_mode=0700") << QStringLiteral("dir_mode=0700");
        break;
    }
    return opts.join(QLatin1Char(','));
}

// ---------------------------------------------------------------------------
// Marker unit generation and the restricted-template validator.
// ---------------------------------------------------------------------------

QString credentialDirectory()
{
    return QStringLiteral("/etc/nasmount");
}

QString credentialPathFor(const QString &id)
{
    Q_ASSERT(UnitValue::isValidShareId(id));
    return QStringLiteral("%1/%2.cred").arg(credentialDirectory(), id);
}

QString mountOptionsFor(const UnitValue::Marker &marker)
{
    const QString credPath = (marker.authentication == UnitValue::AuthenticationKind::Guest)
        ? QString()
        : credentialPathFor(marker.id);
    return mountOptions(marker.ownerUid, marker.ownerGid, credPath, marker.access);
}

namespace
{

bool hasLineContinuation(const QString &content)
{
    for (const QString &line : content.split(QLatin1Char('\n'))) {
        if (line.endsWith(QLatin1Char('\\'))) {
            return true;
        }
    }
    return false;
}

/**
 * Extracts every `Key=Value` line found textually inside `[section]` (up to
 * the next `[...]` header or end of file). A key outside `allowedKeys`, a
 * duplicate key, or a non-blank line that is not a well-formed assignment is
 * rejected outright - there is no permissive fallback, and no comment lines
 * are tolerated inside a functional section (this tool never generates one).
 */
bool extractSectionAssignments(const QString &content, const QString &section,
                               const QStringList &allowedKeys, QMap<QString, QString> *values,
                               QString *error)
{
    static const QRegularExpression sectionHeader(QStringLiteral("^\\[([A-Za-z]+)\\]$"));
    static const QRegularExpression assignment(QStringLiteral("^([A-Za-z][A-Za-z0-9]*)=(.*)$"));

    bool inSection = false;
    bool sectionSeen = false;
    for (const QString &line : content.split(QLatin1Char('\n'))) {
        const auto headerMatch = sectionHeader.match(line);
        if (headerMatch.hasMatch()) {
            inSection = (headerMatch.captured(1) == section);
            sectionSeen = sectionSeen || inSection;
            continue;
        }
        if (!inSection || line.trimmed().isEmpty()) {
            continue;
        }
        const auto m = assignment.match(line);
        if (!m.hasMatch()) {
            *error = QStringLiteral("unparseable line in [%1]: %2").arg(section, line);
            return false;
        }
        const QString key = m.captured(1);
        if (!allowedKeys.contains(key)) {
            *error = QStringLiteral("unknown directive in [%1]: %2").arg(section, key);
            return false;
        }
        if (values->contains(key)) {
            *error = QStringLiteral("duplicate assignment in [%1]: %2").arg(section, key);
            return false;
        }
        values->insert(key, m.captured(2));
    }
    if (!sectionSeen) {
        *error = QStringLiteral("missing [%1] section").arg(section);
        return false;
    }
    return true;
}

QString undoublePercentLocal(const QString &value)
{
    QString out = value;
    out.replace(QStringLiteral("%%"), QStringLiteral("%"));
    return out;
}

} // namespace

bool buildMountUnitContent(const UnitValue::Marker &marker, const QString &unc,
                           const QString &mountPoint, QString *content, QString *error)
{
    QString what;
    QString where;
    QString options;
    if (!UnitValue::encodeUnitValue(unc, &what, error)
        || !UnitValue::encodeUnitValue(mountPoint, &where, error)
        || !UnitValue::encodeUnitValue(mountOptionsFor(marker), &options, error)) {
        return false;
    }
    // TimeoutSec=30: a share's unmount can hang past this shutdown when it
    // races an interface going down mid-transfer (the kernel's umount() then
    // blocks on an SMB round trip the server will never answer) -- observed
    // in practice taking the full systemd default of 90s per stuck share
    // before being force-killed. 30s is long enough for a clean unmount,
    // short enough that one stuck share doesn't meaningfully delay a reboot.
    //
    // The first line keeps its em dash (U+2014) while every other dash in the
    // sources is a plain hyphen: this is the byte string every released
    // version wrote into /etc/systemd/system, so "normalising" it makes all
    // existing units differ from what is regenerated, and every share
    // becomes Tampered. goldenunits_test is the gate; the automount header
    // below is the same line.
    *content =QStringLiteral("# Managed by nasmount — do not edit by hand; use the KCM or the dialog.\n%1"
                              "[Unit]\n"
                              "Description=nasmount: %2\n\n"
                              "[Mount]\n"
                              "What=%3\n"
                              "Where=%4\n"
                              "Type=cifs\n"
                              "Options=%5\n"
                              "TimeoutSec=30\n")
                   .arg(UnitValue::markerComment(marker), unc.toHtmlEscaped(), what, where, options);
    return true;
}

bool buildAutomountUnitContent(const UnitValue::Marker &marker, const QString &mountPoint,
                               QString *content, QString *error)
{
    QString where;
    if (!UnitValue::encodeUnitValue(mountPoint, &where, error)) {
        return false;
    }
    // ConditionPathIsDirectory= belongs in [Unit], not [Automount]: every
    // Condition*= directive is parsed out of [Unit] by systemd.unit(5), and
    // systemd logs "Unknown key name ... in section 'Automount', ignoring"
    // for one placed anywhere else -- silently discarding the guard rather
    // than failing. This matters because nasmount-boot arms every share
    // before login: without the condition systemd would
    // set the trigger up over a missing path and, because DirectoryMode= is
    // set, create that path itself.
    *content = QStringLiteral("# Managed by nasmount — do not edit by hand; use the KCM or the dialog.\n%1"
                              "[Unit]\n"
                              "Description=nasmount automount: %2\n"
                              "ConditionPathIsDirectory=%3\n\n"
                              "[Automount]\n"
                              "Where=%3\n"
                              "TimeoutIdleSec=60\n"
                              "DirectoryMode=0700\n")
                   .arg(UnitValue::markerComment(marker), mountPoint.toHtmlEscaped(), where);
    return true;
}

bool validateMountUnitBody(const QString &content, const UnitValue::Marker &marker,
                           const QString &canonicalMountPoint, QString *what, QString *error)
{
    if (hasLineContinuation(content)) {
        *error = QStringLiteral("line continuation is not permitted");
        return false;
    }

    QMap<QString, QString> values;
    static const QStringList allowed = {QStringLiteral("What"), QStringLiteral("Where"), QStringLiteral("Type"),
                                        QStringLiteral("Options"), QStringLiteral("TimeoutSec")};
    if (!extractSectionAssignments(content, QStringLiteral("Mount"), allowed, &values, error)) {
        return false;
    }
    for (const QString &key : allowed) {
        if (!values.contains(key)) {
            *error = QStringLiteral("missing [Mount] directive: %1").arg(key);
            return false;
        }
    }

    if (values.value(QStringLiteral("Type")) != QStringLiteral("cifs")) {
        *error = QStringLiteral("not a CIFS mount");
        return false;
    }
    if (values.value(QStringLiteral("TimeoutSec")) != QStringLiteral("30")) {
        *error = QStringLiteral("unexpected TimeoutSec=");
        return false;
    }
    if (undoublePercentLocal(values.value(QStringLiteral("Where"))) != canonicalMountPoint) {
        *error = QStringLiteral("Where= disagrees with the mount point");
        return false;
    }

    const QString decodedUnc = undoublePercentLocal(values.value(QStringLiteral("What")));
    QString uncNormalised;
    QString uncError;
    if (!validateUnc(decodedUnc, &uncNormalised, &uncError)) {
        *error = QStringLiteral("invalid What=: %1").arg(uncError);
        return false;
    }

    const QString expectedOptions = mountOptionsFor(marker);
    QString expectedOptionsEncoded;
    QString encodeError;
    if (!UnitValue::encodeUnitValue(expectedOptions, &expectedOptionsEncoded, &encodeError)) {
        *error = QStringLiteral("internal error deriving expected options: %1").arg(encodeError);
        return false;
    }
    if (values.value(QStringLiteral("Options")) != expectedOptionsEncoded) {
        *error = QStringLiteral("Options= does not match the fixed template for this share's "
                                "mode/authentication/access/id");
        return false;
    }

    *what = decodedUnc;
    return true;
}

bool validateAutomountUnitBody(const QString &content, const UnitValue::Marker &marker,
                               const QString &canonicalMountPoint, QString *error)
{
    Q_UNUSED(marker);
    if (hasLineContinuation(content)) {
        *error = QStringLiteral("line continuation is not permitted");
        return false;
    }

    QMap<QString, QString> values;
    static const QStringList allowed = {QStringLiteral("Where"), QStringLiteral("TimeoutIdleSec"),
                                        QStringLiteral("DirectoryMode")};
    if (!extractSectionAssignments(content, QStringLiteral("Automount"), allowed, &values, error)) {
        return false;
    }
    for (const QString &key : allowed) {
        if (!values.contains(key)) {
            *error = QStringLiteral("missing [Automount] directive: %1").arg(key);
            return false;
        }
    }

    // ConditionPathIsDirectory= is validated out of [Unit], which is the only
    // section systemd parses a Condition*= from -- generation puts it there
    // for exactly that reason (buildAutomountUnitContent). Description= is
    // descriptive text this deliberately does not inspect, but it has to be
    // permitted or extractSectionAssignments()'s strict unknown-directive
    // rejection would refuse our own generated unit.
    QMap<QString, QString> unitValues;
    static const QStringList unitAllowed = {QStringLiteral("Description"),
                                            QStringLiteral("ConditionPathIsDirectory")};
    if (!extractSectionAssignments(content, QStringLiteral("Unit"), unitAllowed, &unitValues, error)) {
        return false;
    }
    if (!unitValues.contains(QStringLiteral("ConditionPathIsDirectory"))) {
        *error = QStringLiteral("missing [Unit] directive: ConditionPathIsDirectory");
        return false;
    }

    QString expectedWhere;
    QString encodeError;
    if (!UnitValue::encodeUnitValue(canonicalMountPoint, &expectedWhere, &encodeError)) {
        *error = QStringLiteral("internal error encoding mount point: %1").arg(encodeError);
        return false;
    }
    if (values.value(QStringLiteral("Where")) != expectedWhere) {
        *error = QStringLiteral("Where= disagrees with the mount point");
        return false;
    }
    if (unitValues.value(QStringLiteral("ConditionPathIsDirectory")) != expectedWhere) {
        *error = QStringLiteral("ConditionPathIsDirectory= disagrees with the mount point");
        return false;
    }
    if (values.value(QStringLiteral("TimeoutIdleSec")) != QStringLiteral("60")) {
        *error = QStringLiteral("unexpected TimeoutIdleSec=");
        return false;
    }
    if (values.value(QStringLiteral("DirectoryMode")) != QStringLiteral("0700")) {
        *error = QStringLiteral("unexpected DirectoryMode=");
        return false;
    }
    return true;
}

} // namespace UnitSpec
