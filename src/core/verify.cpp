/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "verify.h"
#include "unitspec.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QSet>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace Verify
{

namespace
{

enum class FileProbe { Missing, Tampered, Ok };

/**
 * Opens with O_NOFOLLOW (a symlinked "unit file" is refused without ever
 * reading through it) and re-checks type/owner/mode on the descriptor, so
 * there is no window between validating the name and reading its content.
 */
FileProbe probeUnitFile(const QString &path, QString *content)
{
    const QByteArray raw = path.toLocal8Bit();
    const int fd = ::open(raw.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return (errno == ENOENT) ? FileProbe::Missing : FileProbe::Tampered;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0
        || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        ::close(fd);
        return FileProbe::Tampered;
    }
    QFile f;
    if (!f.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return FileProbe::Tampered;
    }
    *content = QString::fromUtf8(f.readAll());
    return FileProbe::Ok;
}

/** Presence of anything at all here is refused: a drop-in can override any
 *  directive the restricted template validated, so none is ever accepted. */
bool dropInExists(const QString &unitPath)
{
    struct stat st {};
    return ::lstat((unitPath + QStringLiteral(".d")).toLocal8Bit().constData(), &st) == 0;
}

QString undoublePercent(const QString &value)
{
    QString out = value;
    out.replace(QStringLiteral("%%"), QStringLiteral("%"));
    return out;
}

/** Last assignment wins, matching systemd's own handling of a duplicated key. */
bool extractAssignment(const QString &content, const QString &key, QString *value)
{
    bool found = false;
    const QString prefix = key + QLatin1Char('=');
    for (const QString &line : content.split(QLatin1Char('\n'))) {
        if (line.startsWith(prefix)) {
            *value = line.mid(prefix.size());
            found = true;
        }
    }
    return found;
}

enum class SideState { NotPresent, NoMarker, Broken, Valid };

struct SideCheck {
    SideState state = SideState::NotPresent;
    UnitValue::Marker marker;
    QString what; ///< the .mount side's own What=; empty for the .automount side
};

/** Parses and validates one half's marker, independent of its body. Shared by
 *  both typed evaluators below. */
bool parseOwnedMarker(const QString &content, uid_t expectedUid, UnitValue::Marker *marker)
{
    if (!UnitValue::hasMarker(content)) {
        return false;
    }
    QString markerError;
    return UnitValue::parseMarker(content, marker, &markerError) && marker->ownerUid == expectedUid;
}

SideCheck evaluateMountSide(FileProbe probe, const QString &content, uid_t expectedUid,
                           const QString &canonicalMountPoint)
{
    SideCheck sc;
    if (probe == FileProbe::Missing) {
        return sc;
    }
    if (!UnitValue::hasMarker(content)) {
        sc.state = SideState::NoMarker;
        return sc;
    }
    UnitValue::Marker marker;
    if (!parseOwnedMarker(content, expectedUid, &marker)) {
        sc.state = SideState::Broken;
        return sc;
    }
    QString what, bodyError;
    if (!UnitSpec::validateMountUnitBody(content, marker, canonicalMountPoint, &what, &bodyError)) {
        sc.state = SideState::Broken;
        return sc;
    }
    sc.state = SideState::Valid;
    sc.marker = marker;
    sc.what = what;
    return sc;
}

SideCheck evaluateAutomountSide(FileProbe probe, const QString &content, uid_t expectedUid,
                                const QString &canonicalMountPoint)
{
    SideCheck sc;
    if (probe == FileProbe::Missing) {
        return sc;
    }
    if (!UnitValue::hasMarker(content)) {
        sc.state = SideState::NoMarker;
        return sc;
    }
    UnitValue::Marker marker;
    if (!parseOwnedMarker(content, expectedUid, &marker)) {
        sc.state = SideState::Broken;
        return sc;
    }
    QString bodyError;
    if (!UnitSpec::validateAutomountUnitBody(content, marker, canonicalMountPoint, &bodyError)) {
        sc.state = SideState::Broken;
        return sc;
    }
    sc.state = SideState::Valid;
    sc.marker = marker;
    return sc;
}

bool readMountinfoFile(QString *content)
{
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    *content = QString::fromUtf8(f.readAll());
    return true;
}

QString unescapeMountinfoField(const QString &field)
{
    QString out = field;
    out.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    out.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    out.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    // Backslash last: decoding it earlier could fabricate one of the patterns
    // above out of a filename's own literal digits.
    out.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return out;
}

bool systemctlShow(const QString &unit, const QString &property, QString *value)
{
    QProcess proc;
    proc.start(QStringLiteral("systemctl"),
              {QStringLiteral("show"), unit, QStringLiteral("--property=%1").arg(property),
               QStringLiteral("--value")});
    if (!proc.waitForFinished(10000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        return false;
    }
    *value = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
    return true;
}

const QLatin1String AutomountIdDir("/run/nasmount-ids");

} // namespace

namespace
{

/** Copies the identity fields of a validated marker onto a DefinitionCheck. */
void applyMarker(DefinitionCheck *result, const UnitValue::Marker &marker)
{
    result->id = marker.id;
    result->ownerUid = marker.ownerUid;
    result->ownerGid = marker.ownerGid;
    result->authentication = marker.authentication;
    result->access = marker.access;
}

} // namespace

DefinitionCheck inspectDefinition(const UnitValue::UnitPaths &paths, uid_t expectedUid,
                                  const QString &canonicalMountPoint)
{
    DefinitionCheck result;
    result.canonicalMountPoint = canonicalMountPoint;
    result.mountUnitName = paths.unitName + QStringLiteral(".mount");
    result.automountUnitName = paths.unitName + QStringLiteral(".automount");

    QString mountContent;
    QString automountContent;
    const FileProbe mountProbe = probeUnitFile(paths.mountUnitPath, &mountContent);
    const FileProbe automountProbe = probeUnitFile(paths.automountUnitPath, &automountContent);

    if (mountProbe == FileProbe::Tampered || automountProbe == FileProbe::Tampered) {
        result.state = Definition::Tampered;
        result.reason = QStringLiteral("unsafe-file");
        result.detail = QStringLiteral(
            "unit file is a symlink, not a regular root-owned file, or is group/world-writable");
        return result;
    }
    if (dropInExists(paths.mountUnitPath) || dropInExists(paths.automountUnitPath)) {
        result.state = Definition::Tampered;
        result.reason = QStringLiteral("drop-in-present");
        result.detail = QStringLiteral("a drop-in exists for this unit; drop-ins are rejected outright");
        return result;
    }
    if (mountProbe == FileProbe::Missing && automountProbe == FileProbe::Missing) {
        result.state = Definition::None;
        return result;
    }

    const SideCheck mountSide = evaluateMountSide(mountProbe, mountContent, expectedUid, canonicalMountPoint);
    const SideCheck automountSide =
        evaluateAutomountSide(automountProbe, automountContent, expectedUid, canonicalMountPoint);

    if (mountSide.state == SideState::Broken || automountSide.state == SideState::Broken) {
        result.state = Definition::Tampered;
        result.reason = QStringLiteral("marker-or-body-invalid");
        result.detail = QStringLiteral(
            "marker incomplete, owner-uid mismatched, or the unit body does not match the restricted template");
        return result;
    }
    if (mountSide.state == SideState::NoMarker || automountSide.state == SideState::NoMarker) {
        result.state = Definition::NotOurs;
        result.reason = QStringLiteral("not-ours");
        result.detail = QStringLiteral("a unit already exists at this name and is not managed by this tool");
        return result;
    }
    if (mountSide.state == SideState::Valid && automountSide.state == SideState::Valid) {
        if (mountSide.marker != automountSide.marker) {
            result.state = Definition::Tampered;
            result.reason = QStringLiteral("pair-marker-mismatch");
            result.detail = QStringLiteral("the pair disagrees on a marker field");
            return result;
        }
        result.state = Definition::Pair;
        applyMarker(&result, mountSide.marker);
        result.what = mountSide.what;
        return result;
    }
    if (mountSide.state == SideState::Valid || automountSide.state == SideState::Valid) {
        result.state = Definition::Partial;
        applyMarker(&result, (mountSide.state == SideState::Valid) ? mountSide.marker : automountSide.marker);
        result.what = mountSide.what;
        result.reason = QStringLiteral("partial");
        result.detail = QStringLiteral("only one half of the pair exists");
        return result;
    }
    result.state = Definition::None;
    return result;
}

QList<OwnedUnit> pairScannedHalves(const QList<ScannedHalf> &halves)
{
    struct Group {
        std::optional<ScannedHalf> mountHalf;
        std::optional<ScannedHalf> automountHalf;
    };
    QMap<QString, Group> groups;
    for (const ScannedHalf &half : halves) {
        Group &g = groups[half.baseName];
        (half.isMount ? g.mountHalf : g.automountHalf) = half;
    }

    QList<OwnedUnit> result;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        OwnedUnit unit;
        unit.unitName = it.key();
        const Group &g = it.value();
        if (g.mountHalf && g.automountHalf) {
            if (g.mountHalf->marker != g.automountHalf->marker || g.mountHalf->where != g.automountHalf->where) {
                unit.state = Definition::Tampered;
                unit.detail = QStringLiteral("the pair disagrees on a marker field or Where=");
            } else {
                unit.state = Definition::Pair;
                unit.mountPoint = g.mountHalf->where;
                unit.id = g.mountHalf->marker.id;
                unit.ownerUid = g.mountHalf->marker.ownerUid;
                unit.ownerGid = g.mountHalf->marker.ownerGid;
                unit.authentication = g.mountHalf->marker.authentication;
                unit.access = g.mountHalf->marker.access;
                unit.what = g.mountHalf->what;
            }
        } else {
            const ScannedHalf &only = g.mountHalf ? *g.mountHalf : *g.automountHalf;
            unit.state = Definition::Partial;
            unit.mountPoint = only.where;
            unit.id = only.marker.id;
            unit.ownerUid = only.marker.ownerUid;
            unit.ownerGid = only.marker.ownerGid;
            unit.authentication = only.marker.authentication;
            unit.access = only.marker.access;
            unit.detail = QStringLiteral("only one half of the pair exists");
            unit.what = g.mountHalf ? g.mountHalf->what : QString();
        }
        result.append(unit);
    }

    // Second pass: two different base names must never claim the same id or
    // the same mount point -- surface it, never pick one.
    QMap<QString, QList<int>> byId;
    QMap<QString, QList<int>> byMountPoint;
    for (int i = 0; i < result.size(); ++i) {
        if (result.at(i).state == Definition::Tampered) {
            continue;
        }
        byId[result.at(i).id].append(i);
        byMountPoint[result.at(i).mountPoint].append(i);
    }
    QSet<int> collided;
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        if (it.value().size() > 1) {
            collided.unite(QSet<int>(it.value().cbegin(), it.value().cend()));
        }
    }
    for (auto it = byMountPoint.constBegin(); it != byMountPoint.constEnd(); ++it) {
        if (it.value().size() > 1) {
            collided.unite(QSet<int>(it.value().cbegin(), it.value().cend()));
        }
    }
    for (int i : std::as_const(collided)) {
        result[i].state = Definition::Tampered;
        result[i].detail = QStringLiteral("collides with another definition on id or mount point");
        result[i].id.clear();
        result[i].ownerUid = 0;
        result[i].ownerGid = 0;
        result[i].authentication = UnitValue::AuthenticationKind::Credentials;
        result[i].access = UnitValue::AccessMode::ReadWrite;
    }
    return result;
}

