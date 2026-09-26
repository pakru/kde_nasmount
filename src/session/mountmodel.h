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
 *  and the complete actionability mapping. QML never sees these booleans:
 *  rowRemoval() turns them into the one removal a row offers, and QML trusts
 *  that result rather than reproducing backend safety rules from raw role
 *  combinations. */
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

/** "Read only" | "Read & Write" | "Read & Write & Execute" — the access
 *  column's text. Must equal ShareForm's radio labels, so a share reads the
 *  same in the list as in the form that created it; mountmodel_test and
 *  shareform_qml_test pin the same three literals. */
QString accessModeLabel(UnitValue::AccessMode mode);

/** "normal" | "warning" | "error". Busy is a warning (it clears on its own
 *  or after a refresh); MissingCredentials and Broken are errors (they need
 *  the user to act). A string rather than DisplayState itself so QML never
 *  mirrors the enum's numeric values, which it could not check at build
 *  time. */
QString stateSeverity(DisplayState state);

/** Pure input to rowRemoval(). */
struct RowRemovalInput {
    bool hasStoreRecord = false;
    bool hasUnitFiles = false;
    bool canRemoveDefinition = false;
    bool canRemoveLocalRecord = false;
    bool requiresAdministrator = false;
    DisplayState state = DisplayState::Broken;
    QString detail;
};

/** The one removal a row offers, and why there is none when there is not. */
struct RowRemoval {
    /** "delete" (definition and local record), "removeOrphan" (a definition
     *  with no local record, removed by path), "removeRecord" (a local record
     *  alone), or empty when nothing can be removed right now. */
    QString kind;
    /** Set only when `kind` is empty: the disabled remove button's tooltip. */
    QString blockedReason;
};

/**
 * Maps a row's actionability to its single removal. classifyRow() never sets
 * both canRemove flags, so at most one kind ever applies — which is what lets
 * the KCM show one remove button per row that means different things on
 * different rows.
 */
RowRemoval rowRemoval(const RowRemovalInput &input);

/** Pure input to presentRow(): the facts that decide what a row shows,
 *  beyond classifyRow()'s state and detail. */
struct RowPresentInput {
    /** As in RowClassifyInput. */
    QString definitionState = QStringLiteral("none");
    DisplayState state = DisplayState::Broken;
    QString detail;
    QString unc;
    /** Meaningful only for a "pair" or "partial" definition; defaults
     *  otherwise, which presentRow() must never display. */
    UnitValue::AuthenticationKind authentication = UnitValue::AuthenticationKind::Credentials;
    UnitValue::AccessMode access = UnitValue::AccessMode::ReadWrite;
    bool hasStoreRecord = false;
    bool storeCorrupt = false;
    QString storeUsername;
    QString storeDomain;
};

/** Everything a row renders besides its removal, as display strings. */
struct RowPresentation {
    QString remoteUrl;
    QString stateText;
    QString severity;
    QString detail;
    QString access;         ///< accessModeToString(), or "" when unknown
    QString accessText;     ///< accessModeLabel(), or "" when unknown
    QString authentication; ///< "guest" | "credentials", or "" when unknown
    QString username;       ///< from Store, "" without a readable record
    QString domain;         ///< from Store, "" without a readable record
    QString section;        ///< "managed" | "foreign"
};

/**
 * The pure presentation of one row — the only place a row's displayed
 * meaning is decided, beside classifyRow(), so it can be table-tested.
 *
 * Access and authentication are shown only when the marker behind them was
 * validated, which is exactly a "pair" or "partial" definition: Tampered
 * entries reset both to their defaults, and Store-only and Foreign rows never
 * had a marker at all. Everywhere else they are "", never a defaulted
 * "Read & Write" that would read as a fact about the share.
 *
 * Username and domain come from Store — the user's own convenience record,
 * since the credential itself is root-only — and only when that record is
 * readable. A foreign mount reads "Mounted", with no detail: its section
 * header already says another tool made it.
 */
RowPresentation presentRow(const RowPresentInput &input);

class MountModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString bootHealthText READ bootHealthText NOTIFY refreshed)
    Q_PROPERTY(bool bootHealthy READ bootHealthy NOTIFY refreshed)

public:
    /**
     * Only what the KCM's delegate and its Details view actually render, each
     * a finished display value. Definition state, Store corruption, drift and
     * credential health are *inputs* to classifyRow(), presentRow() and
     * rowRemoval(), which fold them into these -- they are deliberately not
     * re-exported raw, so there is one place that decides what a row means
     * and QML never recombines raw facts into a rule of its own.
     * Authentication kind is exported, but only as Details' display value,
     * decided in presentRow() like everything else.
     */
    enum Roles {
        IdRole = Qt::UserRole + 1,
        MountPointRole,
        RemoteUrlRole, ///< the smb:// display form of the share
        StateTextRole,
        SeverityRole, ///< stateSeverity()
        DetailRole,
        /** "readwrite" | "readonly" | "readwrite-executable" from the
         *  validated marker and never from Store, or "" when no validated
         *  marker exists -- never a defaulted "readwrite". Since there is no
         *  Edit, this is the only way to discover a share's access mode short
         *  of reading its unit file. */
        AccessRole,
        AccessTextRole,     ///< accessModeLabel() of AccessRole, or ""
        AuthenticationRole, ///< "guest" | "credentials" | "" (unknown)
        UsernameRole,       ///< from Store, for Details only
        DomainRole,         ///< from Store, for Details only
        RemovalRole,        ///< RowRemoval::kind
        RemovalBlockedReasonRole,
        SectionRole, ///< "managed" | "foreign"
    };

    explicit MountModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

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
         *  for a row with no validated definition behind it; presentRow() is
         *  what keeps that default from ever being displayed. */
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
        /** Copied whenever a Store record merges, corrupt or not;
         *  presentRow() decides whether they are shown. */
        QString storeUsername;
        QString storeDomain;
        /** Computed once per refresh, after every source has been merged, so
         *  data() is a plain field read with no decisions left in it. */
        RowPresentation presentation;
        RowRemoval removal;
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
