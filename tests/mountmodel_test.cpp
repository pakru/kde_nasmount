/*
 * Table-driven tests for Session::classifyRow() and the pure presentation
 * functions beside it: rowRemoval(), presentRow(), stateSeverity() and
 * accessModeLabel().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * All of them are pure: no filesystem, systemd, or Store access, so every
 * DisplayState and every actionability combination is reachable by
 * constructing an input by hand, exactly like arming_test.cpp does for
 * evaluateArmPrecheck(). That is why the per-row presentation lives in
 * presentRow() rather than in MountModel::data(): the refresh reads the real
 * system, and no test could reach a rule left there.
 */

#include "mountmodel.h"

#include <QCoreApplication>
#include <QTextStream>

static int passed = 0;
static int failed = 0;

static void check(const QString &label, bool condition, const QString &detail = QString())
{
    QTextStream out(stdout);
    out << (condition ? "  PASS  " : "  FAIL  ") << label;
    if (!detail.isEmpty()) {
        out << "   " << detail;
    }
    out << Qt::endl;
    condition ? ++passed : ++failed;
}

namespace
{

using Session::classifyRow;
using Session::DisplayState;
using Session::RowClassifyInput;

Verify::RuntimeSnapshot inactiveSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Inactive;
    s.mount = Verify::MountState::Absent;
    return s;
}

Verify::RuntimeSnapshot armedTrustedSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Active;
    s.mount = Verify::MountState::Absent;
    s.activationTrust = Verify::ActivationTrust::Trusted;
    return s;
}

Verify::RuntimeSnapshot armedUntrustedSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Active;
    s.mount = Verify::MountState::Absent;
    s.activationTrust = Verify::ActivationTrust::Untrusted;
    return s;
}

Verify::RuntimeSnapshot mountedMatchSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Active;
    s.mount = Verify::MountState::Present;
    s.verification = Verify::VerificationState::Match;
    s.activationTrust = Verify::ActivationTrust::Trusted;
    return s;
}

Verify::RuntimeSnapshot mountedMismatchSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Inactive;
    s.mount = Verify::MountState::Present;
    s.verification = Verify::VerificationState::Mismatch;
    return s;
}

Verify::RuntimeSnapshot indeterminateMountSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Inactive;
    s.mount = Verify::MountState::Indeterminate;
    return s;
}

Verify::RuntimeSnapshot indeterminateAutomountSnapshot()
{
    Verify::RuntimeSnapshot s;
    s.automount = Verify::AutomountState::Indeterminate;
    s.mount = Verify::MountState::Absent;
    return s;
}

RowClassifyInput pairInput(UnitValue::AuthenticationKind auth, const Verify::RuntimeSnapshot &rt,
                           bool credApplicable = false, bool credHealthy = true)
{
    RowClassifyInput in;
    in.definitionState = QStringLiteral("pair");
    in.authentication = auth;
    in.runtime = rt;
    in.credentialApplicable = credApplicable;
    in.credentialHealthy = credHealthy;
    return in;
}

