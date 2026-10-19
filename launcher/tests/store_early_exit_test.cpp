// Sahin - The game closing right after it started: told from the last start's time.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/early_exit.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("sahin-early-exit-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "last-start").string();
    const std::time_t now = 1'800'000'000;

    // Missing: no start to speak of.
    assert(!store::early_exit(path, now));
    // Written at a start, read back at the next: 5 s is right after, 60 s a game that ran.
    assert(store::mark_start(path, now - 5));
    assert(store::early_exit(path, now));
    assert(store::mark_start(path, now - 60));
    assert(!store::early_exit(path, now));
    // The edge: under the window only.
    assert(store::mark_start(path, now - store::kEarlyExitSeconds + 1));
    assert(store::early_exit(path, now));
    assert(store::mark_start(path, now - store::kEarlyExitSeconds));
    assert(!store::early_exit(path, now));
    // A start in the future (the clock moved back) or a file that isn't a time: no.
    assert(store::mark_start(path, now + 5));
    assert(!store::early_exit(path, now));
    for (const char *text : {"", "abc", "12x", "-5\n", "99999999999999999999999"})
    {
        std::ofstream(path, std::ios::trunc) << text;
        assert(!store::early_exit(path, now));
    }
    // Written as a number and a newline, nothing else.
    assert(store::mark_start(path, now));
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    assert(line == std::to_string(now));
    // A folder that isn't there: refused, not thrown.
    assert(!store::mark_start((dir / "nowhere" / "last-start").string(), now));
    assert(std::string(store::kEarlyExitHint).find("SecuROM") != std::string::npos);

    // The runner's return request: <its library root>/return-<title>, an empty file.
    assert(store::return_request_path("/data/prospero-win", "PPSA99404") ==
           "/data/prospero-win/return-PPSA99404");
    const std::string request = store::return_request_path(dir.string(), "PPSA99404");
    assert(store::request_return(request));
    assert(fs::exists(request) && fs::file_size(request) == 0);
    assert(!store::request_return((dir / "nowhere" / "return-PPSA99404").string()));

    // prepare_launch: record then request, both there on success.
    {
        const std::string rec = (dir / "pl-record").string(), req = (dir / "pl-request").string();
        assert(store::prepare_launch(rec, req, now));
        assert(fs::exists(rec) && fs::exists(req));
        fs::remove(rec);
        fs::remove(req);
        // Record unwritable: no request left.
        assert(!store::prepare_launch((dir / "nowhere" / "r").string(), req, now));
        assert(!fs::exists(req));
        // Request unwritable: the record is removed again.
        assert(!store::prepare_launch(rec, (dir / "nowhere" / "q").string(), now));
        assert(!fs::exists(rec));
    }

    // The last start as the launcher's next start sees it.
    using store::LastStart;
    fs::remove(path);
    fs::remove(request);
    assert(store::last_start(path, request, now) == LastStart::none); // nothing: from the tile
    assert(store::request_return(request));
    assert(store::last_start(path, request, now) == LastStart::none); // a request alone
    assert(store::mark_start(path, now - 5));
    // Both still there: the runner never took the request (closed from the system's menu,
    // crashed): a start from the tile, whatever the record's age.
    assert(store::last_start(path, request, now) == LastStart::none);
    fs::remove(request); // the runner took it and brought us back
    assert(store::last_start(path, request, now) == LastStart::early);
    assert(store::mark_start(path, now - 600));
    assert(store::last_start(path, request, now) == LastStart::ran);
    std::ofstream(path, std::ios::trunc) << "garbage";
    assert(store::last_start(path, request, now) == LastStart::ran);

    // What the start does.
    using store::StartChoice;
    int asked = 0;
    const auto square = [&asked](bool held)
    {
        return [&asked, held]
        {
            ++asked;
            return held;
        };
    };
    const auto choose = [&](bool installed, bool ingame, LastStart last, bool held)
    { return store::choose_start(installed, ingame, last, square(held)); };
    // From the tile: the game, or the page with Square held.
    assert(choose(true, false, LastStart::none, false) == StartChoice::play);
    assert(choose(true, false, LastStart::none, true) == StartChoice::page);
    // Back from a game that ran: Home, or the page with Square held; never the game.
    assert(choose(true, false, LastStart::ran, false) == StartChoice::home);
    assert(choose(true, false, LastStart::ran, true) == StartChoice::page);
    asked = 0;
    // Back at once: the page (with the hint), the controller not read.
    assert(choose(true, false, LastStart::early, false) == StartChoice::page);
    // Not installed, or the in-game menu's choices: the page, whatever the last start.
    for (const auto last : {LastStart::none, LastStart::early, LastStart::ran})
    {
        assert(choose(false, false, last, false) == StartChoice::page);
        assert(choose(true, true, last, false) == StartChoice::page);
    }
    assert(asked == 0);
    // No loop: coming back never plays.
    for (const bool installed : {false, true})
        for (const bool ingame : {false, true})
            for (const bool held : {false, true})
                for (const auto last : {LastStart::early, LastStart::ran})
                    assert(choose(installed, ingame, last, held) != StartChoice::play);

    fs::remove_all(dir);
    std::puts("Early exit checks passed");
    return 0;
}
