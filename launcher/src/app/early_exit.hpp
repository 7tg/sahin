// Sahin - The game closing right after it started, told on the launcher's next start; and what
// that start does: the game, the page, or back to Home.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>
#include <functional>
#include <string>
#include <string_view>

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

// prospero-win's runner (/app0/game.bin), started from the tile, closes to Home when the game
// ends, unless <its library root>/return-<title> is there: then it removes that file (one
// shot, before trying) and LoadExecs /app0/eboot.bin, this launcher, with no arguments
// (patches/prospero-win/0030). Its library root is /data/prospero-win: the runner closes to
// Home before starting a game when /data isn't granted. win_root is that root.
std::string return_request_path(std::string_view win_root, std::string_view title);
// Writes the (empty) request. False when it can't be written.
bool request_return(const std::string &path);

// Hands over to the game: the start record first, then the return request, only when the
// record is there (a request without a record would bring the launcher back to play the game
// again, over and over). When the request can't be written the record is removed again: the
// runner then closes to Home, and a record would read as "back from the game" on the next
// start from the tile. False when either failed (nothing is left behind).
bool prepare_launch(const std::string &record, const std::string &request, std::time_t now);

// The previous start of the game, as this start of the launcher sees it.
enum class LastStart
{
    none,  // no game started before this: a start from the tile
    early, // the runner brought us back within kEarlyExitSeconds of the start
    ran,   // the runner brought us back after the game ran
};

// From the start record (mark_start) and the return request: a record without the request
// means the runner took the request and brought us back; a record with the request still
// there means the runner never got to it (the app was closed from the system's menu, or it
// crashed), so this is a start from the tile like any other.
LastStart last_start(const std::string &record, const std::string &return_request, std::time_t now);

// What a start of the game's own app does.
enum class StartChoice
{
    play, // hand over to the game
    page, // the app's page (install, settings, repair, the early-exit hint)
    home, // close to Home: the game was played and ended
};

// installed: the game's install record is there; ingame: the game's in-game menu left choices
// for this start; square_held: read only when it decides (it waits on the controller). Coming
// back from the game never plays it again, so the game and the launcher can't loop.
StartChoice choose_start(bool installed, bool ingame, LastStart last,
                         const std::function<bool()> &square_held);
} // namespace store
