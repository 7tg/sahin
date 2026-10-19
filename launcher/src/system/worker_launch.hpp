// Prospero BlackBox - Starts the file worker through the console's payload loader.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "install/worker.hpp"

namespace store::system
{
// Sends the bundled worker to the loader on this console; the connection then
// belongs to the worker. False when the loader isn't there or the file is missing.
bool launch_worker(install::Channel &channel);
// The same for another program in the store's folder (self-updater.elf).
bool launch_program(const char *file, install::Channel &channel);
} // namespace store::system