namespace
{

/** Shared by enumerateOwnedUnits()/enumerateManagedUnits(): scans
 *  /etc/systemd/system for every .mount/.automount unit carrying this
 *  tool's complete marker, optionally restricted to one owner uid. */
QList<ScannedHalf> scanManagedHalves(const std::optional<uid_t> &onlyUid)
{
    QList<ScannedHalf> halves;
    QDir dir(QStringLiteral("/etc/systemd/system"));
    const QStringList files =
        dir.entryList({QStringLiteral("*.mount"), QStringLiteral("*.automount")}, QDir::Files);
    for (const QString &name : files) {
        QString content;
        if (probeUnitFile(dir.filePath(name), &content) != FileProbe::Ok) {
            continue;
        }
        UnitValue::Marker marker;
        QString markerError;
        if (!UnitValue::parseMarker(content, &marker, &markerError)) {
            continue;
        }
        if (onlyUid && marker.ownerUid != *onlyUid) {
            continue;
        }
        QString whereRaw;
        if (!extractAssignment(content, QStringLiteral("Where"), &whereRaw)) {
            continue;
        }

        ScannedHalf half;
        half.isMount = name.endsWith(QStringLiteral(".mount"));
        half.baseName = half.isMount ? name.chopped(6) : name.chopped(10);
        half.marker = marker;
        half.where = undoublePercent(whereRaw);
        if (half.isMount) {
            QString whatRaw;
            if (extractAssignment(content, QStringLiteral("What"), &whatRaw)) {
                half.what = undoublePercent(whatRaw);
            }
        }
        halves.append(half);
    }
    return halves;
}

} // namespace

