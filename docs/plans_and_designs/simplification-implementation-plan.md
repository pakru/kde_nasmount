# nasmount simplification implementation plan

Status: **implemented locally; privileged VM acceptance remains**  
Scope: simplify the current implementation before adding more features  
Repository baseline: the tree containing `transaction.*`, automatic Partial
repair, tombstones, transaction-aware inventory, and the full `replace()` mode
matrix

This plan implements the three agreed simplifications:

1. remove the generic transaction and crash-recovery engine;
2. remove automatic repair and forensic cleanup of pathological states;
3. remove atomic in-place Edit across mode, credential, path, and runtime
   transitions.

It is deliberately a reduction plan, not another redesign. The desired product
after this work is still a KDE front end for safe, static systemd mount and
automount units. It supports Add, Connect/arm, Mount now, Unmount now, Disarm,
Delete, boot-time System shares, guest shares, and full uninstall. To change a
share's UNC, mount point, credentials, or mode, the user deletes it and adds it
again.

The Dolphin service-menu dialog is already Add-only and remains so. It requires
only regression verification through the retained Add API, not a separate
redesign.

## 1. Why this work is justified

The three targeted features currently spread through most layers of the
project:

| Complexity source | Direct implementation | Important dependants |
|---|---|---|
| Generic transactions and recovery | `src/root/transaction.{h,cpp}`, transaction branches in `operations.cpp` and `arming.cpp` | boot coordinator, helper recovery action, root inventory, purge, four test binaries |
| Pathological-state automation | Partial repair in Define, `forget`, tombstones, recovery affordances | helper, runtime files, boot, MountActions, MountModel, QML |
| Full in-place Edit matrix | `Operations::replace()`, `replace`/`replacesystem`, two client edit methods | KAuth policy, Store/password sequencing, edit dialog, operation tests |

The current directly affected files total several thousand lines. Removing the
features end to end is expected to delete well over 1,500 lines and, more
importantly, remove the cross-layer recovery and transition contracts that make
small changes risky.

## 2. Guarantees that remain mandatory

Simplification must not remove the comparatively small safeguards that prevent
real data loss or privilege mistakes:

- strict UNC, mount-point, credential-field, and generated-unit validation;
- descriptor-based mount-point walking and `O_NOFOLLOW` protection;
- root ownership/mode checks for managed files;
- a strict managed marker and restricted unit template;
- one per-user lock around Store/KWallet/client operations;
- one root lock around each privileged operation and boot-coordinator run;
- separate action IDs and policies for Session and System operations;
- mode and ownership derived from validated unit markers for existing shares;
- atomic individual file replacement and checked file deletion;
- checked, bounded `systemctl` calls and `daemon-reload`;
- runtime correlation before stopping a live mount or automount;
- an automount instance ID that must be recorded before an arm operation reports
  success;
- no credential file for guest shares;
- Session credentials under `/run`, System credentials under `/etc`;
- System shares armed immediately after Add and by `nasmount-boot` at boot;
- structured `ConfirmedSuccess` / `ConfirmedFailure` / `Unknown` helper outcomes;
- mount-point directories are never removed automatically.

These guarantees remain in `unitspec.*`, `unitvalue.*`, `verify.*`,
`durablefs.*`, `rootlock.*`, `userlock.*`, `credentialstore.*`,
`systemdops.*`, and the correlation portion of `arming.*`.

## 3. Explicitly reduced guarantees and non-goals

The following are intentional product boundaries, not future TODOs hidden by
the refactor:

- no byte-identical rollback after a process crash or power loss;
- no durable per-operation manifests and no startup replay;
- no automatic repair of a one-half (`Partial`) unit pair;
- no `forget` operation that deletes a definition while a non-correlating live
  mount remains loaded;
- no tombstones or later tombstone reconciliation;
- no in-place change of UNC, path, credentials, authentication kind, or
  Session/System mode;
- no attempt to preserve armed state across a failed reconfiguration;
- no protection against a concurrent administrator editing root-owned files;
  normal unprivileged callers remain fully constrained;
- no migration from an earlier build. This remains a clean-install project.

