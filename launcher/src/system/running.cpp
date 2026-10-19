// Prospero BlackBox - Is a title running? Asked before its folder is touched.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/running.hpp"
#include "catalog/catalog.hpp"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>

namespace store::system
{
bool running_titles(std::vector<std::string> &ids, const std::string &sandboxes)
{
    std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(sandboxes.c_str()), closedir);
    if (!directory)
        return false;
    std::vector<std::string> found;
    for (;;)
    {
        errno = 0;
        const auto *entry = readdir(directory.get());
        if (!entry)
        {
            if (errno)
                return false;
            break;
        }
        const std::string name = entry->d_name;
        if (name.size() > 10 && name[9] == '_' && catalog::title_id(name.substr(0, 9)) &&
            std::find(found.begin(), found.end(), name.substr(0, 9)) == found.end())
            found.push_back(name.substr(0, 9));
    }
    ids = std::move(found);
    return true;
}

int title_running(const std::string &id, const std::string &sandboxes)
{
    if (!catalog::title_id(id))
        return -1;
    std::vector<std::string> ids;
    if (running_titles(ids, sandboxes))
        return std::find(ids.begin(), ids.end(), id) != ids.end() ? 1 : 0;
    // The folder can't be listed: ask for the usual sandbox by name.
    struct stat info
    {
    };
    if (lstat((sandboxes + "/" + id + "_000").c_str(), &info) == 0)
        return 1;
    return -1;
}
} // namespace store::system
