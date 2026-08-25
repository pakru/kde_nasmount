# Design — network mounts

This is the current design for nasmount. It describes a clean installation;
there is no migration or compatibility behavior.

The implementation intentionally supports a small lifecycle: Add and Delete.
A share is armed at boot and mounts on first access; there is no connect,
arm, disarm, mount-now or unmount-now verb. Changing a share's UNC, mount
point, credentials or authentication kind is Delete followed by Add. Mount-point directories are never removed automatically.

## 1. One lifecycle

Every share has one root-owned static `.mount`/`.automount` pair and one
lifecycle: an authenticated share's credential is a persistent root-owned
`0600` file under `/etc/nasmount`, and `nasmount-boot.service` arms the share
at boot, before login. It then mounts on first access and releases on the idle
timeout, with the trigger staying armed.

There is deliberately no per-share choice about any of that — no mode, no
"arm at sign-in" switch. Asking for a share to be mounted *is* asking for it
to be there after a reboot.

Guest shares are valid and never have a credential file.

This is a manageability feature, not encryption. Root and anyone who can read
the disk offline can read the credential. Full-disk encryption remains the
appropriate protection against offline access.

### 1.1 The removed sign-in-scoped mode

There used to be a second mode: an authenticated share whose credential lived
under `/run` only while armed, with KWallet holding the user's saved copy, and
a per-user supervisor (`nasmount-session.service`) arming it after login. It
was removed entirely, along with the supervisor binary, the KWallet
integration, and the passwordless `define`/`undefine`/`arm`/`disarm` polkit
tier that existed to avoid an authentication prompt on every login.

It is worth recording *why*, because the tradeoff it made was reasonable: it
kept the password out of any on-disk file. What it could not do is survive a
reboot unattended — it cannot arm before a user logs in and unlocks a wallet,
by construction. It also depended on the desktop's wallet being reachable,
which on a KDE system with a competing Secret Service provider is not
something this tool can guarantee.

`UnitValue::CredentialMode` still carries a `Session` value and still parses
the `session` marker, but nothing creates one and no action routes on it. That
residue is not a compatibility guarantee — this project is clean-install only
and assumes no existing deployment — it is simply not yet removed from
`nasmount-root`'s API surface (`Operations::DefineInput`, `Arming::arm`/
`disarm`). Removing it is a wanted cleanup, not a behaviour change.

## 2. Scope and non-goals

- CIFS only; no NFS, SSHFS, WebDAV, Kerberos, or custom mount options.
- No share discovery; Dolphin or another browser supplies the SMB location.
- No management of mounts or unit files created by another tool.
- No removal of mount-point directories.
- No in-place mutation of an existing definition.
- No automatic repair of an incomplete unit pair.
- No promise of byte-identical restoration after process or power loss.

## 3. Artifacts and mount mechanism

For a share with stable ID `<id>` and escaped unit name `<unit>`:

```text
/etc/systemd/system/<unit>.mount
/etc/systemd/system/<unit>.automount
/etc/nasmount/<id>.cred                 authenticated share, persistent
/run/nasmount-ids/<unit>                active-trigger identity (0644, world-readable)
```

`/run/nasmount-ids` is deliberately a sibling of `/run/nasmount`, not a
subdirectory of it: `/run/nasmount` stays `0700` to protect the credential
files directly inside it, while `/run/nasmount-ids` is `0755` so any local
process can verify a recorded instance id without needing to traverse the
credential directory to reach it.

Both units are static and have no `[Install]` section: they are started by
`nasmount-boot.service`, the only enabled unit this project installs.

The `.automount` creates an autofs trigger. The first access starts the CIFS
`.mount`; `TimeoutIdleSec=` later releases the CIFS mount while leaving the
trigger armed.

### 3.2.2 Guest shares

A guest unit uses the fixed `guest` option instead of `credentials=`. Add,
arming, inventory, Delete, and purge all derive guest status from the validated
unit marker. Caller-supplied empty fields never authorize deleting an arbitrary
credential.

## 4. Safety model

The helper treats every argument as untrusted and derives caller identity from
KAuth. Existing-share mode, authentication kind, owner UID/GID, and stable ID
come from validated root-owned markers, never from user-writable configuration.

Mount points must be below the caller's home, `/mnt`, or `/media`. The helper
walks from the allowed root with descriptor-relative operations and
`O_NOFOLLOW`, refuses mount crossings and foreign-owned components, requires the
final directory to be empty, and applies private ownership/mode.

### 4.2 Runtime correlation

Definition ownership does not prove ownership of a live mount: systemd can
associate a path-derived unit with a mount created elsewhere. Before any stop,
nasmount combines PID 1 state with `/proc/self/mountinfo`:

