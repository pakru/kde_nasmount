# Contributing to kde_nasmount

Thanks for your interest. Bug reports, test results from other distros and
pull requests are all welcome.

## Easy ways to help

- **Try it and report back.** Install a release, mount a share, and tell us
  what worked or broke. Include your distro, Plasma version and kernel.
- **Packaging** for other distributions (AUR, openSUSE, Nix, ...).
- **Docs**: anything in the README that confused you.
- **Translations**: add a language or improve one. No programming needed;
  see [Translating](#translating).
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

## Translating

Every message the two front ends show lives in one gettext catalog per
language, `po/<lang>/kcm_nasmount.po`, built from the template
`po/kcm_nasmount.pot`. Six languages ship: Russian (`ru`), German (`de`),
French (`fr`), Spanish (`es`), Brazilian Portuguese (`pt_BR`) and Simplified
Chinese (`zh_CN`). The German, French, Spanish, Brazilian Portuguese and
Chinese catalogs were written without a native speaker's review, so fixes to
their wording are especially welcome. A few texts sit outside the catalog and
are translated inline in four files, which the checklist below names.
Messages produced by the privileged helper and by the shared validation code
are English for now and are not in the catalog.

### Adding a language

`<lang>` is a locale name: `de`, `pt_BR` or `sr@latin`. Anything else is
refused at configure time.

1. Create the catalog and translate it:

   ```bash
   mkdir po/<lang>
   msginit --no-translator -l <lang> -i po/kcm_nasmount.pot -o po/<lang>/kcm_nasmount.po
   ```

   A `msgctxt` such as `@action:button` says where a text appears. A note
   starting `i18n:` is from the developer. Keep every `%1`, `%2` placeholder;
   they may change order. Paths, addresses and command lines are passed in as
   placeholders on purpose, so they cannot be altered.
2. Add `Name[<lang>]`, and `Comment[<lang>]`, `GenericName[<lang>]` and
   `Keywords[<lang>]` wherever the English key exists, to
   `servicemenu/nasmount.desktop.in` and `packaging/nasmount-manager.desktop`.
   The service menu has two `Name=` lines and both need a translation.
3. Add `"Name[<lang>]"`, `"Description[<lang>]"` and `"X-KDE-Keywords[<lang>]"`
   to `src/kcm/kcm_nasmount.json`.
4. Add `Name[<lang>]` and `Description[<lang>]` to every action in
   `io.github.pakru.nasmount.actions`. The description is the sentence the
   authentication prompt shows. **No Name or Description may contain a comma,
   semicolon, double quote or backslash.** The policy generator reads the file
   as settings and warns about nothing: a comma empties the whole value, a
   semicolon cuts it there, and quotes and backslashes are dropped.
5. `make test`. The translations gate checks steps 1 to 4: every catalog
   compiles with `msgfmt --check`, which rejects a lost placeholder, and no
   metadata file lacks the new language.
6. Nothing else needs editing: the install manifest, the uninstaller and both
   packages find the new catalog by themselves. With podman or docker,
   `make packages` builds both native packages; CI then checks each package's
   contents against the install manifest, the new catalog included. Commit the
   new files.

To improve an existing language, edit its `.po` file and run `make test`.

### When the text in the code changes

Wrapping a message in `i18n()`/`i18nc()` is the developer's part; the rules
for writing one (whole sentences, no concatenation, identifiers as
placeholders) are in AGENTS.md under "Translations". Only the front ends
translate: configure refuses to link KI18n into the privileged targets, and
`tests/removed_api_gates.sh` refuses translation calls in `src/core`,
`src/root`, `src/helper`, `src/boot` and `src/packageguard`.

After adding or changing a message, run

```bash
make pot
```

It rewrites `po/kcm_nasmount.pot` from the sources and merges the result into
every language, marking changed messages fuzzy. Commit the result: the suite
fails when the template is out of date, and that includes a changed translator
note (`// i18n:`) on a message whose text did not change. A catalog may lag
behind the template; a message it lacks is shown in English. Needs `xgettext`, `msgcat` and
`msgmerge` from `gettext`. Changing an English message invalidates every
translation of it, so fix wording on purpose and in batches.

### Trying a translation without installing it

Build, then copy the compiled catalog to where KDE looks for per-user ones, and
run the Dolphin dialog with the language set:

```bash
make
mkdir -p ~/.local/share/locale/<lang>/LC_MESSAGES
cp build/locale/<lang>/LC_MESSAGES/kcm_nasmount.mo ~/.local/share/locale/<lang>/LC_MESSAGES/
LANGUAGE=<lang> build/bin/nasmount-dialog smb://host/share
```

The dialog shows the same form as the System Settings page, from the same
catalog. This route does not exercise the System Settings page itself (its
list rows, column widths and window title), which is best checked on an
installed build, in a VM. When you are done, remove
`~/.local/share/locale/<lang>/LC_MESSAGES/kcm_nasmount.mo` (and the
directories above it, if you created them); copy the file again after every
rebuild.

**The locale must be a real UTF-8 one.** Under `C` and `C.UTF-8`, which
containers use, glibc ignores `LANGUAGE`, and the text stays English however
good the catalog is. Check with `locale`: if the messages locale is `C`,
`POSIX` or `C.UTF-8`, export `LC_ALL=en_US.UTF-8` (or another UTF-8 locale that
`locale -a` lists) first. For the same reason the CTest entry `i18n`, which
loads the Russian catalog from `build/`, runs under `en_US.UTF-8` and is
reported as skipped wherever that locale is not installed, as in the
package-build containers.

### Removing a language

Delete `po/<lang>` and its keys in the four metadata files, then **delete
`build/`** (`make clean`). The build installs the whole of `build/locale`, so
a compiled catalog left there by the removed language would still be
installed.

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
