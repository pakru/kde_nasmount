/*
 * smburl — the service menu's small presentation helpers for the share it
 * was invoked on: a suggested mount point, a coarse state line, and the
 * credential-lookup target.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Split out of the dialog itself so it is testable without a GUI
 * (smburl_test). Turning the smb:// URL Dolphin substitutes into a UNC is not
 * here: the KCM's Add field accepts smb:// addresses too, so that parser lives
 * in the session library as Session::ShareAddress, shared by both front ends.
 */

#pragma once

#include <QString>
#include <QUrl>

namespace Dialog::SmbUrl
{

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

} // namespace Dialog::SmbUrl