When an operation is interrupted, the next inventory refresh reports the files
that actually exist. A complete valid pair remains usable. A validated Partial
pair is reported as Broken and can only be removed. Tampered or `NotOurs` unit
state requires administrator repair. A non-correlating foreign mount occupying a
managed path is instead reported as Busy: nasmount does not stop it, but directs
the user to unmount or disconnect it through the tool/session that created it,
refresh, and then Delete normally. Reboot is only a fallback when that foreign
mount cannot otherwise be released.

An active nasmount automount whose instance ID cannot be trusted is a different
case: it is never stopped or adopted automatically, and the UI directs the user
to reboot or seek administrator help.

The accepted interrupted-operation residues are explicit:

| Interrupted operation | Possible observed state | Supported next action |
|---|---|---|
| Define, after one unit half | owned Partial | show Broken; Remove |
| System Define, after the pair but before credential/arm | Pair with missing credential, or inactive orphan Pair | show Broken/inactive orphan; Remove |
| Session arm, after runtime credential but before start | inactive Pair with a stale `/run` credential | Connect may overwrite it; Disarm/Delete/logout removes it |
| Any arm, after start but before ID recording | active trigger with untrusted identity | do not stop or bless it; reboot or administrator repair |
| Disarm, after stop but before credential/ID cleanup | inactive Pair with stale runtime data | retry Disarm/Delete; cleanup is idempotent |
| Delete, after only part of artifact removal | owned Partial or Store-only row | retry Remove for Partial; Store-only removal may leave an inactive systemd cache entry until the next daemon-reload/reboot |

No background process converts these states. Inventory reports them; the user
chooses the documented next action.

## 4. Target architecture

### 4.1 Root operations

`Root::Operations` has only three public mutations:

- `define(const DefineInput &)`: create a new definition only;
- `remove(const RemovalInput &)`: safely stop and remove a Pair or an
  owner-validated Partial;
- `purge(uid_t)`: uninstall-only removal using the same validation and safe-stop
  rules.

There is no `ReplaceInput`, `ReplaceOutput`, `replace()`, `RecoverySummary`, or
`recoverAll()`.

`DefineInput` loses `isPartialRepair`, `existingShareId`, and
`existingAuthentication`. Define requires `Verify::Definition::None` and always
allocates a new helper-owned ID.

`RemovalInput` loses `isForget`; `RemovalOutput` loses transaction-specific
`leftPendingUnsafeToStop`. A busy, mismatched, or indeterminate runtime produces
a normal checked failure and leaves the definition untouched for retry or manual
repair.

### 4.2 Arming

`Root::Arming` retains `safeStop()` and standalone Session/System arm/disarm
functions. It no longer accepts `Transaction::Handle`, writes activation phases,
or exposes `recoverPending()`.

Each arm follows a small same-call sequence:

1. inspect runtime before touching the mount path;
2. validate the credential state and mount point;
3. for Session credentials, write the `/run` credential;
4. start the automount;
5. capture and durably record the unique mount ID;
6. report success only after step 5 succeeds.

If a later step fails in the same process, stop what this call started and remove
the credential/ID with checked best-effort compensation. Compensation failure is
reported; it does not create a recovery manifest. A crash in the start/record
window may leave an active untrusted trigger. The next refresh must show it as
Broken/Needs administrator rather than blessing or stopping it.

### 4.3 Definition health

Keep the internal `Verify::Definition` values because they cheaply and safely
detect real disk state, but reduce their behavior:

- `Pair`: normal operations are allowed;
- `Partial`: no repair; show Broken; allow only checked removal when the
  surviving marker proves caller ownership;
- `Tampered` or `NotOurs`: show Broken/administrator-required; no mutation;
- `None`: no root definition exists; a Store-only record may be removed locally.

### 4.4 Credential-health ownership

`Root::Inventory` is only the privilege boundary for reading credential files.
It derives owner, stable ID, mode, and authentication kind from validated unit
markers, reads the corresponding credential location, and returns only raw
`{id, credentialApplicable, credentialHealthy}` facts. It does not inspect
runtime state, call systemd, carry `What=`, or decide whether a missing
credential is currently an error.

