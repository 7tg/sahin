// Sahin - The game closing right after it started, told on the launcher's next start; and what
// that start does: the game, the page, or back to Home.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/early_exit.hpp"

#include <cstdio>
#include <sys/stat.h>

namespace store
{
bool mark_start(const std::string &path, std::time_t now)
{
    std::FILE *file = std::fopen(path.c_str(), "w");
    if (!file)
        return false;
    const bool written = std::fprintf(file, "%lld\n", static_cast<long long>(now)) > 0;
    return std::fclose(file) == 0 && written;
}

bool early_exit(const std::string &path, std::time_t now)
{
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file)
        return false;
    char text[32] = {};
    const std::size_t size = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    // Digits, then at most a newline: anything else isn't a time this app wrote.
    long long started = 0;
    std::size_t at = 0;
    for (; at < size && text[at] >= '0' && text[at] <= '9'; ++at)
    {
        if (at >= 18)
            return false; // more than any time this app writes
        started = started * 10 + (text[at] - '0');
    }
    if (at == 0 || (at < size && !(text[at] == '\n' && at + 1 == size)))
        return false;
    const long long age = static_cast<long long>(now) - started;
    return age >= 0 && age < static_cast<long long>(kEarlyExitSeconds);
}

std::string return_request_path(std::string_view win_root, std::string_view title)
{
    return std::string(win_root) + "/return-" + std::string(title);
}

bool request_return(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "w");
    return file && std::fclose(file) == 0;
}

bool prepare_launch(const std::string &record, const std::string &request, std::time_t now)
{
    if (!mark_start(record, now))
    {
        std::remove(record.c_str()); // a half-written record
        return false;
    }
    if (!request_return(request))
    {
        std::remove(request.c_str());
        std::remove(record.c_str());
        return false;
    }
    return true;
}

LastStart last_start(const std::string &record, const std::string &return_request, std::time_t now)
{
    struct stat info{};
    if (stat(record.c_str(), &info) != 0 || stat(return_request.c_str(), &info) == 0)
        return LastStart::none;
    // A record that doesn't read as a recent time is a game that ran.
    return early_exit(record, now) ? LastStart::early : LastStart::ran;
}

StartChoice choose_start(bool installed, bool ingame, LastStart last,
                         const std::function<bool()> &square_held)
{
    if (!installed || ingame || last == LastStart::early)
        return StartChoice::page;
    if (square_held())
        return StartChoice::page;
    return last == LastStart::ran ? StartChoice::home : StartChoice::play;
}
} // namespace store