QList<OwnedUnit> enumerateOwnedUnits(uid_t uid)
{
    return pairScannedHalves(scanManagedHalves(uid));
}

QList<OwnedUnit> enumerateManagedUnits()
{
    return pairScannedHalves(scanManagedHalves(std::nullopt));
}

QList<MountEntry> parseMountinfo(const QString &content)
{
    QList<MountEntry> result;
    const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // Fields 1-6 are fixed, then zero or more optional fields, then the
        // "-" separator, then fstype/source/superopts (proc(5)).
        int sepIndex = -1;
        for (int i = 6; i < fields.size(); ++i) {
            if (fields.at(i) == QLatin1String("-")) {
                sepIndex = i;
                break;
            }
        }
        if (sepIndex < 0 || sepIndex + 2 >= fields.size() || fields.size() <= 4) {
            continue; // malformed line — skip rather than guess
        }
        MountEntry entry;
        entry.mountPoint = unescapeMountinfoField(fields.at(4));
        entry.filesystemType = fields.at(sepIndex + 1);
        entry.mountSource = unescapeMountinfoField(fields.at(sepIndex + 2));
        result.append(entry);
    }
    return result;
}

bool currentMounts(QList<MountEntry> *entries)
{
    QString content;
    if (!readMountinfoFile(&content)) {
        return false;
    }
    *entries = parseMountinfo(content);
    return true;
}