`Session::MountModel` owns that interpretation because it already obtains the
definition and `Verify::RuntimeSnapshot`. Its pure state classifier applies this
precedence:

1. an unsafe definition or runtime-correlation result remains Broken/Busy;
2. for an otherwise valid credential-backed Pair, an unhealthy System
   credential means MissingCredentials in every otherwise-safe runtime state;
3. an unhealthy Session credential means MissingCredentials only while its
   automount or matching mount is active; absence while inactive is expected;
4. guest rows ignore credential health;
5. otherwise runtime selects Inactive, Armed, or Mounted.

This rule is never split between the helper and model.

### 4.5 User-visible operations

The KCM exposes:

- Add;
- Connect, Mount now, Unmount now, and Disarm where applicable;
- Delete for normal Pair rows;
- Remove broken definition for an owner-validated Partial;
- Remove local record for Store-only or drifted Store rows; an owned definition
  left behind then appears as an orphan and is removed separately by path;
- administrator guidance for Tampered, NotOurs, or untrusted-active rows.

The KCM does not expose Edit, Forget, or Recover. The Add dialog is Add-only and
states: “To change a network mount, remove it and add it again.”

## 5. Implementation sequence

Each phase below must build and pass its focused tests before the next phase
starts. Do not combine all phases into one unreviewable patch.

### Phase 0 — lock the simplified contract in documentation

Files:

- `docs/credential-modes-design.md`
- `docs/credential-modes-implementation-plan.md`
- `docs/kcm-implementation-plan.md`
- `README.md`

Actions:

1. Mark the generic recovery table, automatic Partial repair, tombstones,
   in-place Edit matrix, **and the credential-first System Define ordering in
   `credential-modes-design.md` §8.1** as superseded by this plan. Update the
   creation sequence that repeats it in design §9.1, and supersede the matching
   credential-first instruction in
   `credential-modes-implementation-plan.md` §3.2.
2. Replace that ordering rule with the direct-operation sequence used in Phase
   3: mount unit → automount unit → persistent System credential →
   `daemon-reload` → arm. State explicitly that this ordering is valid because
   nasmount does not expose the new units to systemd before the credential
   exists, while an interrupted pre-credential operation remains discoverable
   through the unit files.
3. Add the reduced failure contract from §§2–3 above.
4. Replace every user-facing Edit instruction with Delete and Add again.
5. Remove power-loss failpoint recovery and mode-transition matrix cases from
   the release gate; add interrupted-operation inventory cases instead.
6. Keep the clean-install and mount-directory-retention decisions unchanged.

Gate: a reviewer can state exactly what happens after a failed or interrupted
Add/Delete without consulting the old transaction design. Every occurrence of
“credential first” or “before any unit file” in active documentation is either
removed or explicitly labelled as superseded historical behavior.

### Phase 1 — remove in-place Edit end to end

Files:

- `src/kcm/ui/main.qml`
- `src/kcm/ui/ShareEditDialog.qml` (rename to `ShareAddDialog.qml`)
- `src/session/mountactions.{h,cpp}`
- `src/helper/helper.cpp`
- `src/root/operations.{h,cpp}`
- `src/root/transaction.{h,cpp}`
- `io.github.pakru.nasmount.actions`
- `tests/mountactions_test.cpp`
- `tests/operations_test.cpp`
- `tests/transaction_test.cpp`

Actions:

1. Remove the Edit button and `openForEdit()` path from QML.
2. Make the dialog Add-only; remove `editingId`, `originalMode`, mode-transition
   behavior, password-preservation placeholders, and Save branching. Re-run
   CMake after the rename so `kcmutils_add_qml_kcm()` refreshes its generated
   QML resource list; the current `src/kcm/CMakeLists.txt` has no explicit file
   entry to edit.
3. Remove `MountActions::editShare()`, `editSystemShare()`, and
   `isMountAffecting()`.
4. Remove helper slots `replace` and `replacesystem`, `doReplace()`, and
   `targetMode` parsing.
5. Remove `ReplaceInput`, `ReplaceOutput`, `Operations::replace()`, and all
   replace-specific backup/re-arm helpers.
