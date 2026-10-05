# Contributing to kde_nasmount

Thanks for your interest. Bug reports, test results from other distros and
pull requests are all welcome.

## Easy ways to help

- **Try it and report back.** Install a release, mount a share, and tell us
  what worked or broke. Include your distro, Plasma version and kernel.
- **Packaging** for other distributions (AUR, openSUSE, Nix, ...).
- **Docs**: anything in the README that confused you.
- **Code**: for anything bigger than a small fix, please open an issue first
  so we can agree on the approach before you spend time on it.

## Reporting bugs

Open an issue with what you did, what you expected, and what happened.
`journalctl -u nasmount-boot.service` and `systemctl status <your>.mount`
are the useful logs. **Remove passwords, usernames and server addresses
before pasting anything.**

**Security problems:** don't open a public issue. This tool installs a root
helper, so please email the maintainer (address in the
[README](README.md)) instead.

## Building and testing

Install the build dependencies listed in the [README](README.md), then:

```bash
make              # configure and build into build/
make test         # build, then run the whole suite
```

`make test` must pass before you open a PR. CI runs it on both native
targets (Kubuntu 26.04 and Fedora KDE 44).

- **Don't `make install` on a machine you care about.** It writes system
  D-Bus and polkit files and enables a boot service. Use a VM. The privileged
  paths (KAuth, polkit, real CIFS mounts, reboot behaviour) are not covered by
  the local tests and need a VM to check.
- **Never run `install.sh` as root.** It refuses.
- `make deb` / `make rpm` build the packages in the same pinned containers CI
  uses (needs podman or docker).

## Rules that are easy to break

These aren't style preferences. Each one protects either the root helper or
existing users' shares. [AGENTS.md](AGENTS.md) explains every rule in full.

- **Library linkage is a security boundary.** `kde_nasmount-root` links only
  into `nasmount-helper` and `nasmount-boot`. Never link it into the dialog,
  KCM, session library or anything unprivileged. Configure fails if you do.
  The libraries are static on purpose.
- **Keep the helper thin.** Filesystem and systemd changes belong in
  `src/root`, not in `src/helper`. Everything in a helper argument is
  untrusted: validate again there, even if the UI already did.
- **The generated unit format is frozen.** A share's unit pair survives
  upgrades, so new code must still accept what old versions wrote. If
  `goldenunits_test` fails, revert the change. **Never regenerate
  `tests/golden/`.**
- **Changing a KAuth action** (adding, renaming, removing) breaks old front
  ends talking to a new helper. Raise it in an issue first.
- **Names:** the repository is `kde_nasmount`, everything installed is
  `nasmount`. Don't rename the installed identifiers (units, paths, KAuth id,
  marker lines). Doing so orphans existing shares.
- **No in-place edit, runtime connect/disconnect verbs, or sign-in-scoped
  mode.** They were removed on purpose; AGENTS.md says why. Changing a share
  is delete and add.
- **`ShareForm.qml` must stay host-agnostic and must not import Kirigami.**
  It is shared by the KCM and the Dolphin dialog.
- **Never print or log credentials**, and don't add a logging framework.

## Code style

- C++20, Qt 6 / KF6, KDE style: 4 spaces, braces on their own line for
  functions and attached for control flow. There is no `.clang-format`;
  match the surrounding file.
- Report errors with a `bool` return and a `QString *error` out-parameter, not
  exceptions.
- Comment the *why*, not the what. New headers and standalone `.cpp` files
  start with a short description and `SPDX-License-Identifier:
  GPL-3.0-or-later`.
- Don't reference planning documents or design notes in code, tests or
  comments. Write the reason next to the rule instead.

## Tests

C++ tests are plain `main()` binaries with a small local `check()` helper,
not QTest. Copy an existing one. A new test means editing two places:
`tests/<name>_test.cpp` and `CMakeLists.txt` (`add_executable`,
`target_link_libraries`, `add_test`).

Fix bugs with a test that fails without the fix, where that's practical.

## Pull requests

- Keep them small and focused: one change per PR.
- Explain what changed and why. Say how you tested it, and on which distro.
- Commit subjects follow the existing history: `fix: ...`, `feat: ...`,
  `chore: ...`.
- Don't bump `VERSION` or tag releases. The maintainer does that.
- Adding or removing an installed file means updating several lists together
  (CMake, the install manifest, both package definitions, uninstall). See
  "Native packages" in AGENTS.md.

## Licence

The project is **GPL-3.0-or-later**. By submitting a contribution you agree it
is licensed under the same terms.