MountClassification classifyMountEntries(const QList<MountEntry> &entries, const QString &canonicalMountPoint,
                                         const QString &expectedWhat)
{
    const QString target = QDir::cleanPath(canonicalMountPoint);
    for (const MountEntry &entry : entries) {
        if (entry.mountPoint != target) {
            continue;
        }
        if (entry.filesystemType == QStringLiteral("cifs")) {
            return {MountState::Present,
                   (entry.mountSource == expectedWhat) ? VerificationState::Match : VerificationState::Mismatch};
        }
        if (entry.filesystemType == QStringLiteral("autofs")) {
            // The trigger has not been crossed yet -- the ordinary resting
            // state for an armed-but-idle share.
            return {MountState::Absent, VerificationState::NotApplicable};
        }
        // Some other filesystem entirely occupies the path: a present
        // foreign mount, not "our" CIFS mount absent.
        return {MountState::Present, VerificationState::Mismatch};
    }
    return {MountState::Absent, VerificationState::NotApplicable};
}

RuntimeSnapshot inspectRuntime(const QString &unitName, const QString &mountPoint, const QString &expectedWhat)
{
    RuntimeSnapshot snap;
    const QString mountUnitName = unitName + QStringLiteral(".mount");
    const QString automountUnitName = unitName + QStringLiteral(".automount");

    QString automountState;
    if (!systemctlShow(automountUnitName, QStringLiteral("ActiveState"), &automountState)) {
        snap.automount = AutomountState::Indeterminate;
    } else if (automountState == QStringLiteral("active")) {
        snap.automount = AutomountState::Active;
    } else if (automountState == QStringLiteral("inactive") || automountState == QStringLiteral("failed")) {
        snap.automount = AutomountState::Inactive;
    } else {
        // activating/deactivating/reloading/maintenance and any future value
        // are not stable enough to authorize a stop or removal.
        snap.automount = AutomountState::Indeterminate;
    }

    QString content;
    if (!readMountinfoFile(&content)) {
        snap.mount = MountState::Indeterminate;
        snap.verification = VerificationState::Indeterminate;
    } else {
        const MountClassification classification = classifyMountEntries(parseMountinfo(content), mountPoint, expectedWhat);
        snap.mount = classification.mount;
        snap.verification = classification.verification;

        // mountinfo (this process's own namespace) is authoritative for what is
        // really there, but cross-check PID 1's bookkeeping: a mismatch means
        // this process does not see what systemd sees — e.g. a divergent mount
        // namespace — and that must fail closed rather than silently trust
        // whichever side happened to say "nothing mounted". Failure of this
        // cross-check itself is equally Indeterminate, never a silently
        // omitted check.
        QString mountActiveState;
        if (!systemctlShow(mountUnitName, QStringLiteral("ActiveState"), &mountActiveState)) {
            snap.mount = MountState::Indeterminate;
            snap.verification = VerificationState::Indeterminate;
        } else if (mountActiveState == QStringLiteral("active")
                   || mountActiveState == QStringLiteral("inactive")
                   || mountActiveState == QStringLiteral("failed")) {
            const bool systemdSaysMounted = (mountActiveState == QStringLiteral("active"));
            const bool weSayMounted = (snap.mount == MountState::Present);
            if (systemdSaysMounted != weSayMounted) {
                snap.mount = MountState::Indeterminate;
                snap.verification = VerificationState::Indeterminate;
            }
        } else {
            snap.mount = MountState::Indeterminate;
            snap.verification = VerificationState::Indeterminate;
        }
    }

    if (snap.automount == AutomountState::Active) {
        const uint64_t recorded = readRecordedAutomountId(unitName);
        const uint64_t current = uniqueMountId(mountPoint);
        snap.activationTrust = (recorded != 0 && current != 0 && recorded == current) ? ActivationTrust::Trusted
                                                                                      : ActivationTrust::Untrusted;
    }
    return snap;
}

