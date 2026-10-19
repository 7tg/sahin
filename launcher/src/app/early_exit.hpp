// Sahin - The game closing right after it started, told on the launcher's next start.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>
#include <string>

namespace store
{
// A game that is back within this many seconds of its start closed right after it started.
inline constexpr std::time_t kEarlyExitSeconds = 20;
// What the page says then: the likely cause is a retail Blur.exe, whose SecuROM won't run here.
inline constexpr const char *kEarlyExitHint =
    "Blur closed right after it started. If your Blur.exe is the retail one with SecuROM, it "
    "won't run here: use a DRM-free exe.";

// Writes now (seconds since the epoch) into path, as the game is started. False when it
// can't be written.
bool mark_start(const std::string &path, std::time_t now);

// The start path records was less than kEarlyExitSeconds before now. False when there is no
// record, it doesn't read as a time, or it lies in the future.
bool early_exit(const std::string &path, std::time_t now);
} // namespace store
