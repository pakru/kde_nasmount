/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Session
{

/** Validates a CMake or package-owned cleanup manifest against the complete
 * set of paths this project installs, and returns the exact paths.
 *
 * The set is fixed except for one family: the translation catalogs
 * (/usr/share/locale/<lang>/LC_MESSAGES/kcm_nasmount.mo), of which a
 * manifest holds one per shipped language, possibly none. Each must match a
 * strict pattern, may appear once, and is counted into the completeness check
 * without ever standing in for a fixed component. A listed catalog may be
 * absent on disk (dpkg path-exclude, rpm %_install_langs); this function reads
 * only the manifest's text and never treats that as an error. */
bool validateInstallManifest(const QString &manifestPath, QStringList *targets, QString *error);

} // namespace Session
