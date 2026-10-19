// ps5-native-app-boilerplate - Signal-safe reports and a prestarted restart worker.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace crash_report
{
// Adapted from ProsperoEden's crash handler: fixed buffers and system calls
// in the signal handler; a worker created at startup performs the restart.
// The directory must exist. Call after elevation, before application workers.
// lifecycle(true) restarts the app; lifecycle(false) closes it. No stdio in it.
bool install(const char *directory, const char *version, void (*lifecycle)(bool));
bool recovered();
void stop();
} // namespace crash_report
