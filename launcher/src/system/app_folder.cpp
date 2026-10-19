// Prospero BlackBox - Where the running store's own files are.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/app_folder.hpp"
#include <sys/stat.h>

namespace store::system
{
#define STORE_TITLE_TEXT2(x) #x
#define STORE_TITLE_TEXT(x) STORE_TITLE_TEXT2(x)
const char *own_title()
{
#ifdef BLACKBOX_GAME
    return STORE_TITLE_TEXT(BLACKBOX_GAME);
#else
    return "PPSA99400";
#endif
}

const std::string &app_folder()
{
    static const std::string folder = []
    {
        const std::string title = own_title();
        for (const std::string &candidate :
             {std::string("/app0"), "/system_ex/app/" + title, "/mnt/sandbox/" + title + "_000/app0"})
        {
            struct stat info
            {
            };
            if (stat((candidate + "/eboot.bin").c_str(), &info) == 0 && S_ISREG(info.st_mode))
                return candidate;
        }
        return std::string("/app0");
    }();
    return folder;
}
} // namespace store::system
