/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mountmodel.h"
#include "helperinvoke.h"
#include "store.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QtConcurrentRun>

#include <unistd.h>

namespace Session
{

namespace
{

QString definitionStateText(Verify::Definition state)
{
    switch (state) {
    case Verify::Definition::Pair:
        return QStringLiteral("pair");
    case Verify::Definition::Partial:
        return QStringLiteral("partial");
    case Verify::Definition::Tampered:
        return QStringLiteral("tampered");
    case Verify::Definition::NotOurs:
        return QStringLiteral("notOurs");
    case Verify::Definition::None:
        return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

/** Rule 2/3/4 of the mode-dependent credential rule (design/simplification
 *  plan §4.4): applicable at all only for an authenticated row with fresh
 *  health data; then blocks an Active/Mounted row in either mode, but an
 *  Inactive row only for System, whose credential is persistent and always
 *  expected present -- a Session row's `/run` credential is legitimately
 *  absent while inactive. */
bool credentialUnhealthy(const RowClassifyInput &in)
{
    return in.authentication == UnitValue::AuthenticationKind::Credentials && in.credentialApplicable
        && !in.credentialHealthy;
}

} // namespace

RowClassification classifyRow(const RowClassifyInput &in)
{
    RowClassification out;

    if (in.definitionState == QStringLiteral("none")) {
        out.state = DisplayState::Broken;
        out.detail = in.storeCorrupt ? QStringLiteral("this saved record is missing required data")
                                     : QStringLiteral("no definition exists for this saved record");
        out.canRemoveLocalRecord = in.hasStoreRecord;
        return out;
    }

    if (in.definitionState == QStringLiteral("tampered") || in.definitionState == QStringLiteral("notOurs")) {
        out.state = DisplayState::Broken;
        out.detail = QStringLiteral("requires administrator repair");
        out.requiresAdministrator = true;
        out.canRemoveLocalRecord = in.hasStoreRecord;
        return out;
    }

    // Store drift/corruption is a definition-level problem, decided before
    // any runtime check, for both a Pair and a Partial: fix the local record
    // first, and only then does the owned definition naturally appear as a
    // plain removable orphan on the next refresh (design §7.3.4).
    if (in.drift || in.storeCorrupt) {
        out.state = DisplayState::Broken;
        out.detail = in.drift
            ? QStringLiteral("saved settings do not match the actual definition — remove the local record, "
                             "then remove the resulting orphan definition")
            : QStringLiteral("this share's saved convenience data is incomplete — remove the local record");
        out.canRemoveLocalRecord = true;
        return out;
    }

    // definitionState is "pair" or "partial" from here. The runtime safety
    // gate applies to both (a Busy result takes precedence over an
    // otherwise-removable Partial exactly as it does for a Pair) -- only a
    // Pair continues past it into credential/Mounted/Armed/Inactive.
    const Verify::RuntimeSnapshot &rt = in.runtime;
    const bool isPartial = (in.definitionState == QStringLiteral("partial"));

    if (rt.mount == Verify::MountState::Indeterminate || rt.automount == Verify::AutomountState::Indeterminate) {
        out.state = DisplayState::Busy;
        out.detail = QStringLiteral("runtime state could not be determined — refresh and retry");
        return out;
    }
    if (rt.mount == Verify::MountState::Present && rt.verification != Verify::VerificationState::Match) {
        out.state = DisplayState::Busy;
        out.detail = QStringLiteral(
            "mounted, but does not correlate with this definition — release it with its owning tool, refresh, "
            "then retry");
        return out;
    }
    if (rt.mount != Verify::MountState::Present && rt.automount == Verify::AutomountState::Active
        && rt.activationTrust != Verify::ActivationTrust::Trusted) {
        out.state = DisplayState::Broken;
        out.detail = QStringLiteral(
            "armed, but its instance id does not match what was recorded — reboot or seek administrator help; "
            "never stopped or adopted automatically");
        out.requiresAdministrator = true;
        return out;
    }

    if (isPartial) {
        out.state = DisplayState::Broken;
        out.detail = QStringLiteral("only one half of the pair exists — remove it, it is never repaired");
        out.canRemoveDefinition = true;
        return out;
    }

    if (rt.mount == Verify::MountState::Present) {
        if (credentialUnhealthy(in)) {
            out.state = DisplayState::MissingCredentials;
            out.detail = QStringLiteral("stored credential is missing or unhealthy");
            out.canRemoveDefinition = true;
            return out;
        }
        out.state = DisplayState::Mounted;
        out.canRemoveDefinition = true;
        return out;
    }
    if (rt.automount == Verify::AutomountState::Active) {
        if (credentialUnhealthy(in)) {
            out.state = DisplayState::MissingCredentials;
            out.detail = QStringLiteral("stored credential is missing or unhealthy");
            out.canRemoveDefinition = true;
            return out;
        }
        out.state = DisplayState::Armed;
        out.canRemoveDefinition = true;
        return out;
    }

    // Inactive automount, nothing mounted: the ordinary resting state for
    // a boot-coordinator problem is surfaced by the separate global health
    // banner, not by this row. The credential is persistent, so it is
    // expected present whether or not the trigger is currently armed.
    if (credentialUnhealthy(in)) {
        out.state = DisplayState::MissingCredentials;
        out.detail = QStringLiteral("stored credential is missing or unhealthy");
        out.canRemoveDefinition = true;
        return out;
    }
    out.state = DisplayState::Inactive;
    out.canRemoveDefinition = true;
    return out;
}

bool storeDefinitionDrift(const StoreDefinitionDriftInput &input)
{
    QString normalisedStoreUnc;
    QString uncError;
    const bool uncValid = UnitSpec::validateUnc(input.storeUnc, &normalisedStoreUnc, &uncError);
    const bool markerSaysGuest =
        (input.definitionAuthentication == UnitValue::AuthenticationKind::Guest);
    const bool uncDrift =
        !input.definitionWhat.isEmpty() && (!uncValid || normalisedStoreUnc != input.definitionWhat);
    // Compared as text, against the marker mode's canonical spelling. Store
    // text outside the closed vocabulary therefore matches nothing and is
    // drift -- which is the right answer: an unreadable local record is not
    // evidence that it agrees with the marker.
    const bool accessDrift =
        input.storeAccess != UnitValue::accessModeToString(input.definitionAccess);
    return input.storeMountPoint != input.definitionMountPoint || input.storeSaysGuest != markerSaysGuest
        || uncDrift || accessDrift;
}

namespace
{

/** Queries nasmount-boot.service's own health (design §7.1.8): enabled
 *  state plus ActiveState/Result/ExecMainStatus from its last run. A system
 *  unit's read-only properties are queryable by any local user, no
 *  capability needed -- this never touches kde_nasmount-root, which
 *  kde_nasmount-session must never link. Transport failure (systemctl itself
 *  could not be reached) is kept distinct from "reachable, but disabled or
 *  failed". */
void queryBootHealth(QString *text, bool *healthy)
{
    QProcess proc;
    proc.start(QStringLiteral("systemctl"),
              {QStringLiteral("show"), QStringLiteral("nasmount-boot.service"),
               QStringLiteral("--property=UnitFileState,ActiveState,Result,ExecMainStatus")});
    if (!proc.waitForFinished(10000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        *text = QStringLiteral("boot coordinator status could not be determined");
        *healthy = false;
        return;
    }
    // systemd does not honour the order properties were requested in --
    // it emits them in its own fixed internal order regardless -- so this
    // must key off the `Name=Value` prefix rather than line position.
    QMap<QString, QString> properties;
    const QStringList lines =
        QString::fromLocal8Bit(proc.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) {
            properties.insert(line.left(eq), line.mid(eq + 1));
        }
    }
    if (!properties.contains(QStringLiteral("UnitFileState")) || !properties.contains(QStringLiteral("ActiveState"))
        || !properties.contains(QStringLiteral("Result")) || !properties.contains(QStringLiteral("ExecMainStatus"))) {
        *text = QStringLiteral("boot coordinator status could not be determined");
        *healthy = false;
        return;
    }
    const QString unitFileState = properties.value(QStringLiteral("UnitFileState")).trimmed();
    const QString activeState = properties.value(QStringLiteral("ActiveState")).trimmed();
    const QString result = properties.value(QStringLiteral("Result")).trimmed();
    const QString execMainStatus = properties.value(QStringLiteral("ExecMainStatus")).trimmed();

    if (unitFileState != QStringLiteral("enabled")) {
        *text = QStringLiteral("boot coordinator is not enabled — System-mode shares will not be armed at boot");
        *healthy = false;
        return;
    }
    if (activeState == QStringLiteral("failed") || (result != QStringLiteral("success") && !result.isEmpty())
        || (execMainStatus != QStringLiteral("0") && !execMainStatus.isEmpty())) {
        *text = QStringLiteral(
            "boot coordinator's last run failed — System-mode shares may not be armed (check journalctl -u "
            "nasmount-boot)");
        *healthy = false;
        return;
    }
    *text = QStringLiteral("boot coordinator is enabled and its last run succeeded");
    *healthy = true;
}

} // namespace

MountModel::MountModel(QObject *parent) : QAbstractListModel(parent)
{
    connect(&m_watcher, &QFutureWatcher<RefreshResult>::finished, this, [this]() {
        const RefreshResult result = m_watcher.result();
        beginResetModel();
        m_rows = result.rows;
        m_bootHealthText = result.bootHealthText;
        m_bootHealthy = result.bootHealthy;
        endResetModel();
        Q_EMIT refreshed();
    });
    refresh();
}

int MountModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant MountModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case IdRole:
        return row.id;
    case UncRole:
        return row.unc;
    case MountPointRole:
        return row.mountPoint;
    case StateTextRole:
        switch (row.state) {
        case DisplayState::Inactive:
            return QStringLiteral("Inactive");
        case DisplayState::Armed:
            return QStringLiteral("Armed");
        case DisplayState::Mounted:
            return QStringLiteral("Mounted");
        case DisplayState::MissingCredentials:
            return QStringLiteral("Missing credentials");
        case DisplayState::Broken:
            return QStringLiteral("Broken");
        case DisplayState::Busy:
            return QStringLiteral("Busy");
        case DisplayState::Foreign:
            return QStringLiteral("Foreign");
        }
        return {};
    case DetailRole:
        return row.detail;
    case HasUnitFilesRole:
        return row.hasUnitFiles;
    case HasStoreRecordRole:
        return row.hasStoreRecord;
    case DriftRole:
        return row.drift;
    case CanRemoveDefinitionRole:
        return row.canRemoveDefinition;
    case CanRemoveLocalRecordRole:
        return row.canRemoveLocalRecord;
    case RequiresAdministratorRole:
        return row.requiresAdministrator;
    case AccessRole:
        return UnitValue::accessModeToString(row.access);
    default:
        return {};
    }
}

QHash<int, QByteArray> MountModel::roleNames() const
{
    return {
        {IdRole, "shareId"},
        {UncRole, "unc"},
        {MountPointRole, "mountPoint"},
        {StateTextRole, "stateText"},
        {DetailRole, "detail"},
        {HasUnitFilesRole, "hasUnitFiles"},
        {HasStoreRecordRole, "hasStoreRecord"},
        {DriftRole, "drift"},
        {CanRemoveDefinitionRole, "canRemoveDefinition"},
        {CanRemoveLocalRecordRole, "canRemoveLocalRecord"},
        {RequiresAdministratorRole, "requiresAdministrator"},
        {AccessRole, "access"},
    };
}

bool MountModel::hasShares() const
{
    for (const Row &row : m_rows) {
        if (row.hasUnitFiles) {
            return true;
        }
    }
    return false;
}

QString MountModel::bootHealthText() const
{
    return m_bootHealthText;
}

bool MountModel::bootHealthy() const
{
    return m_bootHealthy;
}

void MountModel::refresh()
{
    m_watcher.setFuture(QtConcurrent::run(&MountModel::computeRefresh));
}

MountModel::RefreshResult MountModel::computeRefresh()
{
    RefreshResult result;
    QList<Row> &rows = result.rows;
    const uid_t uid = ::getuid();

    QMap<QString, int> indexById;
    QSet<QString> coveredMountPoints;

    // ---- source 2: both managed unit halves -- authoritative for mode,
    // authentication and definition state (design §7.1.2), including
    // either-half Partial, Tampered, and orphan pairs with no Store record
    // at all (design §7.1 gate: "orphan full pairs and either-half
    // Partials are visible"). ----------------------------------------------
    for (const Verify::OwnedUnit &unit : Verify::enumerateOwnedUnits(uid)) {
        Row row;
        row.mountPoint = unit.mountPoint;
        row.unc = unit.what;
        row.definitionWhat = unit.what;
        row.authentication = unit.authentication;
        // From the marker, via Verify -- never from the Store record merged
        // in below (AGENTS.md: properties of an existing definition are
        // always re-derived from the validated unit marker).
        row.access = unit.access;
        row.definitionState = definitionStateText(unit.state);
        row.hasUnitFiles = true;

        if (unit.state == Verify::Definition::Pair || unit.state == Verify::Definition::Partial) {
            row.runtime = Verify::inspectRuntime(unit.unitName, unit.mountPoint, unit.what);
        }

        RowClassifyInput in;
        in.definitionState = row.definitionState;
        in.authentication = unit.authentication;
        in.runtime = row.runtime;
        const RowClassification classification = classifyRow(in);
        row.state = classification.state;
        row.detail = unit.detail.isEmpty() ? classification.detail : unit.detail;
        row.canRemoveDefinition = classification.canRemoveDefinition;
        row.canRemoveLocalRecord = classification.canRemoveLocalRecord;
        row.requiresAdministrator = classification.requiresAdministrator;

        coveredMountPoints.insert(unit.mountPoint);

        if (!unit.id.isEmpty()) {
            row.id = unit.id;
            indexById.insert(unit.id, rows.size());
        }
        rows.append(row);
    }

    // ---- source 1: Store records, including corrupt groups (design
    // §7.1 gate: "corrupt Store rows are visible"). A corrupt record whose
    // id nonetheless matches a real definition is not a separate "orphaned
    // record" -- it is that definition, with unreliable convenience
    // metadata; only a corrupt/None record proven to match nothing real is
    // offered "Remove record" (design §7.3.4). Merged only by exact stable
    // ID -- never a path-based adoption rule (simplification plan §4.4
    // action 4). ------------------------------------------------------------
    for (const Store::Snapshot &snap : Store::shareSnapshots()) {
        const Store::Share &share = snap.share;
        const auto it = share.id.isEmpty() ? indexById.constEnd() : indexById.constFind(share.id);
        if (it != indexById.constEnd()) {
            // A real definition already covers this id -- merge Store's
            // convenience fields onto it, and flag Drift if Store's own
            // idea of mode/authentication disagrees with the marker
            // (design §7.1.2: the marker always wins; Store is never
            // consulted for that decision). Re-classify: drift/corruption
            // changes the outcome to Broken/local-record-only.
            Row &row = rows[*it];
            row.hasStoreRecord = true;
            if (snap.corrupt) {
                row.storeCorrupt = true;
            } else {
                StoreDefinitionDriftInput driftInput;
                driftInput.storeUnc = share.unc;
                driftInput.storeMountPoint = share.mountPoint;
                driftInput.storeSaysGuest = share.username.isEmpty();
                driftInput.storeAccess = share.access;
                driftInput.definitionWhat = row.definitionWhat;
                driftInput.definitionMountPoint = row.mountPoint;
                driftInput.definitionAuthentication = row.authentication;
                driftInput.definitionAccess = row.access;
                row.drift = storeDefinitionDrift(driftInput);
                if (!row.drift) {
                    QString normalisedStoreUnc;
                    QString ignored;
                    if (UnitSpec::validateUnc(share.unc, &normalisedStoreUnc, &ignored)) {
                        row.unc = normalisedStoreUnc;
                    }
                }
            }
            if (row.storeCorrupt || row.drift) {
                RowClassifyInput in;
                in.definitionState = row.definitionState;
                in.hasStoreRecord = true;
                in.storeCorrupt = row.storeCorrupt;
                in.drift = row.drift;
                const RowClassification classification = classifyRow(in);
                row.state = classification.state;
                row.detail = classification.detail;
                row.canRemoveDefinition = classification.canRemoveDefinition;
                row.canRemoveLocalRecord = classification.canRemoveLocalRecord;
                row.requiresAdministrator = classification.requiresAdministrator;
            }
        } else {
            // Nothing matched this Store record by stable id. Inspect the
            // unit path derived from its saved mount point before calling it
            // Store-only: a foreign/tampered file at that exact name still
            // needs administrator guidance, while a validated owned
            // definition with another id is explicit local-record drift.
            Row row;
            row.id = share.id;
            row.unc = share.unc;
            row.mountPoint = share.mountPoint;
            row.hasStoreRecord = true;
            row.storeCorrupt = snap.corrupt;
            row.definitionState = QStringLiteral("none");
            row.hasUnitFiles = false;

            if (!share.mountPoint.isEmpty()) {
                const QString canonicalStoredPath = QDir::cleanPath(share.mountPoint);
                UnitValue::UnitPaths storedPaths;
                QString pathError;
                if (UnitValue::unitPathsFor(canonicalStoredPath, &storedPaths, &pathError)) {
                    const Verify::DefinitionCheck storedDefinition =
                        Verify::inspectDefinition(storedPaths, uid, canonicalStoredPath);
                    if (storedDefinition.state == Verify::Definition::Tampered
                        || storedDefinition.state == Verify::Definition::NotOurs) {
                        row.definitionState = definitionStateText(storedDefinition.state);
                        row.hasUnitFiles = true;
                    } else if (storedDefinition.state == Verify::Definition::Pair
                               || storedDefinition.state == Verify::Definition::Partial) {
                        row.definitionState = definitionStateText(storedDefinition.state);
                        row.hasUnitFiles = true;
                        row.drift = true; // same path, different stable id
                        row.authentication = storedDefinition.authentication;
                        // Marker-sourced here too. Without this the row would
                        // report the default read-write for a definition that
                        // is not.
                        row.access = storedDefinition.access;
                        row.mountPoint = storedDefinition.canonicalMountPoint;
                        row.definitionWhat = storedDefinition.what;
                        if (!storedDefinition.what.isEmpty()) {
                            row.unc = storedDefinition.what;
                        }
                    }
                }
            }

            RowClassifyInput in;
            in.definitionState = row.definitionState;
            in.authentication = row.authentication;
            in.hasStoreRecord = true;
            in.storeCorrupt = snap.corrupt;
            in.drift = row.drift;
            const RowClassification classification = classifyRow(in);
            row.state = classification.state;
            row.detail = classification.detail;
            row.canRemoveDefinition = classification.canRemoveDefinition;
            row.canRemoveLocalRecord = classification.canRemoveLocalRecord;
            row.requiresAdministrator = classification.requiresAdministrator;

            if (!share.id.isEmpty()) {
                indexById.insert(share.id, rows.size());
            }
            rows.append(row);
            if (!share.mountPoint.isEmpty()) {
                coveredMountPoints.insert(share.mountPoint);
            }
        }
    }

    // ---- source 3: caller-scoped privileged inventory -- raw credential
    // health that neither of the above two, unprivileged, sources can see
    // (design §7.1.3). A failure here just means health data is unavailable
    // this refresh; the definitions themselves, from sources 1-2 above, are
    // entirely unaffected. Re-classifies each matched row with the fresh
    // credential facts folded in. ------------------------------------------
    const HelperResult inventoryResult = invokeHelperAction(QStringLiteral("inventory"), {});
    if (inventoryResult.outcome == HelperOutcome::ConfirmedSuccess) {
        const QByteArray json = inventoryResult.data.value(QStringLiteral("shares")).toString().toUtf8();
        const QJsonArray shares = QJsonDocument::fromJson(json).array();
        for (const QJsonValue &value : shares) {
            const QJsonObject o = value.toObject();
            const QString id = o.value(QStringLiteral("id")).toString();
            const auto it = id.isEmpty() ? indexById.constEnd() : indexById.constFind(id);
            if (it == indexById.constEnd()) {
                continue;
            }
            Row &row = rows[*it];
            row.credentialApplicable = o.value(QStringLiteral("credentialApplicable")).toBool();
            row.credentialHealthy = o.value(QStringLiteral("credentialHealthy")).toBool();

            if (row.storeCorrupt || row.drift) {
                continue; // already Broken on Store grounds; credential health cannot un-block it
            }
            RowClassifyInput in;
            in.definitionState = row.definitionState;
            in.authentication = row.authentication;
            in.runtime = row.runtime; // reuse source 2's snapshot; never re-queried
            in.credentialApplicable = row.credentialApplicable;
            in.credentialHealthy = row.credentialHealthy;
            const RowClassification classification = classifyRow(in);
            row.state = classification.state;
            if (!classification.detail.isEmpty()) {
                row.detail = classification.detail;
            }
            row.canRemoveDefinition = classification.canRemoveDefinition;
            row.canRemoveLocalRecord = classification.canRemoveLocalRecord;
            row.requiresAdministrator = classification.requiresAdministrator;
        }
    }

    // ---- source 4: unclaimed live CIFS mounts. A mountinfo read failure
    // here just means foreign mounts cannot be discovered this refresh — it
    // must not be mistaken for "there are none" (plan §1.4.1), so the
    // augmentation is skipped rather than asserting an empty result. --------
    QList<Verify::MountEntry> mounts;
    if (Verify::currentMounts(&mounts)) {
        for (const Verify::MountEntry &entry : mounts) {
            if (entry.filesystemType != QStringLiteral("cifs") || coveredMountPoints.contains(entry.mountPoint)) {
                continue;
            }
            Row row;
            row.unc = entry.mountSource;
            row.mountPoint = entry.mountPoint;
            row.state = DisplayState::Foreign;
            row.detail = QStringLiteral("mounted by another tool");
            rows.append(row);
        }
    }

    queryBootHealth(&result.bootHealthText, &result.bootHealthy);
    return result;
}

} // namespace Session
