/*
 * userlock — the per-user lock shared by the dialog, the KCM and the
 * cleanup tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The root lock in the helper only covers one privileged call at a time; it
 * says nothing about the unprivileged config writes a client makes
 * immediately before or after that call. Without a lock spanning all of it,
 * two cooperating frontends can interleave
 * an Add or Delete's config snapshot, its KAuth call and its config commit.
 * This does not defend against a hostile process already running as the
 * user — nothing unprivileged can — it keeps
 * *cooperating* callers consistent with each other.
 *
 * Lock ordering is fixed: this lock is always acquired before the root lock,
 * and released after it. The helper never acquires this one.
 */

#pragma once

#include <QString>

#include <memory>

namespace Session
{

/**
 * RAII flock() in $XDG_RUNTIME_DIR. Move-only so its lifetime can be handed
 * from a background thread to a continuation on another thread by moving a
 * std::unique_ptr<UserLock>, which is how MountActions keeps it held across
 * the KAuth call and the following config commit.
 */
class UserLock
{
public:
    UserLock(const UserLock &) = delete;
    UserLock &operator=(const UserLock &) = delete;
    UserLock(UserLock &&other) noexcept;
    UserLock &operator=(UserLock &&other) noexcept;
    ~UserLock();

    /** Blocks until the lock is held, or returns false with `error` set. */
    static std::unique_ptr<UserLock> acquire(QString *error);

private:
    UserLock() = default;
    int fd_ = -1;
};

} // namespace Session