6. Because this is a clean-install project, remove `Operation::Replace`, its
   replace-only manifest fields/artifacts, and `recoverPendingReplace()` from
   the still-temporary transaction engine. Do not retain a parser or replay path
   for an old Replace manifest.
7. Remove the `replace` and `replacesystem` KAuth actions.
8. Delete replace/mode-transition tests. The symbol gate below is the assertion
   that no Edit entry point remains; do not add a QML test framework solely for
   this deletion.

Gate:

```bash
rg -n 'editShare|editSystemShare|isMountAffecting|doReplace|ReplaceInput|ReplaceOutput|recoverPendingReplace|replacesystem|Operations::replace|Operation::Replace' src tests io.github.pakru.nasmount.actions
```

returns no matches, and Add/Delete still work in both modes.

### Phase 2 — remove automatic repair, Forget, and tombstones

Files:

- `src/helper/helper.cpp`
- `src/root/operations.{h,cpp}`
- `src/root/transaction.{h,cpp}`
- `src/root/runtimefiles.{h,cpp}`
- `src/root/durablefs.{h,cpp}` (remove tombstone/manifest-only comments)
- `src/core/verify.h` (update the runtime-ID writer/remover cross-reference)
- `src/boot/main.cpp`
- `src/supervisor/supervisor.cpp`
- `src/session/mountactions.{h,cpp}`
- `src/session/mountmodel.{h,cpp}`
- `src/session/store.{h,cpp}`
- `src/kcm/ui/main.qml`
- `io.github.pakru.nasmount.actions`
- `tests/operations_test.cpp`
- `tests/transaction_test.cpp`
- `tests/arming_test.cpp`
- `tests/store_test.cpp`

Actions:

1. Change `doDefine()` to accept only `Definition::None`; a Partial returns a
   clear “broken definition must be removed first” error.
2. Remove Partial-repair fields and branches from `DefineInput` and `define()`.
3. Keep Partial detection in `verify.*`, but route it only to Broken display and
   checked removal.
4. Remove `RemovalInput::isForget` and all Forget branches from `remove()`.
5. Remove helper slots/actions `forget`, `forgetsystem`, and
   `sweeptombstones`.
6. Remove `MountActions::forgetShare()` and `forgetOrphanByPath()` and their QML
   buttons. Rename the unrelated local primitives `Store::forgetShare()` to
   `Store::removeShare()` and `Store::forgetPassword()` to
   `Store::removePassword()` so “Forget” no longer names either ordinary
   KConfig/KWallet deletion operation; update their callers and Store tests
   mechanically.
7. Delete tombstone APIs and storage from `runtimefiles.*`; retain only automount
   ID read/write/removal behavior. As a separate mechanical dead-code cleanup,
   delete the declaration and definition of `forgetAutomountId()`—the current
   tree has zero callers and already uses checked `removeAutomountId()`
   everywhere. Update `verify.h`'s stale cross-reference accordingly; no caller
   migration or behavior change is required.
8. Delete tombstone sweeping from `nasmount-boot` and from both the start and
   stop paths of `nasmount-supervisor`.
9. Remove `Operation::Forget`, its manifest fields, and its recovery cases from
   the temporary transaction engine. Remove `partialRepair` fields and recovery
   branches at the same time. No compatibility parser is required.
10. For a busy, mismatched, or indeterminate Delete, return failure without
   removing units, credentials, IDs, or Store data.
11. Ensure full purge also aborts on these unsafe runtimes; it must not recreate
    Forget semantics.

Gate:

```bash
rg -n 'tombstone|sweeptombstones|forgetShare|forgetPassword|forgetOrphanByPath|forgetsystem|Operation::Forget|isPartialRepair|partialRepair|forgetAutomountId' src tests io.github.pakru.nasmount.actions
```

returns no matches. A Partial is visible and removable but never repairable.

### Phase 3 — replace transaction-backed operations with direct checked operations

Files:

