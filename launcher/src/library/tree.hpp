// Prospero BlackBox - Whole folders: listed, copied with progress, removed.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace store::library
{
struct CopyProgress
{
    std::uint64_t done = 0, total = 0; // bytes
    std::string file;                  // the file being copied
};

// What other computers leave beside the files they copy: macOS's resource forks and
// folder settings, Windows's thumbnails. Never part of a game; never copied.
bool litter(const std::string &name);

// Every regular file under root (relative paths, sizes) and every folder, parents first.
// False with error set at a link or a special file, or a folder that can't be read.
bool walk(const std::string &root, std::vector<std::pair<std::string, std::uint64_t>> &files,
          std::vector<std::string> &dirs, std::string &error);

// Removes a file or a whole folder; true when nothing is left at path.
bool remove_all(const std::string &path);

// Copies the folder from into to, which is made new (whatever was there is removed
// first). Every file is synced. False with error set on failure or cancel, and then
// nothing is left at to.
bool copy_tree(const std::string &from, const std::string &to,
               const std::function<void(const CopyProgress &)> &progress,
               const std::atomic<bool> &cancel, std::string &error);
} // namespace store::library
