/*
 * rootlock — the single root-owned advisory lock shared by every privileged
 * nasmount executable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers authorisation, credential writes/deletes, unit-file changes,
 * start/stop and the completed reload for one privileged operation.
 * nasmount-helper and nasmount-boot both take this same lock, so a
 * boot arming pass and a helper call can never interleave — otherwise
 * `undefinesystem` could remove a pair that boot has already enumerated,
 * after which boot starts the still-loaded unit and records an id for an
 * orphan. nasmount-boot must never acquire the *per-user* lock
 * (Session::UserLock) — it has no user session — so no lock-order inversion
 * between the two lock kinds is possible.
 */

#pragma once

#include <QString>

#include <memory>

namespace Root
{

/**
 * RAII flock() on a root-owned, descriptor-verified file below
 * `/run/nasmount`. Move-only.
 */
class RootLock
{
public:
    RootLock(const RootLock &) = delete;
    RootLock &operator=(const RootLock &) = delete;
    RootLock(RootLock &&other) noexcept;
    RootLock &operator=(RootLock &&other) noexcept;
    ~RootLock();

    /**
     * Blocks until the lock is held, or returns nullptr with `error` set.
     * The lock directory and file are opened no-follow and verified root-
     * owned (regular file, mode exactly 0600) via their descriptors before
     * flock() — never "repaired" if verification fails, since a wrong
     * owner/mode/type at this exact path is itself a sign something is
     * already not as expected.
     */
    static std::unique_ptr<RootLock> acquire(QString *error);

private:
    RootLock() = default;
    int fd_ = -1;
};

} // namespace Root
