# Credential modes — implementation status

The original multi-phase build plan has been retired. The authoritative
implementation sequence is
[`simplification-implementation-plan.md`](simplification-implementation-plan.md),
and the consolidated current behavior is
[`credential-modes-design.md`](credential-modes-design.md).

This is a clean project: there is no migration, compatibility alias, upgrade
workflow, or automatic removal of mount-point directories.

## Implemented structure

- `src/core`: unit/path validation, marker and unit generation, definition and
  runtime inspection.
- `src/root`: descriptor-safe filesystem operations, credential/runtime stores,
  checked systemd commands, direct Define/Remove/Purge, and arming.
- `src/helper`: thin KAuth validation and dispatch under the root lock.
- `src/session`: Store/KWallet, per-user lock, helper outcome classification,
  actions, and the KCM model.
- `src/boot`: System-share boot arming.
- `src/supervisor`: Session sign-in/logout lifecycle.
- `src/cleanup`: authenticated uninstall coordination.

## Current operation order

System Add:

1. validate caller, UNC, path, and `Definition::None` under the root lock;
2. write mount unit, then automount unit;
3. write the persistent credential for authenticated shares;
4. reload systemd;
5. arm immediately and record the unique automount ID;
6. commit Store only after confirmed helper success.

Delete:

1. derive mode/identity from the validated marker;
2. prove live runtime safe to stop;
3. remove credential, unit halves, and automount ID;
4. reload systemd;
5. remove KWallet entry, then Store record.

Changing a share is Delete followed by Add.

## Completion checks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
bash -n install.sh uninstall.sh tests/removed_api_gates.sh
bash tests/removed_api_gates.sh
systemd-analyze verify build/nasmount-session.service
```

Root, reboot, polkit, real CIFS, interruption, and full-uninstall acceptance
tests belong in a disposable VM. The required scenarios are listed in
[`simplification-implementation-plan.md`](simplification-implementation-plan.md)
§6 and summarized in the design §16.
