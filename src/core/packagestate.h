/*
 * packagestate - read-only proof that package removal cannot orphan nasmount
 * state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native package managers can remove program files without an interactive
 * KAuth round trip. Their pre-removal hooks therefore need a deliberately
 * narrower answer than the UI state model: removal is permitted only when
 * the host is provably empty of every nasmount-managed or managed-looking
 * root artifact. This API never repairs or removes anything.
 */

#pragma once

#include <QString>

namespace PackageState
{

enum class Classification {
    Empty,
    Managed,
    Unsafe,
    Indeterminate,
};

struct Result {
    Classification classification = Classification::Indeterminate;
    QString detail;
};

/**
 * Classifies package-removal state beneath an absolute filesystem root.
 * Production passes `/`; tests pass a temporary root so every fail-closed
 * branch is covered without touching the real `/etc` or `/run`.
 */
Result classify(const QString &filesystemRoot);

/** Classifies the real host. Kept separate so the guard has no path input. */
Result classifyHost();

} // namespace PackageState