void checkActionability(const QString &label, const Session::RowClassification &c, bool canRemoveDefinition,
                        bool canRemoveLocalRecord, bool requiresAdministrator)
{
    check(label + QStringLiteral(": canRemoveDefinition"), c.canRemoveDefinition == canRemoveDefinition);
    check(label + QStringLiteral(": canRemoveLocalRecord"), c.canRemoveLocalRecord == canRemoveLocalRecord);
    check(label + QStringLiteral(": requiresAdministrator"), c.requiresAdministrator == requiresAdministrator);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== exact-ID Store/definition drift comparison ===" << Qt::endl;
    {
        using Session::storeDefinitionDrift;
        using Session::StoreDefinitionDriftInput;

        // The agreeing baseline every case below perturbs by exactly one
        // field, so a failure names the field that caused it.
        auto agreeing = []() {
            StoreDefinitionDriftInput in;
            in.storeUnc = QStringLiteral("//host/share");
            in.storeMountPoint = QStringLiteral("/mnt/share");
            in.storeSaysGuest = false;
            in.storeAccess = QStringLiteral("readwrite");
            in.definitionWhat = QStringLiteral("//host/share");
            in.definitionMountPoint = QStringLiteral("/mnt/share");
            in.definitionAuthentication = UnitValue::AuthenticationKind::Credentials;
            in.definitionAccess = UnitValue::AccessMode::ReadWrite;
            return in;
        };

        check(QStringLiteral("matching Store and definition are not drift"),
              !storeDefinitionDrift(agreeing()));
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeUnc = QStringLiteral("//host/share/");
            check(QStringLiteral("equivalent UNC with trailing slash is normalised"),
                  !storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeMountPoint = QStringLiteral("/mnt/other");
            check(QStringLiteral("mount-point mismatch is drift"), storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeUnc = QStringLiteral("//host/other");
            check(QStringLiteral("UNC mismatch is drift"), storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeSaysGuest = true;
            check(QStringLiteral("authentication mismatch is drift"), storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.definitionWhat = QString();
            check(QStringLiteral("automount-only Partial defers unavailable UNC comparison"),
                  !storeDefinitionDrift(in));
        }

        // Access: the marker is authoritative in both directions. A local
        // record claiming read-write for a read-only definition is exactly as
        // much drift as the reverse -- neither side is quietly believed.
        {
            StoreDefinitionDriftInput in = agreeing();
            in.definitionAccess = UnitValue::AccessMode::ReadOnly;
            check(QStringLiteral("Store readwrite vs read-only marker is drift"),
                  storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeAccess = QStringLiteral("readonly");
            check(QStringLiteral("Store readonly vs read-write marker is drift"),
                  storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeAccess = QStringLiteral("readwrite-executable");
            in.definitionAccess = UnitValue::AccessMode::ReadWriteExecutable;
            check(QStringLiteral("agreeing executable access is not drift"),
                  !storeDefinitionDrift(in));
        }
        {
            StoreDefinitionDriftInput in = agreeing();
            in.storeAccess = QStringLiteral("readonly");
            in.definitionAccess = UnitValue::AccessMode::ReadWriteExecutable;
            check(QStringLiteral("two different non-default modes are drift"),
                  storeDefinitionDrift(in));
        }
        {
            // An unreadable local record is not evidence that it agrees.
            StoreDefinitionDriftInput in = agreeing();
            in.storeAccess = QStringLiteral("bogus");
            check(QStringLiteral("unknown Store access text is drift, not defaulted"),
                  storeDefinitionDrift(in));
        }
        {
            // A record written before 0.1.4 has no Access key; Store reads it
            // back as "readwrite", which is what those shares actually are.
            // This must not look like drift or every pre-upgrade row breaks.
            StoreDefinitionDriftInput in = agreeing();
            in.storeAccess = QStringLiteral("readwrite");
            in.definitionAccess = UnitValue::AccessMode::ReadWrite;
            check(QStringLiteral("a pre-0.1.4 record's defaulted access is not drift"),
                  !storeDefinitionDrift(in));
        }
    }

    out << "=== Store-only rows (definitionState == none) ===" << Qt::endl;
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("none");
        in.hasStoreRecord = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("hasStoreRecord, well-formed -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("Store-only, well-formed"), c, false, true, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("none");
        in.hasStoreRecord = true;
        in.storeCorrupt = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("hasStoreRecord, corrupt -> Broken"), c.state == DisplayState::Broken);
        check(QStringLiteral("corrupt detail mentions missing data"), c.detail.contains(QStringLiteral("missing")),
              c.detail);
        checkActionability(QStringLiteral("Store-only, corrupt"), c, false, true, false);
    }

    out << "=== Tampered / NotOurs ===" << Qt::endl;
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("tampered");
        const auto c = classifyRow(in);
        check(QStringLiteral("Tampered, no Store record -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("Tampered, no Store record"), c, false, false, true);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("notOurs");
        in.hasStoreRecord = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("NotOurs, with Store record -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("NotOurs, with Store record"), c, false, true, true);
    }

    out << "=== Store drift / corruption on an owned Pair ===" << Qt::endl;
    {
        RowClassifyInput in = pairInput(UnitValue::AuthenticationKind::Credentials,
                                        mountedMatchSnapshot());
        in.hasStoreRecord = true;
        in.drift = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("drift always wins over a healthy runtime -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("drift"), c, false, true, false);
    }
    {
        RowClassifyInput in = pairInput(UnitValue::AuthenticationKind::Credentials,
                                        inactiveSnapshot());
        in.hasStoreRecord = true;
        in.storeCorrupt = true;
        const auto c = classifyRow(in);
        check(QStringLiteral("corrupt Store data on a Pair -> Broken"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("Pair + storeCorrupt"), c, false, true, false);
    }

    out << "=== Partial ===" << Qt::endl;
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = inactiveSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("clean Partial -> Broken, removable, never repaired"), c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("clean Partial"), c, true, false, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = mountedMismatchSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("Partial with a non-correlating live mount -> Busy, takes precedence"),
              c.state == DisplayState::Busy);
        checkActionability(QStringLiteral("Busy Partial"), c, false, false, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = indeterminateMountSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("Partial with indeterminate runtime -> Busy"), c.state == DisplayState::Busy);
        checkActionability(QStringLiteral("Indeterminate Partial"), c, false, false, false);
    }
    {
        RowClassifyInput in;
        in.definitionState = QStringLiteral("partial");
        in.runtime = armedUntrustedSnapshot();
        const auto c = classifyRow(in);
        check(QStringLiteral("Partial with an untrusted active trigger -> Broken, administrator"),
              c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("untrusted-active Partial"), c, false, false, true);
    }

    out << "=== Pair: runtime-only classification (guest, or no fresh credential data) ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Guest, inactiveSnapshot()));
        check(QStringLiteral("guest, inactive -> Inactive"), c.state == DisplayState::Inactive);
        checkActionability(QStringLiteral("guest inactive"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Guest, armedTrustedSnapshot()));
        check(QStringLiteral("guest, armed -> Armed"), c.state == DisplayState::Armed);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Guest, mountedMatchSnapshot()));
        check(QStringLiteral("guest, mounted -> Mounted"), c.state == DisplayState::Mounted);
    }
    {
        // Guest rows never have credentialApplicable set, but even if a
        // caller mistakenly passed unhealthy data, the credential rule
        // ignores it for a guest row.
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Guest,
                                             armedTrustedSnapshot(), /*credApplicable=*/false,
                                             /*credHealthy=*/false));
        check(QStringLiteral("guest ignores credential health even if somehow flagged"),
              c.state == DisplayState::Armed);
    }

    out << "=== Pair: the credential rule ===" << Qt::endl;
    {
        // The credential is persistent and root-owned, so its absence is an
        // error whether or not the trigger is currently armed.
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("inactive + missing credential -> MissingCredentials"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("armed + missing credential -> MissingCredentials"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, mountedMatchSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("mounted + missing credential -> MissingCredentials"),
              c.state == DisplayState::MissingCredentials);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        check(QStringLiteral("inactive + healthy credential -> Inactive"), c.state == DisplayState::Inactive);
    }
    {
        // No fresh inventory data this refresh (credentialApplicable still
        // false) must not be mistaken for "missing" -- the row keeps its
        // runtime-only classification.
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/false, /*credHealthy=*/false));
        check(QStringLiteral("no fresh credential data yet -> runtime-only Inactive, not guessed as missing"),
              c.state == DisplayState::Inactive);
    }

    out << "=== Pair: runtime safety always wins over credential health (rule 1) ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, armedUntrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("untrusted active + missing credential -> Broken, not MissingCredentials"),
              c.state == DisplayState::Broken);
        checkActionability(QStringLiteral("untrusted-active + missing credential"), c, false, false, true);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, indeterminateAutomountSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("indeterminate + missing credential -> Busy, not MissingCredentials"),
              c.state == DisplayState::Busy);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, mountedMismatchSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        check(QStringLiteral("non-correlating live mount + missing credential -> Busy, not MissingCredentials"),
              c.state == DisplayState::Busy);
    }

    out << "=== Pair: healthy/clean runtime states and their actionability ===" << Qt::endl;
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, inactiveSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        check(QStringLiteral("healthy inactive Session -> Inactive"), c.state == DisplayState::Inactive);
        checkActionability(QStringLiteral("healthy Inactive"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        checkActionability(QStringLiteral("healthy Armed"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, mountedMatchSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/true));
        checkActionability(QStringLiteral("healthy Mounted"), c, true, false, false);
    }
    {
        const auto c = classifyRow(pairInput(UnitValue::AuthenticationKind::Credentials, armedTrustedSnapshot(),
                                             /*credApplicable=*/true, /*credHealthy=*/false));
        checkActionability(QStringLiteral("MissingCredentials (active)"), c, true, false, false);
    }

    out << "=== Foreign is never produced by classifyRow() itself ===" << Qt::endl;
    {
        // Foreign rows are assembled directly in computeRefresh() from
        // unclaimed mountinfo entries -- classifyRow() only ever sees an
        // owned definition or a Store record, documented here so the
        // omission is not mistaken for a gap.
        check(QStringLiteral("(no case: Foreign is not reachable through classifyRow())"), true);
    }

    out << "=== accessModeLabel: the access column's exact words ===" << Qt::endl;
    {
        // The same three literals shareform_qml_test pins for the Add form's
        // radio labels: a share must read the same in the list as in the
        // form that created it.
        check(QStringLiteral("read-only label"),
              Session::accessModeLabel(UnitValue::AccessMode::ReadOnly) == QStringLiteral("Read only"));
        check(QStringLiteral("read-write label"),
              Session::accessModeLabel(UnitValue::AccessMode::ReadWrite) == QStringLiteral("Read & Write"));
        check(QStringLiteral("executable label"),
              Session::accessModeLabel(UnitValue::AccessMode::ReadWriteExecutable)
                  == QStringLiteral("Read & Write & Execute"));
    }

    out << "=== stateSeverity ===" << Qt::endl;
    {
        using Session::stateSeverity;
        check(QStringLiteral("Inactive is normal"), stateSeverity(DisplayState::Inactive) == QStringLiteral("normal"));
        check(QStringLiteral("Armed is normal"), stateSeverity(DisplayState::Armed) == QStringLiteral("normal"));
        check(QStringLiteral("Mounted is normal"), stateSeverity(DisplayState::Mounted) == QStringLiteral("normal"));
        check(QStringLiteral("Foreign is normal"), stateSeverity(DisplayState::Foreign) == QStringLiteral("normal"));
        check(QStringLiteral("Busy is a warning"), stateSeverity(DisplayState::Busy) == QStringLiteral("warning"));
        check(QStringLiteral("MissingCredentials is an error"),
              stateSeverity(DisplayState::MissingCredentials) == QStringLiteral("error"));
        check(QStringLiteral("Broken is an error"), stateSeverity(DisplayState::Broken) == QStringLiteral("error"));
    }

    out << "=== rowRemoval: the three removal kinds ===" << Qt::endl;
    {
        using Session::RowRemovalInput;
        using Session::rowRemoval;
        // Each case is one of the three visibility expressions the KCM's
        // delegate used before the decision moved here; the mapping must be
        // exactly the same.
        RowRemovalInput del;
        del.hasStoreRecord = true;
        del.hasUnitFiles = true;
        del.canRemoveDefinition = true;
        check(QStringLiteral("record + units + removable definition -> delete"),
              rowRemoval(del).kind == QStringLiteral("delete") && rowRemoval(del).blockedReason.isEmpty());

        RowRemovalInput orphan;
        orphan.hasUnitFiles = true;
        orphan.canRemoveDefinition = true;
        check(QStringLiteral("units without a record -> removeOrphan"),
              rowRemoval(orphan).kind == QStringLiteral("removeOrphan"));

        RowRemovalInput record;
        record.hasStoreRecord = true;
        record.canRemoveLocalRecord = true;
        check(QStringLiteral("record alone -> removeRecord"),
              rowRemoval(record).kind == QStringLiteral("removeRecord"));
        record.hasUnitFiles = true; // drift: the record goes first, the definition stays
        check(QStringLiteral("drifted record beside units -> removeRecord"),
              rowRemoval(record).kind == QStringLiteral("removeRecord"));

        RowRemovalInput noUnits;
        noUnits.canRemoveDefinition = true; // no units behind it: nothing to remove by definition
        check(QStringLiteral("removable definition without units offers nothing"),
              rowRemoval(noUnits).kind.isEmpty());
    }

    out << "=== rowRemoval: why nothing can be removed ===" << Qt::endl;
    {
        using Session::RowRemovalInput;
        using Session::rowRemoval;
        RowRemovalInput admin;
        admin.hasUnitFiles = true;
        admin.requiresAdministrator = true;
        admin.detail = QStringLiteral("requires administrator repair");
        check(QStringLiteral("administrator repair wins over the detail"),
              rowRemoval(admin).kind.isEmpty()
                  && rowRemoval(admin).blockedReason == QStringLiteral("Requires administrator repair"),
              rowRemoval(admin).blockedReason);

        RowRemovalInput foreign;
        foreign.hasUnitFiles = true;
        foreign.state = DisplayState::Foreign;
        foreign.detail = QStringLiteral("mounted by another tool");
        check(QStringLiteral("a foreign mount names its owner"),
              rowRemoval(foreign).blockedReason.startsWith(QStringLiteral("Mounted by another tool")),
              rowRemoval(foreign).blockedReason);

        RowRemovalInput busy;
        busy.hasStoreRecord = true;
        busy.hasUnitFiles = true;
        busy.state = DisplayState::Busy;
        busy.detail = QStringLiteral("runtime state could not be determined");
        check(QStringLiteral("a busy row carries its detail"),
              rowRemoval(busy).blockedReason
                  == QStringLiteral("Can't be removed right now: runtime state could not be determined"),
              rowRemoval(busy).blockedReason);

        RowRemovalInput bare;
        check(QStringLiteral("no detail still gives a reason"),
              rowRemoval(bare).blockedReason == QStringLiteral("Can't be removed right now"));
    }

    out << "=== rowRemoval: at most one kind for every classification ===" << Qt::endl;
    {
        // The KCM shows one remove button per row. That is only honest if
        // classifyRow() never offers both removals at once, whatever it is
        // given -- swept here over every definition state, Store condition,
        // runtime and credential case.
        const QStringList definitionStates = {QStringLiteral("pair"), QStringLiteral("partial"),
                                              QStringLiteral("tampered"), QStringLiteral("notOurs"),
                                              QStringLiteral("none")};
        const QList<Verify::RuntimeSnapshot> runtimes = {inactiveSnapshot(), armedTrustedSnapshot(),
                                                         armedUntrustedSnapshot()};
        int combinations = 0;
        bool exclusive = true;
        QString firstViolation;
        for (const QString &definitionState : definitionStates) {
            for (const Verify::RuntimeSnapshot &runtime : runtimes) {
                for (int flags = 0; flags < 32; ++flags) {
                    RowClassifyInput in;
                    in.definitionState = definitionState;
                    in.runtime = runtime;
                    in.hasStoreRecord = flags & 1;
                    in.storeCorrupt = flags & 2;
                    in.drift = flags & 4;
                    in.credentialApplicable = flags & 8;
                    in.credentialHealthy = flags & 16;
                    const auto c = classifyRow(in);
                    ++combinations;
                    if (c.canRemoveDefinition && c.canRemoveLocalRecord) {
                        exclusive = false;
                        if (firstViolation.isEmpty()) {
                            firstViolation = QStringLiteral("%1 flags=%2").arg(definitionState).arg(flags);
                        }
                    }
                }
            }
        }
        check(QStringLiteral("no classification offers both removals (%1 combinations)").arg(combinations),
              exclusive, firstViolation);
    }

    out << "=== presentRow: access and authentication only from a validated marker ===" << Qt::endl;
    {
        using Session::presentRow;
        using Session::RowPresentInput;
        const QList<QPair<UnitValue::AccessMode, QString>> modes = {
            {UnitValue::AccessMode::ReadOnly, QStringLiteral("readonly")},
            {UnitValue::AccessMode::ReadWrite, QStringLiteral("readwrite")},
            {UnitValue::AccessMode::ReadWriteExecutable, QStringLiteral("readwrite-executable")},
        };
        for (const QString &known : {QStringLiteral("pair"), QStringLiteral("partial")}) {
            for (const auto &mode : modes) {
                RowPresentInput in;
                in.definitionState = known;
                in.state = DisplayState::Armed;
                in.access = mode.first;
                in.authentication = UnitValue::AuthenticationKind::Guest;
                const auto p = presentRow(in);
                check(QStringLiteral("%1 %2: access shown").arg(known, mode.second),
                      p.access == mode.second && p.accessText == Session::accessModeLabel(mode.first)
                          && p.authentication == QStringLiteral("guest"),
                      p.access + QStringLiteral(" / ") + p.accessText);
            }
            RowPresentInput credentials;
            credentials.definitionState = known;
            credentials.authentication = UnitValue::AuthenticationKind::Credentials;
            check(QStringLiteral("%1: credentials authentication shown").arg(known),
                  presentRow(credentials).authentication == QStringLiteral("credentials"));
        }

        // The rule the access column rests on: these rows still carry the
        // input defaults (ReadWrite, Credentials), and showing them would
        // present a guess as a fact about the share.
        for (const QString &unknown : {QStringLiteral("tampered"), QStringLiteral("notOurs"), QStringLiteral("none")}) {
            RowPresentInput in;
            in.definitionState = unknown;
            in.state = DisplayState::Broken;
            const auto p = presentRow(in);
            check(QStringLiteral("%1: access and authentication blank, not defaulted").arg(unknown),
                  p.access.isEmpty() && p.accessText.isEmpty() && p.authentication.isEmpty(),
                  p.access + QStringLiteral(" / ") + p.authentication);
        }
        RowPresentInput foreign;
        foreign.state = DisplayState::Foreign;
        const auto pf = presentRow(foreign);
        check(QStringLiteral("foreign: access and authentication blank, not defaulted"),
              pf.access.isEmpty() && pf.accessText.isEmpty() && pf.authentication.isEmpty());
    }

    out << "=== presentRow: username and domain only from a readable record ===" << Qt::endl;
    {
        using Session::presentRow;
        using Session::RowPresentInput;
        RowPresentInput in;
        in.definitionState = QStringLiteral("pair");
        in.storeUsername = QStringLiteral("alice");
        in.storeDomain = QStringLiteral("WORKGROUP");
        in.hasStoreRecord = true;
        auto p = presentRow(in);
        check(QStringLiteral("readable record: username and domain shown"),
              p.username == QStringLiteral("alice") && p.domain == QStringLiteral("WORKGROUP"));
        in.storeCorrupt = true;
        p = presentRow(in);
        check(QStringLiteral("corrupt record: nothing shown"), p.username.isEmpty() && p.domain.isEmpty());
        in.storeCorrupt = false;
        in.hasStoreRecord = false;
        p = presentRow(in);
        check(QStringLiteral("no record: nothing shown"), p.username.isEmpty() && p.domain.isEmpty());
    }

    out << "=== presentRow: state text, detail and section ===" << Qt::endl;
    {
        using Session::presentRow;
        using Session::RowPresentInput;
        const QList<QPair<DisplayState, QString>> managed = {
            {DisplayState::Inactive, QStringLiteral("Inactive")},
            {DisplayState::Armed, QStringLiteral("Armed")},
            {DisplayState::Mounted, QStringLiteral("Mounted")},
            {DisplayState::MissingCredentials, QStringLiteral("Missing credentials")},
            {DisplayState::Broken, QStringLiteral("Broken")},
            {DisplayState::Busy, QStringLiteral("Busy")},
        };
        for (const auto &state : managed) {
            RowPresentInput in;
            in.state = state.first;
            in.detail = QStringLiteral("some detail");
            const auto p = presentRow(in);
            check(QStringLiteral("%1: text, detail and section").arg(state.second),
                  p.stateText == state.second && p.detail == QStringLiteral("some detail")
                      && p.section == QStringLiteral("managed") && p.severity == Session::stateSeverity(state.first),
                  p.stateText);
        }
        // Inside the "Mounted by other tools" section, "Foreign" and "mounted
        // by another tool" would both repeat the section header.
        RowPresentInput foreign;
        foreign.state = DisplayState::Foreign;
        foreign.detail = QStringLiteral("mounted by another tool");
        const auto p = presentRow(foreign);
        check(QStringLiteral("foreign: Mounted, no detail, foreign section"),
              p.stateText == QStringLiteral("Mounted") && p.detail.isEmpty() && p.section == QStringLiteral("foreign"));
    }

    out << "=== presentRow: remote address ===" << Qt::endl;
    {
        // One spot check: displayUrl() itself is covered in shareaddress_test.
        Session::RowPresentInput in;
        in.unc = QStringLiteral("//nas.local/Media Library");
        check(QStringLiteral("remoteUrl is the smb:// display form"),
              Session::presentRow(in).remoteUrl == QStringLiteral("smb://nas.local/Media Library"),
              Session::presentRow(in).remoteUrl);
    }

    out << Qt::endl << passed << " passed, " << failed << " failed" << Qt::endl;
    return failed == 0 ? 0 : 1;
}
