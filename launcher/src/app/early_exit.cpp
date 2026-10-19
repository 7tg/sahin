// Sahin - The game closing right after it started, told on the launcher's next start.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/early_exit.hpp"

#include <cstdio>

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
} // namespace store
