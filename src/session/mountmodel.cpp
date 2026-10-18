/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mountmodel.h"
#include "helperinvoke.h"
#include "shareaddress.h"
#include "store.h"
#include "unitspec.h"
#include "unitvalue.h"
#include "verify.h"

#include <KLocalizedString>

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

/** The credential rule: applicable at all only for an authenticated row with
 *  fresh health data; then an unhealthy credential blocks the row whatever
 *  its runtime state, because the credential is persistent and always
 *  expected present -- an Inactive share needs it at the next boot. */
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
        out.detail = in.storeCorrupt ? i18n("this saved record is missing required data")
                                     : i18n("no definition exists for this saved record");
        out.canRemoveLocalRecord = in.hasStoreRecord;
        return out;
    }

    if (in.definitionState == QStringLiteral("tampered") || in.definitionState == QStringLiteral("notOurs")) {
        out.state = DisplayState::Broken;
        out.detail = i18n("requires administrator repair");
        out.requiresAdministrator = true;
        out.canRemoveLocalRecord = in.hasStoreRecord;
        return out;
    }

    // Store drift/corruption is a definition-level problem, decided before
    // any runtime check, for both a Pair and a Partial: fix the local record
    // first, and only then does the owned definition naturally appear as a
    // plain removable orphan on the next refresh.
    if (in.drift || in.storeCorrupt) {
        out.state = DisplayState::Broken;
        out.detail = in.drift
            ? i18n("saved settings do not match the actual definition - remove the local record, "
                   "then remove the resulting orphan definition")
            : i18n("this share's saved convenience data is incomplete - remove the local record");
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
        out.detail = i18n("runtime state could not be determined - refresh and retry");
        return out;
    }
    if (rt.mount == Verify::MountState::Present && rt.verification != Verify::VerificationState::Match) {
        out.state = DisplayState::Busy;
        out.detail = i18n(
            "mounted, but does not correlate with this definition - release it with its owning tool, refresh, "
            "then retry");
        return out;
    }
    if (rt.mount != Verify::MountState::Present && rt.automount == Verify::AutomountState::Active
        && rt.activationTrust != Verify::ActivationTrust::Trusted) {
        out.state = DisplayState::Broken;
        out.detail = i18n(
            "automount is active, but its instance id does not match what was recorded - reboot or seek "
            "administrator help; "
            "never stopped or adopted automatically");
        out.requiresAdministrator = true;
        return out;
    }

    if (isPartial) {
        out.state = DisplayState::Broken;
        out.detail = i18n("only one half of the pair exists - remove it, it is never repaired");
        out.canRemoveDefinition = true;
        return out;
    }

    if (rt.mount == Verify::MountState::Present) {
        if (credentialUnhealthy(in)) {
            out.state = DisplayState::MissingCredentials;
            out.detail = i18n("stored credential is missing or unhealthy");
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
            out.detail = i18n("stored credential is missing or unhealthy");
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
        out.detail = i18n("stored credential is missing or unhealthy");
        out.canRemoveDefinition = true;
        return out;
    }
    out.state = DisplayState::Inactive;
    out.canRemoveDefinition = true;
    return out;
}

QString accessModeLabel(UnitValue::AccessMode mode)
{
    // Each call carries its own translator comment: the extractor attaches a
    // comment only to the call that directly follows it.
    switch (mode) {
    case UnitValue::AccessMode::ReadOnly:
        // i18n: Keep identical to the Add form's access radio button.
        return i18nc("@item:inlistbox access mode", "Read only");
    case UnitValue::AccessMode::ReadWrite:
        // i18n: Keep identical to the Add form's access radio button.
        return i18nc("@item:inlistbox access mode", "Read & Write");
    case UnitValue::AccessMode::ReadWriteExecutable:
        // i18n: Keep identical to the Add form's access radio button.
        return i18nc("@item:inlistbox access mode", "Read & Write & Execute");
    }
    return QString();
}

QString stateLabel(DisplayState state)
{
    // Foreign shares Mounted's call: one catalog entry, because it is one
    // meaning. The switch names every enumerator and has no default, so the
    // compiler warns about a new DisplayState here; allStateLabels() then
    // picks it up by itself.
    switch (state) {
    case DisplayState::Inactive:
        return i18nc("@item:inlistbox share state", "Inactive");
    case DisplayState::Armed:
        return i18nc("@item:inlistbox share state", "Ready");
    case DisplayState::Mounted:
    case DisplayState::Foreign:
        return i18nc("@item:inlistbox share state", "Mounted");
    case DisplayState::MissingCredentials:
        return i18nc("@item:inlistbox share state", "Missing credentials");
    case DisplayState::Broken:
        return i18nc("@item:inlistbox share state", "Broken");
    case DisplayState::Busy:
        return i18nc("@item:inlistbox share state", "Busy");
    }
    return QString();
}