- a live CIFS mount must match the definition's filesystem type and `What=`;
- an active automount with no live CIFS mount must match the unique mount ID
  recorded when nasmount started it;
- failed, transitional, contradictory, or unavailable inspection is
  indeterminate and blocks mutation.

Correlation is best effort for CIFS mounts, not provenance. Automount identity
is exact because nasmount records `STATX_MNT_ID_UNIQUE` immediately after start.

## 5. Stable ID and marker

### 5.1 Stable ID

The privileged helper generates a globally unique 32-character lowercase
hexadecimal ID. The ID is used by unit markers, credential filenames and
Store. It is never supplied by the client for a new definition.

### 5.2 Marker

Both unit halves contain a complete marker recording ID, owner UID/GID, mode,
and authentication kind. The pair must agree. The unit body is validated
against a restricted template; unknown functional directives, unsafe files,
drop-ins, symlinks, marker disagreement, and name/path disagreement are refused.

## 6. Units and arming

### 6.1 Generated units

Unit values use one encoder that quotes and escapes backslashes, quotes, and
percent specifiers. UNC, paths, credential fields, IDs, and file sizes are
bounded and reject control characters. Mount options are fixed:

```text
guest | credentials=<validated path>,nosuid,nodev,forceuid,forcegid,
uid=<caller>,gid=<caller>,nounix,iocharset=utf8,file_mode=0600,dir_mode=0700
```

### 6.2 Static activation

Share units are never enabled. Arming starts the static automount immediately
after Add, and again at boot.

### 6.3 Boot coordinator

`nasmount-boot.service` runs once as root under the global root lock. It scans
all structurally valid System pairs, resolves each recorded owner, verifies the
existing mount path without creating or changing it, validates credential
health, and arms each safe inactive trigger. One share's failure is logged and
does not prevent other shares from being attempted.

An unreachable NAS does not delay boot: arming installs only the autofs trigger
and does not access the remote share.

### 6.3a Immediate System arming

System Add succeeds only after the new automount is active and its unique ID is
durably recorded. There is no “wait until reboot” success state.

### 6.4 Automount identity

Every arm inspects runtime before touching the path, starts only an inactive
trigger, captures its unique mount ID, and records that ID before reporting
success. An already-active trigger is accepted only when its current ID matches
the recorded ID. An active unrecorded or mismatched trigger is never adopted or
stopped automatically.

If a same-call failure occurs after start, nasmount re-checks the exact ID it
observed before stopping. A caller may remove a newly created definition only
after that stop is confirmed. If identity or stop cannot be confirmed, the
complete definition and credential remain so refresh can show the unsafe state.

## 7. Helper API and authorization

### 7.1 Actions

Every mutation requires administrator authentication (`auth_admin`); the
read-only inventory does not. There is no passwordless mutation tier -- see
the header of `io.github.pakru.nasmount.actions` for why one used to exist and
why its justification is gone.

```text
definesystem
undefinesystem
inventory
purge
```

Existing-share actions re-read the marker for owner, ID and authentication
kind rather than trusting any caller-supplied value.

### 7.1.2 Marker authority

Store supplies convenience data only. The marker is authoritative for mode,
authentication, owner, ID, mount point, and generated-unit identity.

### 7.1.3 Credential inventory

The privileged inventory returns only raw
`{id, credentialApplicable, credentialHealthy}` records derived from validated
markers. It does not query runtime or decide whether missing credentials are an
error.

### 7.1.8 Boot health

The KCM displays boot-coordinator health as a global banner, separate from each
share's runtime state.

### 7.3.4 Store drift

Store and definitions merge only by stable ID. A differing UNC, mount point,
mode, or authentication kind is Broken drift and permits only local-record
removal. An unmatched Store row's derived unit path is inspected before it is
called Store-only; unsafe or foreign unit files also require administrator
guidance.

### 7.4.6 Row actions

Every row is boot-managed and exposes no manual arming action. The only verbs
are Delete (a share with a local record) and Remove (an owned definition with
no record); a row needing administrator repair offers neither.

## 8. Credentials

### 8.1 System Define ordering

Creation order is:

1. mount unit;
2. automount unit;
3. persistent System credential, when authenticated;
4. `daemon-reload`;
5. immediate arm and durable automount-ID recording.

The new units are not exposed to systemd before the credential exists. An
interruption before credential creation therefore leaves discoverable unit
state instead of an unindexed secret.

Credential files and directories are opened and verified through descriptors,
written by atomic replacement, and synced. Guest operations assert absence.

## 9. Direct operations

### 9.1 Define

