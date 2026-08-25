# KCM implementation — current architecture

> **Partly superseded.** This document predates the removal of the
> sign-in-scoped (Session) credential mode. Anything below describing two
> modes, KWallet persistence, a per-share `reconnect` switch, mode-routed
> helper actions, or the runtime verbs (Connect / Mount now / Unmount now /
> Disarm) describes a lifecycle that no longer exists — see
> [`credential-modes-design.md`](credential-modes-design.md) §1 and §1.1 for
> what replaced it and why. The privilege, locking and validation rules it
> states are still current.

`kcm_nasmount` is an immediate-action System Settings module for CIFS mounts
backed by generated static systemd mount and automount units. It shares the
same session library and privileged helper as the Dolphin service-menu dialog.

There is no staged Apply operation and no in-place change workflow. To change
UNC, mount point, credentials, authentication, or mode, Delete the share and
Add it again.

## 1. Boundaries

- The KCM, dialog, and supervisor are unprivileged and never link
  `nasmount-root`.
- Root mutations go through typed KAuth actions.
- Every client mutation runs on a worker thread under `Session::UserLock`.
- Every privileged mutation and boot scan runs under `Root::RootLock`.
- Existing-share identity and mode come from validated unit markers.
- Store and KWallet are convenience/session data, never authorization inputs.

## 2. Model inputs

`MountModel::computeRefresh()` assembles one immutable refresh result from:

1. validated Store snapshots, including corrupt groups;
2. both halves of every caller-owned managed definition;
3. raw privileged credential health keyed by stable ID;
4. unclaimed live CIFS mounts;
5. boot-coordinator health for the global banner.

Definition and Store rows merge only by exact stable ID. The validated
definition supplies actual mount point, UNC, mode, and authentication. A Store
field mismatch is drift and offers only local-record removal. Before declaring
an unmatched Store row Store-only, the model inspects its derived unit path for
unsafe or foreign unit files.

## 3. States and actionability

The pure `classifyRow()` function produces state, guidance, and three removal
booleans:

- `Inactive`, `Armed`, `Mounted`, `MissingCredentials`;
- `Broken`, `Busy`, `Foreign`;
- `canRemoveDefinition`, `canRemoveLocalRecord`,
  `requiresAdministrator`.

QML consumes those booleans directly for removal; it does not reproduce
backend safety rules.

Busy means the path/runtime is temporarily unsafe to mutate. It offers no
removal and is not an administrator condition. Release the conflicting mount
with its owning tool, refresh, and retry. An active trigger with an untrusted
ID is Broken and administrator-required.

Credential interpretation belongs only to the model:

- unhealthy System credential: `MissingCredentials` in every otherwise-safe
  runtime state;
- unhealthy Session credential: `MissingCredentials` only while active or
  mounted;
- guest: credential health ignored.

## 4. Actions

- Add Session: Define, arm, then commit Store; optional KWallet persistence is
  applied afterward.
- Add System: authenticated Define performs immediate arm, then Store commits.
- Delete: disable Session reconnect, call mode-correct checked Remove, then
  delete KWallet/Store.
- Remove local record: worker-thread KWallet/Store cleanup under the user lock;
  no privileged mutation.
- Remove orphan definition: path-based, but mode and ownership are re-derived
  from the marker before calling the helper.
- Connect/Mount now/Unmount now/Disarm: Session only.

Unknown helper results never alter Store destructively and direct the user to
refresh before retrying.

## 5. Asynchrony

KAuth, KWallet, Store I/O, systemctl inspection, and model refresh all run off
the GUI thread. Action completion emits `finished`; the KCM refreshes actual
state afterward.

## 6. Tests

- `mountmodel_test`: complete state/action table and Store/definition drift.
- `mountactions_test`: guest-field consistency and mode action routing.
- `helperinvoke_test`: confirmed success/failure/unknown classification.
- `store_test`: generation checks, corrupt records, and user-lock behavior.
- `verify_test`: mountinfo, definition safety, runtime command failure, and
  both unit-half shapes.

Real KAuth, KWallet, systemd, CIFS, boot, logout, and uninstall behavior is
validated in a disposable VM, never through privileged workstation tests.
