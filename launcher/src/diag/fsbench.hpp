// Prospero BlackBox - Development only: what each storage call costs on the console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace store::diag
{
inline long long bench_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct BenchRun
{
    std::string folder;
    int files = 0;
    std::size_t size = 0;
    int flags = 0;
    bool sync = false;
    long long open_us = 0, write_us = 0, sync_us = 0, close_us = 0, unlink_us = 0;
};

inline void *bench_files(void *opaque)
{
    auto &run = *static_cast<BenchRun *>(opaque);
    std::vector<char> data(run.size, 'x');
    mkdir(run.folder.c_str(), 0755);
    for (int i = 0; i < run.files; ++i)
    {
        const std::string path = run.folder + "/f" + std::to_string(i);
        auto t = bench_us();
        const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | run.flags, 0644);
        run.open_us += bench_us() - t;
        if (descriptor < 0)
            continue;
        t = bench_us();
        for (std::size_t done = 0; done < data.size();)
        {
            const auto wrote = write(descriptor, data.data() + done, data.size() - done);
            if (wrote <= 0)
                break;
            done += static_cast<std::size_t>(wrote);
        }
        run.write_us += bench_us() - t;
        if (run.sync)
        {
            t = bench_us();
            fsync(descriptor);
            run.sync_us += bench_us() - t;
        }
        t = bench_us();
        close(descriptor);
        run.close_us += bench_us() - t;
    }
    for (int i = 0; i < run.files; ++i)
    {
        const auto t = bench_us();
        unlink((run.folder + "/f" + std::to_string(i)).c_str());
        run.unlink_us += bench_us() - t;
    }
    rmdir(run.folder.c_str());
    return nullptr;
}

inline void bench_report(const char *name, const BenchRun &run, long long wall)
{
    std::printf("[STORE] fsbench %s files=%d size=%zu wall_ms=%lld open_ms=%lld write_ms=%lld "
                "sync_ms=%lld close_ms=%lld unlink_ms=%lld\n",
                name, run.files, run.size, wall / 1000, run.open_us / 1000, run.write_us / 1000,
                run.sync_us / 1000, run.close_us / 1000, run.unlink_us / 1000);
}

inline void bench_one(const char *name, const std::string &folder, int files, std::size_t size,
                      int flags, bool sync)
{
    BenchRun run{folder, files, size, flags, sync};
    const auto started = bench_us();
    bench_files(&run);
    bench_report(name, run, bench_us() - started);
}

// Waits until the storage answers quickly again and says how long that took.
inline void bench_settle(const std::string &base)
{
    const auto started = bench_us();
    for (int round = 0; round < 120; ++round)
    {
        BenchRun run{base + "/probe", 20, 4096, 0, false};
        const auto t = bench_us();
        bench_files(&run);
        const auto took = (bench_us() - t) / 1000;
        if (took < 400)
        {
            std::printf("[STORE] fsbench settled after_ms=%lld probe_ms=%lld\n",
                        (bench_us() - started) / 1000, took);
            std::fflush(stdout);
            return;
        }
        sleep(5);
    }
    std::printf("[STORE] fsbench never settled\n");
}

// An install in small: a download of megabytes written in pieces, then count
// files of size bytes each. sync_big makes the download durable before the
// files; sync_end makes every file durable at the end.
inline void bench_install(const std::string &base, const char *name, int megabytes, int count,
                          std::size_t size, bool sync_big, bool sync_end)
{
    bench_settle(base);
    std::vector<char> data(std::max<std::size_t>(size, 262144), 'x');
    const std::string big = base + "/big.bin";
    auto mark = bench_us();
    long long big_sync = 0;
    {
        const int descriptor = open(big.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        for (int i = 0; i < megabytes * 4 && descriptor >= 0; ++i)
            if (write(descriptor, data.data(), 262144) < 0)
                break;
        const auto t = bench_us();
        if (sync_big && descriptor >= 0)
            fsync(descriptor);
        big_sync = bench_us() - t;
        if (descriptor >= 0)
            close(descriptor);
    }
    std::printf("[STORE] fsbench %s download mb=%d ms=%lld sync_ms=%lld\n", name, megabytes,
                (bench_us() - mark) / 1000, big_sync / 1000);
    long long open_us = 0, write_us = 0;
    const auto begun = bench_us();
    mark = begun;
    for (int i = 0; i < count; ++i)
    {
        const std::string folder = base + "/s" + std::to_string(i / 100);
        if (i % 100 == 0)
            mkdir(folder.c_str(), 0755);
        auto t = bench_us();
        const int descriptor =
            open((folder + "/f" + std::to_string(i)).c_str(), O_WRONLY | O_CREAT, 0644);
        open_us += bench_us() - t;
        if (descriptor >= 0)
        {
            t = bench_us();
            if (write(descriptor, data.data(), size) < 0)
                std::printf("[STORE] fsbench write failed\n");
            write_us += bench_us() - t;
            close(descriptor);
        }
        if (i % 500 == 499)
        {
            std::printf("[STORE] fsbench %s files=%d step_ms=%lld open_ms=%lld write_ms=%lld\n",
                        name, i + 1, (bench_us() - mark) / 1000, open_us / 1000, write_us / 1000);
            std::fflush(stdout);
            mark = bench_us();
            open_us = write_us = 0;
        }
    }
    const auto made = bench_us() - begun;
    mark = bench_us();
    if (sync_end)
        for (int i = 0; i < count; ++i)
        {
            const int descriptor =
                open((base + "/s" + std::to_string(i / 100) + "/f" + std::to_string(i)).c_str(),
                     O_RDONLY);
            if (descriptor >= 0)
            {
                fsync(descriptor);
                close(descriptor);
            }
        }
    const auto synced = bench_us() - mark;
    mark = bench_us();
    unlink(big.c_str());
    for (int i = 0; i < count; ++i)
        unlink((base + "/s" + std::to_string(i / 100) + "/f" + std::to_string(i)).c_str());
    for (int i = 0; i <= count / 100; ++i)
        rmdir((base + "/s" + std::to_string(i)).c_str());
    std::printf("[STORE] fsbench %s total files=%d create_ms=%lld sync_ms=%lld remove_ms=%lld\n",
                name, count, made / 1000, synced / 1000, (bench_us() - mark) / 1000);
    std::fflush(stdout);
}

// root is a folder that exists; everything made below it is removed again.
inline void *fsbench(void *opaque)
{
    const std::string root = *static_cast<std::string *>(opaque);
    const std::string base = root + "/fsbench";
    mkdir(base.c_str(), 0755);
    std::printf("[STORE] fsbench start root=%s\n", root.c_str());
    bench_install(base, "files-only", 0, 3000, 65536, false, false);
    bench_install(base, "after-download", 200, 3000, 65536, false, false);
    bench_install(base, "download-synced", 200, 3000, 65536, true, false);
    bench_install(base, "synced-at-end", 200, 3000, 65536, true, true);
    bench_settle(base);
    rmdir(base.c_str());
    std::printf("[STORE] fsbench done root=%s\n", root.c_str());
    std::fflush(stdout);
    return nullptr;
}
} // namespace store::diag
