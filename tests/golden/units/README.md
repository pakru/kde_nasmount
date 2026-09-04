# Frozen unit corpus

Byte-for-byte records of what a released nasmount version wrote into
`/etc/systemd/system` for a share. Read by
[`goldenunits_test.cpp`](../goldenunits_test.cpp).

These files are **evidence, not fixtures.** A share's unit pair survives a
package upgrade, so the binaries in the new package must still accept bytes an
older package wrote. Generation and validation share the same fixed-value
functions in `UnitSpec`, so a change to one changes both together and the rest
of the suite stays green while every share already on a user's disk becomes
`Tampered` — which silently unarms it at the next boot.

If `goldenunits` fails, the format drifted. The fix is to revert the change, or
to design a migration and raise `MIN_UPGRADABLE_VERSION` in
`packaging/debian/nasmount.preinst.in`. **Never regenerate these files to make
the test pass** — that deletes the only record of what users actually have.

Add a new `v<version>/` directory when a release deliberately changes the
format, and register it in `Corpora` in the test. Each entry there names its
own fixture set, because a release freezes whatever bytes it actually changed
— not necessarily every share shape. Never delete a directory for a version
users may still be upgrading from.

The fixture inputs (marker, UNC, mount point, uid/gid) that produced each file
are recorded in the test source, not here, so that a generation change cannot
redefine its own expected output.

`v0.1.0/` covers 0.1.0 through 0.1.3: `git diff v0.1.0 v0.1.3 -- src/core` is
empty, so all four wrote identical bytes.

`v0.1.4/` records **only** the read-only and read-write-executable variants,
because 0.1.4 did not change the default read-write bytes. That is not an
omission — it is the invariant the access-mode feature was built on.
`X-Nasmount-Access` is optional on read and omitted on write when the value is
read-write, so a read-write share's units are still byte-identical to what
0.1.0–0.1.3 wrote and stay frozen in `v0.1.0/`. If `v0.1.0/` ever fails, that
rule was implemented wrong; the bug is in the code, not in this directory.
