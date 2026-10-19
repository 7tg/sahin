// Prospero BlackBox - The store's own folder, taken back when another user left files in it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace store::system
{
// A store that ran sandboxed (ShadowMountPlus 1.7 gives a sandboxed app /data) left its
// folder owned by the sandbox's user; the elevated store is then refused in it ("open:
// Permission denied"). Call once elevated, before anything is written there: when
// something in the folder belongs to another user, the file worker takes it all back.
// Returns what was found and done, for the log.
std::string reclaim_store_folder(const std::string &folder);
} // namespace store::system