namespace
{

// The distinct, non-empty labels of an enumeration, found by asking `label`
// about each value from zero until it has none. The enumerators are the
// consecutive values 0, 1, 2, ... and `label` returns an empty string for any
// other value (both label functions fall out of their switch to
// QString()), so the list is built from the label function itself and no
// second list of enumerators exists to fall behind it.
template<typename Enum>
QStringList labelsOf(QString (*label)(Enum))
{
    QStringList labels;
    for (int value = 0;; ++value) {
        const QString text = label(static_cast<Enum>(value));
        if (text.isEmpty()) {
            break;
        }
        // Foreign repeats Mounted's text.
        if (!labels.contains(text)) {
            labels.append(text);
        }
    }
    return labels;
}

} // namespace

QStringList allStateLabels()
{
    return labelsOf<DisplayState>(&stateLabel);
}

QStringList allAccessLabels()
{
    return labelsOf<UnitValue::AccessMode>(&accessModeLabel);
}

QString stateSeverity(DisplayState state)
{
    switch (state) {
    case DisplayState::Inactive:
    case DisplayState::Armed:
    case DisplayState::Mounted:
    case DisplayState::Foreign:
        return QStringLiteral("normal");
    case DisplayState::Busy:
        return QStringLiteral("warning");
    case DisplayState::MissingCredentials:
    case DisplayState::Broken:
        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

RowRemoval rowRemoval(const RowRemovalInput &in)
{
    RowRemoval out;
    if (in.hasStoreRecord && in.hasUnitFiles && in.canRemoveDefinition) {
        out.kind = QStringLiteral("delete");
    } else if (!in.hasStoreRecord && in.hasUnitFiles && in.canRemoveDefinition) {
        out.kind = QStringLiteral("removeOrphan");
    } else if (in.hasStoreRecord && in.canRemoveLocalRecord) {
        out.kind = QStringLiteral("removeRecord");
    } else if (in.requiresAdministrator) {
        out.blockedReason = i18nc("@info:tooltip", "Requires administrator repair");
    } else if (in.state == DisplayState::Foreign) {
        out.blockedReason = i18nc("@info:tooltip", "Mounted by another tool - unmount it with that tool");
    } else if (!in.detail.isEmpty()) {
        out.blockedReason = i18nc("@info:tooltip", "Can't be removed right now: %1", in.detail);
    } else {
        out.blockedReason = i18nc("@info:tooltip", "Can't be removed right now");
    }
    return out;
}

RowPresentation presentRow(const RowPresentInput &in)
{
    RowPresentation out;
    out.remoteUrl = ShareAddress::displayUrl(in.unc);
    out.severity = stateSeverity(in.state);
    out.detail = in.detail;
    out.stateText = stateLabel(in.state);
    out.section = QStringLiteral("managed");

    // Only Foreign differs beyond its text. Every state is still named, so a
    // new one has to be decided here rather than silently treated as managed.
    switch (in.state) {
    case DisplayState::Foreign:
        out.detail.clear();
        out.section = QStringLiteral("foreign");
        break;
    case DisplayState::Inactive:
    case DisplayState::Armed:
    case DisplayState::Mounted:
    case DisplayState::MissingCredentials:
    case DisplayState::Broken:
    case DisplayState::Busy:
        break;
    }

    const bool markerKnown =
        in.definitionState == QStringLiteral("pair") || in.definitionState == QStringLiteral("partial");
    if (markerKnown) {
        out.access = UnitValue::accessModeToString(in.access);
        out.accessText = accessModeLabel(in.access);
        out.authentication = in.authentication == UnitValue::AuthenticationKind::Guest
            ? QStringLiteral("guest")
            : QStringLiteral("credentials");
    }

    if (in.hasStoreRecord && !in.storeCorrupt) {
        out.username = in.storeUsername;
        out.domain = in.storeDomain;
    }
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

/** Queries nasmount-boot.service's own health: enabled
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
        *text = i18n("service status undefined");
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
        *text = i18n("service status undefined");
        *healthy = false;
        return;
    }
    const QString unitFileState = properties.value(QStringLiteral("UnitFileState")).trimmed();
    const QString activeState = properties.value(QStringLiteral("ActiveState")).trimmed();
    const QString result = properties.value(QStringLiteral("Result")).trimmed();
    const QString execMainStatus = properties.value(QStringLiteral("ExecMainStatus")).trimmed();

    if (unitFileState != QStringLiteral("enabled")) {
        *text = i18n("service disabled - shares will not be armed at boot");
        *healthy = false;
        return;
    }
    if (activeState == QStringLiteral("failed") || (result != QStringLiteral("success") && !result.isEmpty())
        || (execMainStatus != QStringLiteral("0") && !execMainStatus.isEmpty())) {
        *text = i18n("service failed to start - shares may not be armed (check %1)",
                     QStringLiteral("journalctl -u nasmount-boot"));
        *healthy = false;
        return;
    }
    *text = i18n("service enabled");
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
    const RowPresentation &p = row.presentation;
    switch (role) {
    case IdRole:
        return row.id;
    case MountPointRole:
        return row.mountPoint;
    case RemoteUrlRole:
        return p.remoteUrl;
    case StateTextRole:
        return p.stateText;
    case SeverityRole:
        return p.severity;
    case DetailRole:
        return p.detail;
    case AccessRole:
        return p.access;
    case AccessTextRole:
        return p.accessText;
    case AuthenticationRole:
        return p.authentication;
    case UsernameRole:
        return p.username;
    case DomainRole:
        return p.domain;
    case RemovalRole:
        return row.removal.kind;
    case RemovalBlockedReasonRole:
        return row.removal.blockedReason;
    case SectionRole:
        return p.section;
    default:
        return {};
    }
}

QHash<int, QByteArray> MountModel::roleNames() const
{
    return {
        {IdRole, "shareId"},
        {MountPointRole, "mountPoint"},
        {RemoteUrlRole, "remoteUrl"},
        {StateTextRole, "stateText"},
        {SeverityRole, "severity"},
        {DetailRole, "detail"},
        {AccessRole, "access"},
        {AccessTextRole, "accessText"},
        {AuthenticationRole, "authentication"},
        {UsernameRole, "username"},
        {DomainRole, "domain"},
        {RemovalRole, "removal"},
        {RemovalBlockedReasonRole, "removalBlockedReason"},
        {SectionRole, "section"},
    };
}

QString MountModel::bootHealthText() const
{
    return m_bootHealthText;
}

bool MountModel::bootHealthy() const
{
    return m_bootHealthy;
}

QStringList MountModel::stateLabels() const
{
    return allStateLabels();
}

QStringList MountModel::accessLabels() const
{
    return allAccessLabels();
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
    // authentication and definition state, including either-half Partial,
    // Tampered, and orphan pairs with no Store record at all -- every one
    // of them must be visible. ---------------------------------------------
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

    // ---- source 1: Store records, including corrupt groups, which must be
    // visible rather than silently skipped. A corrupt record whose
    // id nonetheless matches a real definition is not a separate "orphaned
    // record" -- it is that definition, with unreliable convenience
    // metadata; only a corrupt/None record proven to match nothing real is
    // offered "Remove record". Merged only by exact stable ID -- never a
    // path-based adoption rule. ---------------------------------------------
    for (const Store::Snapshot &snap : Store::shareSnapshots()) {
        const Store::Share &share = snap.share;
        const auto it = share.id.isEmpty() ? indexById.constEnd() : indexById.constFind(share.id);
        if (it != indexById.constEnd()) {
            // A real definition already covers this id -- merge Store's
            // convenience fields onto it, and flag Drift if Store's own
            // idea of mode/authentication disagrees with the marker
            // (the marker always wins; Store is never consulted for that
            // decision). Re-classify: drift/corruption
            // changes the outcome to Broken/local-record-only.
            Row &row = rows[*it];
            row.hasStoreRecord = true;
            row.storeUsername = share.username;
            row.storeDomain = share.domain;
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
            row.storeUsername = share.username;
            row.storeDomain = share.domain;
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
    // health that neither of the above two, unprivileged, sources can see.
    // A failure here just means health data is unavailable this refresh; the
    // definitions themselves, from sources 1-2 above, are entirely
    // unaffected. Re-classifies each matched row with the fresh
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

    // ---- source 4: unclaimed live CIFS mounts. Appended after every
    // managed row on purpose: the KCM groups rows into ListView sections by
    // presentRow()'s `section`, and a section is one block only while its rows
    // are adjacent. A mountinfo read failure here just means foreign mounts
    // cannot be discovered this refresh - it must not be mistaken for "there
    // are none", so the augmentation is skipped rather than asserting an
    // empty result. ----------------------------------------------------------
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
            row.detail = i18n("mounted by another tool");
            rows.append(row);
        }
    }

    // Presentation and removal last, after the inventory pass has had its
    // chance to re-classify, so they describe each row's final state.
    for (Row &row : rows) {
        RowPresentInput present;
        present.definitionState = row.definitionState;
        present.state = row.state;
        present.detail = row.detail;
        present.unc = row.unc;
        present.authentication = row.authentication;
        present.access = row.access;
        present.hasStoreRecord = row.hasStoreRecord;
        present.storeCorrupt = row.storeCorrupt;
        present.storeUsername = row.storeUsername;
        present.storeDomain = row.storeDomain;
        row.presentation = presentRow(present);

        RowRemovalInput removal;
        removal.hasStoreRecord = row.hasStoreRecord;
        removal.hasUnitFiles = row.hasUnitFiles;
        removal.canRemoveDefinition = row.canRemoveDefinition;
        removal.canRemoveLocalRecord = row.canRemoveLocalRecord;
        removal.requiresAdministrator = row.requiresAdministrator;
        removal.state = row.state;
        removal.detail = row.detail;
        row.removal = rowRemoval(removal);
    }

    queryBootHealth(&result.bootHealthText, &result.bootHealthy);
    return result;
}

} // namespace Session
