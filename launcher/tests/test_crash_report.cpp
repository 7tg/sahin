// ps5-native-app-boilerplate - Bounded subprocess crash-report regression.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../examples/crash-report/crash_report.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int main()
{
    char folder[] = "/tmp/native-crash-XXXXXX";
    assert(mkdtemp(folder));
    for (int run = 0; run < 2; ++run)
    {
        const auto child = fork();
        assert(child >= 0);
        if (child == 0)
        {
            alarm(5);
            assert(crash_report::install(folder, "01.000.000",
                                         [](bool restart) { _exit(restart ? 42 : 43); }));
            raise(SIGABRT);
            _exit(99);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 42 + run);
        const std::string path = std::string(folder) + "/crash-latest.txt";
        FILE *file = std::fopen(path.c_str(), "rb");
        assert(file);
        char contents[2048]{};
        assert(std::fread(contents, 1, sizeof(contents) - 1, file) > 0);
        assert(std::fclose(file) == 0);
        assert(std::strstr(contents, "rip: 0x") && std::strstr(contents, "01.000.000"));
    }
    unlink((std::string(folder) + "/crash-latest.txt").c_str());
    unlink((std::string(folder) + "/crash-pending").c_str());
    rmdir(folder);
}
