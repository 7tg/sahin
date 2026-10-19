// Prospero BlackBox - The shared Wine prefix, made from the bare prefix the manager carries.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace store::library
{
// Whether <win_root>/prefixes/<name> is a prefix (system.reg, user.reg, drive_c/windows).
bool prefix_ready(const std::string &win_root, const std::string &name);

// Makes <win_root>/prefixes/<name> from the bare-prefix zip (made by the Sahin repo's
// tools/make-bare-prefix.py: a prefix's contents, its links kept in .pw-symlinks tables
// as prospero-win reads them) when it isn't a prefix yet; an existing one is never touched.
// Unpacked beside its final name and renamed only when whole; plain relative paths only,
// no links. progress gets bytes written and the total. False with error set.
bool ensure_prefix(const std::string &win_root, const std::string &name, const std::string &zip,
                   const std::function<void(std::uint64_t done, std::uint64_t total)> &progress,
                   const std::atomic<bool> &cancel, std::string &error);
} // namespace store::library
