// Prospero BlackBox - Is a title running? Asked before its folder is touched.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace store::system
{
// The console gives every running title a sandbox folder named
// <TITLEID>_<number> and removes it when the title ends. 0: not running,
// 1: running, -1: unknown (and unknown is treated as running by the caller).
int title_running(const std::string &id, const std::string &sandboxes = "/mnt/sandbox");
// The title IDs with a sandbox now; false when the folder can't be listed.
bool running_titles(std::vector<std::string> &ids, const std::string &sandboxes = "/mnt/sandbox");
} // namespace store::system