uint64_t uniqueMountId(const QString &path)
{
#if defined(STATX_MNT_ID_UNIQUE)
    struct statx sx {};
    if (::statx(AT_FDCWD, path.toLocal8Bit().constData(), AT_NO_AUTOMOUNT, STATX_MNT_ID_UNIQUE, &sx) == 0
        && (sx.stx_mask & STATX_MNT_ID_UNIQUE)) {
        return sx.stx_mnt_id;
    }
#else
    (void)path;
#endif
    return 0;
}

uint64_t readRecordedAutomountId(const QString &unitName)
{
    // Mirrors probeUnitFile() above: O_NOFOLLOW plus a descriptor-level
    // type/owner/writability check, so there is no window between opening
    // the name and trusting its content. /run/nasmount-ids is world-
    // readable by design, but that only ever widens who may *reach* this
    // path -- what comes back still feeds ActivationTrust, so it is
    // verified exactly like every other root-owned artifact this project
    // reads, not taken on faith just because the containing directory is
    // meant to be public.
    const QByteArray raw = (AutomountIdDir + QLatin1Char('/') + unitName).toLocal8Bit();
    const int fd = ::open(raw.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    struct stat st {};
    constexpr qint64 MaxIdFileBytes = 32; // a uint64 in decimal, plus slack
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0
        || (st.st_mode & (S_IWGRP | S_IWOTH)) || st.st_size > MaxIdFileBytes) {
        ::close(fd);
        return 0;
    }
    QFile f;
    if (!f.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return 0;
    }
    bool ok = false;
    const uint64_t id = f.readAll().trimmed().toULongLong(&ok);
    return ok ? id : 0;
}

} // namespace Verify
