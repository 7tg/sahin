// Prospero BlackBox - What a game's plan needs, put in mods-cache.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "library/install_plan.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace store::library
{
// Every source the plan can use: its placements' and its upgrades'.
std::vector<const Source *> needed_sources(const InstallPlan &plan);

// Downloads url, or with ranged its length bytes from offset (an HTTP Range request), handing
// each piece to sink (false from it stops); never more than length bytes. True when all of it
// arrived; error otherwise. The caller's transport (the store's HTTPS stack on the console, a
// local server in tests). Called from several threads at once, worker (0 to
// kParallelDownloads - 1) saying which: each worker can keep its own connection.
using Download = std::function<bool(std::size_t worker, const std::string &url, std::uint64_t offset,
                                    std::uint64_t length, bool ranged,
                                    const std::function<bool(std::string_view)> &sink,
                                    std::string &error)>;

// Downloads run this many requests at once, a big file in kDownloadPiece ranges: one
// connection got 11.6 MB/s on a wired console where a Mac on the same line got 96 MB/s, and
// three whole files at once ended with the biggest alone. Files the manager carries are
// copied one at a time first.
inline constexpr int kParallelDownloads = 4;
inline constexpr std::uint64_t kDownloadPiece = 32ull << 20;

struct FetchStatus
{
    std::vector<std::string> names;    // the sources being fetched now, for the page
    std::size_t index = 0, count = 0;  // how many started, of how many missing
    std::uint64_t done = 0, total = 0; // bytes of the ones being fetched now
    std::uint64_t all_done = 0, all_total = 0; // bytes of all the missing ones
    // Set on the report right after a file arrived: its name, bytes and seconds.
    std::string finished;
    std::uint64_t finished_bytes = 0;
    double finished_seconds = 0.0;
    // Set on the report right after a ranged piece arrived, for the log: which, by which
    // worker, its seconds in all, writing to the file and waiting for the shared lock.
    std::string piece;
};

// Puts every needed source the cache lacks into it, as <sha256>: from bundled (a folder of
// <sha256>[.ext] files shipped with the manager; empty for none), else from its url. Each
// file is written beside its final name and hashed before it takes it, so the cache only
// ever holds whole, right files. A source with neither is named in error (it has to be put
// in mods-cache by hand). False with error set at the first one that can't be had.
bool fetch_sources(const InstallPlan &plan, const std::string &cache, const std::string &bundled,
                   const Download &download,
                   const std::function<void(const FetchStatus &)> &progress,
                   const std::atomic<bool> &cancel, std::string &error,
                   std::uint64_t piece = kDownloadPiece);
} // namespace store::library