- `src/root/arming.{h,cpp}`
- `src/root/operations.{h,cpp}`
- `src/root/inventory.{h,cpp}`
- `src/root/durablefs.{h,cpp}` (remove transaction-only comments)
- `src/root/transaction.{h,cpp}` (delete)
- `src/boot/main.cpp`
- `src/supervisor/supervisor.cpp`
- `src/helper/helper.cpp`
- `src/session/mountactions.{h,cpp}`
- `src/session/mountmodel.{h,cpp}`
- `src/kcm/ui/main.qml`
- `src/cleanup/main.cpp` only if its message still mentions recovery
- `CMakeLists.txt`
- `install.sh`
- `uninstall.sh`
- `tests/transaction_test.cpp` (delete)
- arming, operations, inventory, and model tests

Actions:

1. Write direct `define()`:
   - under the root lock, revalidate `Definition::None`, input/name/path
     agreement, and absence of a loaded-unit or mounted-path collision, then
     allocate an ID;
   - atomically write the mount half, then the automount half;
   - for authenticated System mode, atomically write the credential only after
     both halves exist; for guest mode, verify no credential exists;
   - run checked `daemon-reload`;
   - immediately arm System mode;
   - return success only if all required forward steps complete.
   This order is deliberate: nasmount does not reload or start the new unit
   before the credential exists. Every pre-credential crash is therefore
   visible as Partial or MissingCredentials instead of leaving an invisible
   credential-only secret. Concurrent actions by a root administrator remain
   outside the supported concurrency contract. Update the `DefineInput` and
   `define()` comments in `src/root/operations.h` and the corresponding comments
   in `operations.cpp` in the same patch; they currently cite design §8.1 and
   mandate credential-first ordering.
2. On same-call Define failure, perform only safe, checked compensation for
   artifacts this call knows it created. Track those artifacts and any freshly
   observed automount instance only in memory. If this call started a trigger,
   re-check that exact instance before stopping it. Delete created units and
   credentials only after that stop succeeds; otherwise leave the definition
   intact and visible. Remove its ID and reload systemd when the corresponding
   cleanup step is safe. Do not restore byte backups or create a persistent
   recovery record. Always return the original failure plus any compensation
   failure and let inventory show the remaining state.
3. Write direct `remove()`:
   - revalidate Pair/owned Partial under the root lock;
   - pass `safeStop()` before any deletion. For an automount-only Partial,
     require the mount path to be unoccupied and stop an active automount only
     when its recorded instance ID matches; for a mount-only Partial, use its
     validated `What=` for normal correlation;
   - remove credential, both unit halves, automount ID, then reload systemd;
   - make missing artifacts idempotent where the operation already has authority;
   - report failure without claiming rollback.
4. Rewrite standalone Session and System arm/disarm functions as specified in
   §4.2; delete `armWithinTransaction()`, `armShareWithinTransaction()`, and
   `recoverPending()`.
5. Remove `recoverBeforeActing()`, helper `recover`, the Recover policy action,
   `MountActions::recoverPending()`, and the Recover button. Remove the startup
   `recover` invocation from `nasmount-supervisor` as well.
6. Remove startup recovery from `nasmount-boot`; boot now validates actual unit
   pairs and attempts to arm only valid System pairs.
7. Remove transaction enumeration/merging from `Root::Inventory` and replace it
   with the smallest direct owned-unit mapping needed to keep the current JSON
   consumer building. Leave presentation/state cleanup to Phase 4.
8. Remove the transaction preflight and transaction-directory deletion from
   `Operations::purge()`; keep its global ownership, structural validation,
   safe-stop, credential, unit, runtime-ID, and private-root cleanup.
9. Remove `transaction.cpp` from `nasmount-root`, delete `transaction_test`, and
   remove its CMake test target and the `transaction_test` name in `install.sh`.
10. Remove every reference to the former transaction directory under
    `/etc/nasmount` from documentation and lifecycle code. Change
    `uninstall.sh`'s “recovery records” wording to the remaining concrete
    credential/runtime artifacts. No migration or cleanup compatibility path is
    added.

Gate:

```bash
rg -n 'Transaction::|transaction\.(h|cpp)|transaction_test|recoverAll|recoverPending' src tests CMakeLists.txt install.sh uninstall.sh
rg -n -g '!simplification-implementation-plan.md' '/etc/nasmount/transactions|recovery records' README.md docs src install.sh uninstall.sh
```

returns no matches.

### Phase 4 — simplify inventory and KCM states

Files:

