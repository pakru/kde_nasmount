/*
 * mountmodel — the merged view for the KCM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Merges, per refresh: Store records (including corrupt groups); both
 * managed unit halves, including either-half Partial, Tampered and orphan
 * (no Store record) pairs; the caller-scoped privileged `inventory` raw
 * credential health; and unclaimed live CIFS mounts nobody here defined at
 * all. The root-owned marker's mode/authentication are always authoritative;
 * a Store row that disagrees is flagged Broken drift, never silently trusted.
 *
 * Whether a credential problem makes a row unusable is decided only here, in
 * classifyRow() — never by the privileged helper, which returns only raw
 * {id, credentialApplicable, credentialHealthy} facts. Everything here is read-only. Three of the four sources need no
 * capability at all (unit files are world-readable; mountinfo/statx need no
 * capability); the fourth, `inventory`, is a passwordless KAuth round trip.
 * All of it, including that KAuth call, runs on a QtConcurrent worker
 * thread, never the GUI thread.
 */

#pragma once

#include "unitvalue.h"
#include "verify.h"

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QString>
#include <QVariantMap>

namespace Session
{

/**
 * The seven practical presentation states, uniform for every share; the
 * separate boot-coordinator health banner is what explains an Inactive
 * share.
 */
enum class DisplayState { Inactive, Armed, Mounted, MissingCredentials, Broken, Busy, Foreign };

/** Pure input to classifyRow() -- everything it needs to decide a row's
 *  DisplayState and actionability, with no filesystem/systemd access of its
 *  own. Exposed for table-driven testing (tests/mountmodel_test.cpp). */
struct RowClassifyInput {
    /** "pair" | "partial" | "tampered" | "notOurs" | "none". */
    QString definitionState = QStringLiteral("none");
    UnitValue::AuthenticationKind authentication = UnitValue::AuthenticationKind::Credentials;
    /** Meaningful only when definitionState is "pair" or "partial". */
    Verify::RuntimeSnapshot runtime;
    /** Only meaningful when authentication == Credentials; both false for a
     *  guest row or when inventory has no fresh data this refresh. */
    bool credentialApplicable = false;
    bool credentialHealthy = true;
    bool hasStoreRecord = false;
    bool storeCorrupt = false;
    /** Store's own mode/guest-status disagrees with the validated marker. */
    bool drift = false;
};

/** Pure output of classifyRow(): the display state, its explanatory detail,
 *  and the complete actionability mapping -- QML trusts these three booleans directly
 *  rather than reproducing backend safety rules from raw role combinations. */
struct RowClassification {
    DisplayState state = DisplayState::Broken;
    QString detail;
    bool canRemoveDefinition = false;
    bool canRemoveLocalRecord = false;
    bool requiresAdministrator = false;
};

/**
 * The pure row classifier. Priority order: an unsafe definition (Tampered/NotOurs/Store
 * drift or corruption/Partial) or an unsafe runtime correlation
 * (Indeterminate, a non-correlating live mount, an untrusted active trigger)
 * is decided first and always wins over credential health; only once the
 * definition and runtime are both safe does the mode-dependent credential
 * rule select MissingCredentials, and only then does plain runtime state
 * select Inactive/Armed/Mounted. No filesystem, systemd, or Store access --
 * fully unit-testable.
 */
RowClassification classifyRow(const RowClassifyInput &input);

/**
 * Inputs to the exact-ID Store/definition drift comparison.
 *
 * A named struct rather than a positional parameter list: the comparison has
 * grown past the point where six or eight same-typed arguments in a row can
 * be read (or called) safely, and every future comparison field should be a
 * local addition here rather than another signature migration across every
 * call site.
 */
struct StoreDefinitionDriftInput {
    QString storeUnc;
    QString storeMountPoint;
    bool storeSaysGuest = false;
    /** Store's recorded access spelling. Never trusted; only compared. */
    QString storeAccess = QStringLiteral("readwrite");
    /** Empty for an automount-only Partial, which has no validated What=. */
    QString definitionWhat;
    QString definitionMountPoint;
    UnitValue::AuthenticationKind definitionAuthentication = UnitValue::AuthenticationKind::Credentials;
    UnitValue::AccessMode definitionAccess = UnitValue::AccessMode::ReadWrite;
};

/** Pure comparison used by the exact-ID Store/definition merge. The root
 *  definition remains authoritative; any differing canonical mount point,
 *  normalised UNC, authentication kind, or access mode is local-record drift.
 *  An automount-only Partial has no validated What=, so UNC
 *  comparison is deferred until a mount half exists.
 *
 *  Store access text outside the closed vocabulary differs from every valid
 *  marker mode and so becomes drift, rather than being quietly defaulted to
 *  read-write and matching. */
bool storeDefinitionDrift(const StoreDefinitionDriftInput &input);

class MountModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool hasShares READ hasShares NOTIFY refreshed)
    Q_PROPERTY(QString bootHealthText READ bootHealthText NOTIFY refreshed)
    Q_PROPERTY(bool bootHealthy READ bootHealthy NOTIFY refreshed)