Define requires `Definition::None`, allocates a new ID, and writes forward in
the §8.1 order. An existing one-sided pair is rejected with “remove it first.”

Same-process compensation tracks every attempted artifact, including a write
that may have renamed successfully before a later sync error. A credential
cleanup failure retains the unit definition so the secret remains indexed. A
started trigger that cannot be proven stopped always retains the definition.

### 9.2 Remove

Remove revalidates a complete pair or owner-validated one-sided pair under the
root lock, calls the safe-stop correlation gate before deletion, then removes
credential, both unit halves, automount ID, and finally reloads systemd.
Missing artifacts are idempotent after ownership is established. Busy,
mismatched, or indeterminate runtime leaves the definition untouched.

### 9.5 Arming

Arming performs a runtime precheck, path validation, start, and ID recording.
It validates the persistent credential but never writes or rewrites it: the
credential is created once, by Add, before the unit is ever started.

## 10. Local paths and credential storage

### 10.1 Boot no-create policy

Interactive Add may create missing allowed path components and give them to the
caller. Boot never creates, owns, repairs, or changes a local mount path. A
missing path, wrong owner/group, symlink, mount crossing, or non-empty directory
leaves that share unarmed and logged.

### 10.2 Credential-store policy

Credential roots are root-owned `0700`; credential files are root-owned `0600`.
Names come only from validated stable IDs. Reads and removals reject symlinks,
wrong owners, wrong types, unsafe modes, and oversized content.

## 11. KCM state model

The model merges Store snapshots, validated unit definitions, raw privileged
credential health, and unclaimed live CIFS mounts. It classifies rows as:

- `Inactive`, `Armed`, `Mounted`, `MissingCredentials`;
- `Broken`, `Busy`, or read-only `Foreign`.

Unsafe definition/runtime state takes precedence over credentials. A missing
credential is always an error, since it is persistent and expected present
whether or not the trigger is currently armed. Guest rows ignore credential
health.

Removal actionability comes only from model booleans:

| Condition | Remove definition | Remove local record | Administrator |
|---|---:|---:|---:|
| Safe complete pair or one-sided pair | yes | no | no |
| Busy runtime | no | no | no |
| Store-only | no | yes | no |
| Store drift | no | yes | no |
| Unsafe/foreign unit with Store row | no | yes | yes |
| Unsafe/foreign unit without Store row | no | no | yes |
| Active untrusted trigger | no | no | yes |
| Foreign CIFS mount | no | no | no |

## 12. Session lifecycle

### 12.1 Sign-in

`nasmount-session.service` is always active for the graphical session. Its
supervisor arms Session shares with Reconnect enabled. It waits a bounded time
for KWallet to become unlocked but never opens a locked wallet or prompts.
Already-active Session triggers are accepted only when their recorded ID
matches.

### 12.2 Logout

`ExecStopPost` enumerates every validated Session definition owned by the user,
including one-sided pairs, and requests checked disarm. It never tears down
System shares.

## 13. Store and KWallet

Store groups and KWallet entries are keyed by stable ID. Every client mutation
runs off the GUI thread under one per-user lock. Store writes use a generation
check and explicit sync. Delete removes the wallet entry before its Store group;
if wallet cleanup cannot be confirmed, the Store record remains as the index to
the possible secret.

Unknown helper outcomes never trigger blind retries or destructive Store
updates; refresh reconciles actual unit/runtime state.

## 14. Full uninstall

Authenticated purge validates every managed definition and refuses mixed-owner,
unsafe, or busy state before deleting the first artifact. It safely stops all
validated shares, removes credentials, unit files, automount IDs, private
nasmount roots, KWallet data, configuration, and installed files. Mount-point
directories remain.

## 15. Interrupted and blocked operations

There is no startup replay. Refresh reports actual artifacts:

| Residue | Display/action |
|---|---|
| One unit half | Broken; checked Remove |
| Complete pair, missing System credential | Missing credentials; Delete |
| Active trigger without matching ID | Broken; reboot or administrator repair |
| Foreign mount on claimed path | Busy; release it with its owning tool, refresh, retry |
| Store record without definition | Broken; Remove local record |

Reboot is a fallback for a Busy mount that cannot otherwise be released, not
the first remedy.

## 16. Verification

Local tests cover unit generation for both modes/authentication kinds, runtime
classification, arm/safe-stop decision tables, Store drift, display/action
mapping, durable filesystem primitives, inventory shape, helper outcomes,
manifest validation, and removed-API gates.

Real root/systemd behavior is validated only in a disposable VM: Session and
System Add/Delete, boot without login, logout teardown, guest/authenticated
mounts, both one-sided pair shapes, interrupted creation, compensation failure,
foreign-mount refusal, and full uninstall.
