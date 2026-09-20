/*
 * smburl — the service menu's pure input handling: turning the smb:// URL
 * Dolphin substitutes into a validated CIFS UNC, and the small presentation
 * helpers that go with it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Split out of the dialog itself so it is testable without a GUI (smburl_test)
 * and so it survives the front end being rewritten: it moved here intact when
 * the QWidgets dialog was replaced by the shared QML form. None of it is a
 * security boundary — the KAuth helper re-validates every field regardless,
 * because a hostile process can invoke the action directly and never come
 * through this code at all — but it is the parsing most likely to meet
 * genuinely odd input, since Dolphin substitutes %u with the URL *as
 * displayed*, spaces and all.
 */

#pragma once

#include <QString>
#include <QUrl>

namespace Dialog::SmbUrl
{

/** Turns an smb:// URL into a CIFS UNC path, or reports why it cannot.
 *  Rejects ports, passwords, queries, fragments, IPv6 hosts, server-only
 *  URLs and control characters. */
bool parse(const QString &raw, QString *unc, QString *user, QString *error);

/**
 * Suggests ~/<ShareName> for the share being mounted.
 *
 * The share's own name is kept as-is rather than upper-cased or filed under a
 * fixed parent folder: those are personal conventions, and the name the server
 * already uses is the least surprising default.
 */
QString suggestMountpoint(const QString &unc);

/** Coarse state text for a saved share, without needing the full KCM model. */
QString describeState(const QString &mountPoint);

/**
 * The credential-lookup target for a share (autofill plan §3.2).
 *
 * KDE's SMB worker authenticates against the server and the *first* path
 * component only — `smb://10.0.0.10/DATA` for `//10.0.0.10/DATA/Pavel` — so
 * matching that construction is what makes a lookup hit an entry Dolphin
 * saved. Built with QUrl's structured setters from the already-validated UNC
 * rather than by string concatenation, so a share name containing a space or
 * a percent sign is encoded once, by QUrl, on the way out.
 *
 * Returns an invalid QUrl for anything that is not `//host/share[/...]`;
 * a caller treats that as "no lookup", never as a reason to broaden the
 * target.
 */
QUrl authLookupTarget(const QString &unc);

/** A username split into its domain and bare-name halves. */
struct Identity {
    QString domain;
    QString username;
};

/**
 * Splits `DOMAIN\user` or `DOMAIN/user` exactly as KDE's SMB worker does
 * (SMBUrl::splitDomainUser): the *first* of the two separators wins, and a
 * separator at position 0 does not split at all. `user@example.com` is
 * deliberately left whole — a UPN is a username, not a qualified pair
 * (plan §3.3).
 *
 * Pure, and used on both sides of the comparison: a candidate returned by
 * KDE is normalised through this before it is compared with the identity the
 * smb:// URL carried, so `DOMAIN\pavel` and `pavel` are not mistaken for two
 * different accounts.
 */
Identity splitDomainUser(const QString &combined);

} // namespace Dialog::SmbUrl