- `src/root/inventory.{h,cpp}`
- `src/session/mountmodel.{h,cpp}`
- `src/kcm/ui/main.qml`
- `CMakeLists.txt`
- `install.sh`
- `tests/inventory_test.cpp`
- new `tests/mountmodel_test.cpp`

Actions:

1. Replace `Inventory::ShareRecord` with a narrow credential-health record
   containing only stable ID, `credentialApplicable`, and `credentialHealthy`.
   `buildFor(uid)` still derives IDs and credential mode from
   `Verify::enumerateOwnedUnits(uid)`; it never trusts those values from the
   caller.
2. Remove duplicated mount point, unit name, mode, authentication, pair detail,
   runtime correlation, pending-only records, and pending-operation fields from
   the privileged inventory JSON. `MountModel` already obtains definition and
   runtime facts through `Verify` and currently does not consume the inventory's
   `runtimeCorrelation` field. Delete `ShareRecord::what`,
   `runtimeCorrelation`, `verificationStr()`, and the `Verify::inspectRuntime()`
   call from `Root::Inventory`. Delete `enrichWithHealth()` and populate raw
   credential records directly in `buildFor()` while iterating validated owned
   units.
3. Merge the narrow raw credential result into model rows by validated stable
   ID, then apply the complete mode/runtime rule from §4.4 only in
   `MountModel`'s pure classifier. Do not return a derived MissingCredentials or
   runtime state from the helper.
4. Merge Store and definition rows only by exact stable ID; do not invent a
   path-based adoption rule. Any ID/path/mode/auth mismatch is Broken drift and
   gets only “Remove local record.” After that local removal, the validated
   owned definition naturally appears as an orphan row and can be removed by
   path on the next refresh. Before calling an unmatched Store row “Store-only,”
   inspect its derived unit path: `Tampered`/`NotOurs` also gets administrator
   guidance, while local Store removal remains safe because it does not mutate
   the foreign root artifact.
5. Extract the row-to-display-state decision into a small pure function in
   `mountmodel.{h,cpp}` and cover it with table-driven
   `tests/mountmodel_test.cpp`. Add the test target to `CMakeLists.txt` and the
   explicit test loop in `install.sh`. Do not introduce a new state framework.
6. Collapse presentation into these practical states:
   - `Inactive`: valid Pair, no active trigger or mount;
   - `Armed`: trusted active automount, no CIFS mount;
   - `Mounted`: correlating CIFS mount;
   - `MissingCredentials`: persistent System credential missing/unhealthy, or an
     active Session trigger without its required runtime credential;
   - `Broken`: Partial, Tampered, NotOurs, Store drift/corruption, or an active
     automount trigger without a matching recorded ID;
   - `Busy`: a claimed row whose path is occupied by a non-correlating live
     mount;
   - `Foreign`: an unclaimed live CIFS mount, retained as a read-only
     informational row with no nasmount actions.
7. Remove `hasPendingTransactions`, `PendingTransactionRole`,
   `PendingOperationRole`, and transaction-oriented detail text.
8. Replace `AdminOnlyRole` with the explicit booleans
   `canRemoveDefinition`, `canRemoveLocalRecord`, and
   `requiresAdministrator`; do not make QML reproduce backend safety rules.
   The pure classifier supplies this complete mapping:

   | Row condition | Remove definition | Remove local record | Requires administrator | Guidance |
   |---|---:|---:|---:|---|
   | correlation-safe owned Pair (`Inactive`, `Armed`, `Mounted`, or `MissingCredentials`) | yes | no | no | normal Delete |
   | correlation-safe owned Partial | yes | no | no | remove broken definition; it is never repaired |
   | `Busy` claimed path/runtime | no | no | no | release/unmount the conflicting use with the tool that owns it, refresh, then retry |
   | Store-only with `Definition::None` | no | yes | no | remove local record |
   | drifted/corrupt Store record associated with an owned definition | no | yes | no | remove local record; remove the resulting orphan definition after refresh |
   | `Tampered`/`NotOurs`, no Store record | no | no | yes | administrator repair |
   | `Tampered`/`NotOurs` with a Store record | no | yes | yes | local record may be removed, but the root artifact still needs administrator repair |
   | active trigger with untrusted identity | no | no | yes | reboot or administrator repair; never stop or adopt it automatically |
   | unclaimed `Foreign` CIFS mount | no | no | no | informational only; manage it with its owning tool |

   A busy result is therefore a temporary user-resolvable block, not an
   authorization problem. If a Pair or Partial otherwise permits removal but
   its runtime becomes Busy, the Busy row takes precedence and disables removal.
