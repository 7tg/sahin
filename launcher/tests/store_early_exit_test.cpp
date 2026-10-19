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

    fs::remove_all(dir);
    std::puts("Early exit checks passed");
    return 0;
}
