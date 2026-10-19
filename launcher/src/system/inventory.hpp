// Prospero BlackBox - Read-only discovery of installed folders and images.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "system/locations.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace store::system
{
struct InstalledApp
{
    std::string id, name, version, path, reason;
    bool image = false;
    bool managed = false;
    bool duplicate = false;
};
struct Inventory
{
    std::vector<InstalledApp> apps;
    std::vector<std::string> errors;
    bool complete = true;
};
// Advisory snapshot only: transaction code must revalidate paths and ownership.
// Never follows a known symlink, writes files, mounts images, or removes receipts.
Inventory scan_installed(const ScanPolicy &policy, const std::string &receipts,
                         const std::atomic<bool> &cancelled);
} // namespace store::system