9. Keep boot-coordinator health as a separate global banner; it is useful and
   independent of transaction complexity.

Gate: each state above has a pure table-driven test, and QML action visibility is
driven by those three actionability booleans, not combinations of five raw roles.
The following returns no matches:

```bash
rg -n 'hasPendingTransactions|PendingTransactionRole|PendingOperationRole|pendingTransaction|pendingOperation|AdminOnlyRole|adminOnly' src tests
rg -n 'runtimeCorrelation|verificationStr|inspectRuntime|\bwhat\b' src/root/inventory.h src/root/inventory.cpp
```

### Phase 5 — simplify uninstall around the reduced backend

Files:

- `src/root/operations.cpp`
- `src/cleanup/main.cpp`
- `src/session/cleanupvalidation.{h,cpp}`
- `uninstall.sh`
- `tests/cleanupvalidation_test.cpp`
- `tests/operations_test.cpp`

Actions:

1. Retain the single authenticated `purge` action and user-side cleanup
   coordinator; this is already a narrow, understandable boundary.
2. Remove only transaction/tombstone-specific purge steps.
3. Keep strict complete manifest validation, caller ownership checks, malformed
   marker refusal, safe-stop checks, credential/ID cleanup, KWallet folder
   removal, config removal, and installed-file removal.
4. Keep purge two-pass: validate ownership, structure, and safe-stop feasibility
   for every Pair and owner-validated Partial before deleting the first artifact;
   then perform direct checked removals. An I/O failure during the second pass
   is reported without rollback, leaves the helper installed, and is retried
   from actual inventory.
5. Continue leaving mount-point directories untouched.
6. Preserve fail-closed behavior: if any definition is unsafe or belongs to
   another user, abort while the helper remains installed.

Gate: full purge succeeds for ordinary Pair and owned Partial definitions and
aborts for a busy/mismatched/Tampered definition; no transaction or tombstone
path is used.

### Phase 6 — remove stale tests and add reduced-contract coverage

Delete or rewrite tests that enforce removed behavior; do not mechanically keep
them by recreating the same complexity under new names.

Required local/unprivileged tests:

1. Pair generation produces the same strict units for Session/System and
   credentials/guest.
2. Pure arm/safe-stop decision tables refuse busy, correlation-mismatch,
   indeterminate, and active-untrusted runtime states.
3. The pure display classifier covers every Phase-4 state, including the
   different credential rule for inactive Session and System rows. Include at
   least: inactive Session + missing → Inactive; active Session + missing →
   MissingCredentials; inactive System + missing → MissingCredentials; guest +
   missing → runtime-derived state; untrusted active + missing → Broken. Cover
   every actionability row in Phase 4 action 8, including Busy →
   `false/false/false` plus user-resolution guidance.
4. Exact-ID merge, ID/path drift, Store-only, NotOurs, and both Partial shapes
   have table-driven inventory/model coverage.
5. Store is changed only after confirmed helper success; Unknown triggers a
   refresh and does not blindly retry.
6. Manifest validation and full-purge input behavior remain covered.
7. The Phase 1–4 `rg` gates run in CI so deleted APIs cannot silently return.

Required privileged VM tests:

1. authenticated and guest Session Add → arm → access → idle/unmount → logout;
2. authenticated and guest System Add → immediate arm → reboot without login →
   access;
3. Add over either Partial shape is rejected with “remove first”; it is never
   repaired.
4. Delete for both modes;
5. force a checked failure after the new pair is written and verify same-call
   compensation removes the units/credential, reloads systemd, and still
   reports the original failure;
6. force compensation itself to fail and verify the remaining state is visible
   as Broken and is never reported as success;
7. deliberately interrupt unit-pair creation and verify Broken + Remove, with no
   automatic repair;
