/*
 * smburl — the service menu's pure input handling: turning the smb:// URL
 * Dolphin substitutes into a validated CIFS UNC, and the small presentation
 * helpers that go with it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Split out of the dialog itself so it is testable without a GUI
 * (smburl_test). None of it is a
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
 * The credential-lookup target for a share: the server and the
 * *first* path component only, which is what KDE's SMB worker authenticates
 * against and therefore what makes a lookup hit an entry Dolphin saved. Built
 * with QUrl's setters, so a space or percent sign is encoded once. An invalid
 * QUrl means "no lookup", never a reason to broaden the target.
 */
QUrl authLookupTarget(const QString &unc);

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

} // namespace Dialog::SmbUrl
