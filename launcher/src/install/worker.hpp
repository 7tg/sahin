// Prospero BlackBox - Talking to the file worker, the process that unpacks and removes.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "install/archive.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace store::install
{
// The request is lines: the magic, "extract" with the archive, the title and
// the destination, or "remove" with a path. The worker answers "ready" before
// it touches anything, then "p <bytes unpacked>" five times a second, then
// "ok ..." or "fail <reason>". Any byte sent after the request cancels.
// "store" with a path saves a download: after "ready" the store sends pieces,
// each a four-byte little-endian length and that many bytes, and a length of
// zero to end; the worker answers "ok" once the file is durable. A connection
// that ends before the zero length leaves no file behind.
// "unregister" with a title ID takes that title off the home screen through the
// console's app-install service, which a loader payload may use on every
// firmware tried (an app may not on 12.70): the worker answers "ready", then
// "ok" or "fail <code>" with the console's answer as eight hex digits.
inline constexpr std::string_view kWorkerMagic = "PSW1";
inline constexpr std::size_t kWorkerLine = 2048;
inline constexpr std::size_t kWorkerPiece = 1u << 20;

// The worker runs with every right, so it only takes paths inside one of the
// store's own work folders (<drive>/nfs-blackbox/...), written plainly.
bool worker_path(std::string_view path);

// One connection to a worker that has just been started.
struct Channel
{
    std::function<long(const void *data, std::size_t size)> send;
    std::function<long(void *data, std::size_t size)> receive;
    std::function<void()> close;
};
using Connect = std::function<bool(Channel &channel)>;

// 1: done. 0: failed (error says why). -1: no worker could be started and
// nothing was touched, so the caller does the work itself.
int worker_extract(const Connect &connect, const std::string &archive, std::string_view title,
                   const std::string &destination, const std::atomic<bool> &cancelled,
                   std::atomic<std::uint64_t> &written, std::string &error, ExtractTimes &times);
int worker_remove(const Connect &connect, const std::string &path);
// One of the store's own folders, written plainly: a path ending in "/nfs-blackbox".
bool worker_folder(std::string_view path);
// "reclaim" with such a folder: everything in it is given to the worker's user (root)
// and opened to every user (folders 0777, files 0666), links untouched. A store that
// once ran sandboxed left folders an elevated store is refused in. 1: done. 0: failed.
// -1: no worker could be started, or the folder isn't one of the store's.
int worker_reclaim(const Connect &connect, const std::string &folder);
// A title ID, written plainly: four capitals and five digits (PPSA99400).
bool title_id_plain(std::string_view title);
// 1: the console took the title off the home screen. 0: it refused (code holds
// its answer). -1: no worker could be started, or the title ID is malformed.
int worker_unregister(const Connect &connect, const std::string &title, int &code);

// Where a download is written. finish makes it durable and closes it, and is
// always called; a worker whose writer is dropped without it leaves no file.
struct Writer
{
    std::function<bool(std::string_view data)> write;
    std::function<bool()> finish;
};
// A new file at path, written by a worker. False: no worker could be started
// and nothing was touched.
bool worker_store(const Connect &connect, const std::string &path, Writer &writer);
} // namespace store::install