8. interrupt arm after start but before ID recording and verify the trigger is
   untrusted, not stopped, and not automatically adopted;
9. delete both Partial shapes, and refuse Tampered/NotOurs definitions;
10. delete while a foreign mount causes correlation mismatch and verify no
    artifact is removed; unmount it through its owning tool, refresh, and verify
    normal Delete then succeeds;
11. Session logout disarms Session shares; System shares remain boot-managed;
12. delete and Add again as the supported replacement for Edit/mode change;
13. authenticated full uninstall, with mount-point directories retained.

Use the existing injectable `SystemdOps` runner for systemd failures. For the two
process-interruption cases, a narrowly scoped test-only pause point is acceptable
in the privileged VM test build; it must not create an installed API, manifest,
on-disk state, general failpoint framework, or production branch matrix.

Explicitly remove the old “power loss at every transaction failpoint” and full
replace-matrix test requirements.

### Phase 7 — final documentation and release gate

Files:

- `README.md`
- `docs/credential-modes-design.md`
- `docs/credential-modes-implementation-plan.md`
- `docs/kcm-implementation-plan.md`
- comments throughout touched source files

Actions:

1. Remove stale claims about transactions, rollback, recovery, Partial repair,
   tombstones, Forget, and Edit.
2. Document the supported Delete-and-Add workflow prominently.
3. Document interrupted-operation behavior with this troubleshooting order:
   - for Busy caused by a foreign mount, unmount/disconnect it with its owning
     tool or session, refresh the KCM, and retry Delete;
   - if the foreign mount cannot be released, reboot, then refresh and retry;
   - reserve administrator repair guidance for Tampered/NotOurs artifacts or an
     active nasmount trigger whose instance identity cannot be trusted.
4. Update architecture diagrams and action tables to the reduced action set.
5. Remove phase/history comments from production code where they no longer
   explain a current invariant.

Release validation:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
bash -n install.sh uninstall.sh
systemd-analyze verify build/nasmount-session.service
```

Also stage an install and verify that the complete manifest allowlist still
matches every installed target. Verify `nasmount-boot.service` after installing
the staged tree into the privileged test VM; checking its build-tree copy on the
host produces a false missing-`ExecStart` diagnostic because the configured
libexec binary has not been installed there.

## 6. Sequencing and review rules

- Do not begin Phase 3 until Edit and tombstones are gone; otherwise transaction
  removal has too many callers and becomes another broad rewrite.
- Do not simplify `verify.*` before Phase 4. Its current Pair/Partial/Tampered
  detection is the safety net while backend behavior is being removed.
- Do not remove unique-ID correlation merely because transaction recovery is
  removed; it is the authorization gate for stopping a live automount.
- Do not combine Store/KWallet simplification with this work. Generation/CAS,
  structured helper outcomes, and user-lock scope solve separate concurrency
  problems and can be reconsidered only after this reduction is stable.
- Every phase must leave the tree buildable. Prefer one reviewable commit per
  phase, with Phase 3 allowed to use separate arming, operations, inventory, and
  deletion commits if each intermediate commit builds.
- Measure success by deleted contracts and reduced branching, not by recreating
  recovery behavior in a new “reconcile” framework.

## 7. Completion criteria

The simplification is complete when:

- `src/root/transaction.{h,cpp}` and `tests/transaction_test.cpp` no longer
  exist;
- no transaction, recovery, tombstone, Forget, Replace, or in-place Edit API is
  present;
- Add/Delete are the only definition mutations;
- Partial is detected but never repaired;
- Tampered/NotOurs/untrusted-active state is visible and never automatically
  modified;
- `operations.cpp`, `arming.cpp`, `helper.cpp`, `mountactions.cpp`, and
  `mountmodel.cpp` have materially fewer branches and no backup/replay matrix;
- privileged inventory contains no runtime inspection or runtime-correlation
  field; the model is the sole owner of the mode-dependent credential rule;
- all retained guarantees in §2 have tests or a privileged integration case;
- all local tests and the reduced privileged VM suite pass;
- README and all three design/implementation documents describe the simpler
  product without referring readers back to superseded recovery behavior.
