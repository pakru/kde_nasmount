/*
 * shareaddress — the user-facing spelling of a share: turning an smb:// URL
 * or a //host/share UNC into the validated UNC the rest of the tool uses,
 * and a UNC back into the smb:// address people see.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * smb:// is only ever a spelling for display and input. The unit's What=, its
 * Description= line, Store's `unc` key and the helper's `unc` argument all
 * stay //host/share: mount.cifs requires that form, the unit body is a
 * frozen on-disk format, and a running old front end must keep working
 * against a new helper (and the reverse). Conversion therefore happens once,
 * on the client, before anything is sent or saved.
 *
 * This lives in the session library and never in core. Core is linked into
 * the privileged helper; keeping the parser out of every library the helper
 * links is what guarantees, structurally, that the helper can never be handed
 * an smb:// value and accept it. removed_api_gates.sh enforces the placement.
 *
 * None of it is a security boundary — the helper re-validates every field —
 * but a wrong UNC here means mounting the wrong share.
 */

#pragma once

#include <QString>

namespace Session::ShareAddress
{

/** Turns an smb:// URL into a CIFS UNC path, or reports why it cannot.
 *  Rejects ports, passwords, queries, fragments, IPv6 hosts, server-only
 *  URLs and control characters. `user` receives the URL's user, if any. */
bool parseSmbUrl(const QString &raw, QString *unc, QString *user, QString *error);

/** A username split into its domain and bare-name halves. */
struct Identity {
    QString domain;
    QString username;
};

/**
 * Splits `DOMAIN\user` or `DOMAIN/user` exactly as KDE does
 * (SMBUrl::splitDomainUser): the first separator wins, one at position 0 does
 * not split, and a UPN is left whole. Both sides of an identity comparison go
 * through it, so `DOMAIN\alice` and `alice` are not taken for two accounts.
 */
Identity splitDomainUser(const QString &combined);

/**
 * Whether two usernames name the same account: bare names compared
 * case-insensitively, as SMB names are, and domains compared only when both
 * sides have one. The same rule the credential autofill applies when it
 * refuses a candidate for another account, so the Add form's URL-user check
 * and the autofill agree about who is who.
 */
bool sameIdentity(const QString &a, const QString &b);

/**
 * The display form of a UNC: "//host/a b" -> "smb://host/a b".
 *
 * Spaces and every other character stay literal, as Dolphin's location bar
 * shows them, except `%`, `#` and `?`, which are percent-encoded. Those three
 * are exactly the ones QUrl::toDisplayString() keeps encoded, because a
 * literal one would be read back as an escape, a fragment or a query — so
 * without this an address copied from the list could not be pasted into Add.
 * Built by hand rather than through QUrl, which would lowercase the host;
 * the display keeps the host exactly as the unit wrote it.
 *
 * Anything not starting with exactly "//" is returned unchanged: a foreign
 * mount's source comes straight from mountinfo, and is shown as found.
 */
QString displayUrl(const QString &unc);

/**
 * The Add field's input, in either spelling, resolved to a validated,
 * normalised UNC.
 *
 * `//host/share` is validated as is; `smb://…` is parsed first. Anything else
 * is refused. When the smb:// address names a user, `username` (the Username
 * field) must name the same account: an empty one is refused, because the
 * form already fills Username from the address, so an empty field here means
 * the user cleared it deliberately — and filling it silently at submit would
 * create an account share whose password was never entered.
 */
bool resolveShareInput(const QString &input, const QString &username, QString *unc, QString *error);

/** The user an smb:// input names, or empty (no user, not smb://, or not
 *  parsable yet). Drives the form's fill-Username-from-the-address step. */
QString userInShareInput(const QString &input);

} // namespace Session::ShareAddress