public:
    /**
     * Only what a delegate actually renders. Authentication kind, definition
     * state, Store corruption and credential health are *inputs* to
     * classifyRow(), which folds them into stateText/detail/drift -- they are
     * deliberately not re-exported raw, so there is one place that decides
     * what a row means and the two front ends cannot disagree about it.
     */
    enum Roles {
        IdRole = Qt::UserRole + 1,
        UncRole,
        MountPointRole,
        StateTextRole,
        DetailRole,
        HasUnitFilesRole,
        HasStoreRecordRole,      ///< whether an id (and so the id-based actions) applies to this row
        DriftRole,               ///< Store disagrees with the marker on authentication or access
        CanRemoveDefinitionRole,   ///< Delete is offered
        CanRemoveLocalRecordRole,  ///< "Remove local record" is offered
        RequiresAdministratorRole, ///< Tampered/NotOurs/untrusted-active -- never casually actionable
        /** "readwrite" | "readonly" | "readwrite-executable", always from the
         *  validated marker and never from Store. Since there is no Edit,
         *  this is the only way to discover a share's access mode short of
         *  reading its unit file. */
        AccessRole,
    };

    explicit MountModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** Whether at least one row has unit files -- gates the boot-health
     *  banner, which is global health and only noise when there is nothing
     *  for boot to arm. */
    bool hasShares() const;
    QString bootHealthText() const;
    bool bootHealthy() const;

public Q_SLOTS:
    /**
     * Re-reads Store, both unit halves, privileged inventory, any uncovered
     * CIFS mounts, and nasmount-boot.service's own health.
     *
     * Runs the actual inspection (Store, systemctl, mountinfo, one KAuth
     * round trip — all blocking I/O) on a worker thread via QtConcurrent,
     * never the GUI thread. Calling refresh() again before a
     * previous call has completed simply retargets the single
     * QFutureWatcher at the new future; Qt only ever delivers finished()
     * for the future a watcher is *currently* assigned to, so a slower,
     * now-stale refresh cannot land after and overwrite a newer one: each
     * refresh publishes one immutable result covering every source.
     */
    void refresh();

Q_SIGNALS:
    void refreshed();

private:
    struct Row {
        QString id;
        QString unc;
        QString mountPoint;
        QString definitionWhat; ///< validated .mount What=; empty for automount-only Partial
        UnitValue::AuthenticationKind authentication = UnitValue::AuthenticationKind::Credentials;
        /** From the validated marker, never from Store. Left at the default
         *  for a row with no validated definition behind it. */
        UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite;
        QString definitionState = QStringLiteral("none");
        /** Computed once, from source 2, and reused when source 3's fresh
         *  credential health triggers re-classification -- never re-queried. */
        Verify::RuntimeSnapshot runtime;
        DisplayState state = DisplayState::Broken;
        QString detail;
        bool hasUnitFiles = true; ///< false only for a Broken row backed by no unit at all
        bool hasStoreRecord = false;
        bool storeCorrupt = false;
        bool drift = false;
        bool credentialApplicable = false;
        bool credentialHealthy = true;
        bool canRemoveDefinition = false;
        bool canRemoveLocalRecord = false;
        bool requiresAdministrator = false;
    };

    struct RefreshResult {
        QList<Row> rows;
        QString bootHealthText;
        bool bootHealthy = true;
    };

    /** The blocking inspection pass. Must not touch `this` — it runs on a
     *  QtConcurrent worker thread, not the GUI thread. */
    static RefreshResult computeRefresh();

    QList<Row> m_rows;
    QString m_bootHealthText;
    bool m_bootHealthy = true;
    QFutureWatcher<RefreshResult> m_watcher;
};

} // namespace Session
