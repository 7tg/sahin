// Prospero BlackBox - What a game's plan needs, put in mods-cache.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/game_sources.hpp"
#include "core/save_file.hpp"
#include "library/game_install.hpp"
#include "library/sha256_hex.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>

namespace store::library
{
std::vector<const Source *> needed_sources(const InstallPlan &plan)
{
    std::vector<const Source *> needed;
    std::set<std::string> seen;
    const auto add = [&](const std::string &id)
    {
        if (const auto *source = plan.source(id); source && seen.insert(id).second)
            needed.push_back(source);
    };
    for (const auto *list : {&plan.game_files, &plan.system_files})
        for (const auto &placement : *list)
            add(placement.source);
    for (const auto &upgrade : plan.upgrades)
        add(upgrade.source);
    return needed;
}

namespace
{
// Bytes a ranged piece gathers before writing them out (see the piece's sink).
constexpr std::size_t kWriteBuffer = 4u << 20;

// A file being written beside its final name, hashed as it goes; it takes its name only
// whole and right.
struct Part
{
    std::string path, final_path;
    int fd = -1;
    std::uint64_t written = 0;
    picosha2::hash256_one_by_one hasher;

    bool open_for(const std::string &cache, const Source &source, std::string &error)
    {
        final_path = cache + "/" + source.sha256;
        path = final_path + ".part";
        unlink(path.c_str());
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
        if (fd < 0)
            error = "Can't write to mods-cache: " + std::string(std::strerror(errno));
        return fd >= 0;
    }
    bool write(std::string_view piece)
    {
        hasher.process(piece.begin(), piece.end());
        written += piece.size();
        while (!piece.empty())
        {
            const ssize_t step = ::write(fd, piece.data(), piece.size());
            if (step <= 0)
                return false;
            piece.remove_prefix(static_cast<std::size_t>(step));
        }
        return true;
    }
    // Closes it; true and named when it is the source, removed otherwise.
    bool finish(const Source &source, std::string &error)
    {
        bool ok = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0 && close(fd) != 0)
            ok = false;
        fd = -1;
        hasher.finish();
        if (ok && (written != source.size || sha256_hex(hasher) != source.sha256))
        {
            error = source.name + " didn't arrive as the plan pins it (size or SHA-256)";
            ok = false;
        }
        else if (!ok)
            error = "Can't write " + source.name + " to mods-cache: " + std::strerror(errno);
        if (ok && rename(path.c_str(), final_path.c_str()) != 0)
        {
            error = "Can't put " + source.name + " in mods-cache: " + std::strerror(errno);
            ok = false;
        }
        if (!ok)
            unlink(path.c_str());
        return ok;
    }
    ~Part()
    {
        if (fd >= 0)
        {
            close(fd);
            unlink(path.c_str());
        }
    }
};
} // namespace

bool fetch_sources(const InstallPlan &plan, const std::string &cache, const std::string &bundled,
                   const Download &download,
                   const std::function<void(const FetchStatus &)> &progress,
                   const std::atomic<bool> &cancel, std::string &error, std::uint64_t piece_size)
{
    if (!hui::save::ensure_directory(cache))
    {
        error = "Can't make " + cache;
        return false;
    }
    // What the cache lacks: carried by the manager, else downloaded, else named in error.
    std::vector<std::pair<const Source *, std::string>> carried; // and the carried file
    std::vector<const Source *> downloads;
    FetchStatus status;
    for (const auto *source : needed_sources(plan))
    {
        if (cancel.load())
        {
            error = "Cancelled";
            return false;
        }
        if (!find_cached(cache, *source, cancel).empty())
            continue;
        status.all_total += source->size;
        ++status.count;
        auto shipped = bundled.empty() ? std::string() : find_cached(bundled, *source, cancel);
        if (!shipped.empty())
            carried.emplace_back(source, std::move(shipped));
        else if (!source->url.empty() && download)
            downloads.push_back(source);
        else
        {
            error = source->name + " can't be downloaded: put it in mods-cache by hand";
            return false;
        }
    }

    // The status is shared by the threads: changed and reported under this lock.
    std::mutex lock;
    std::map<const Source *, std::uint64_t> flying; // bytes so far of each one under way
    std::uint64_t arrived = 0;                      // bytes of the ones done
    std::atomic<bool> stop{false};
    std::string first_error;
    const auto report = [&] // under lock
    {
        status.names.clear();
        status.done = status.total = 0;
        for (const auto &[source, done] : flying)
        {
            status.names.push_back(source->name.substr(0, source->name.find(" (")));
            status.done += done;
            status.total += source->size;
        }
        status.all_done = arrived + status.done;
        if (progress)
            progress(status);
    };
    // One source the manager carries into the cache. False with first_error set when it
    // can't be had.
    const auto fetch_one = [&](const Source *source, const std::string &shipped)
    {
        const auto started = std::chrono::steady_clock::now();
        {
            std::lock_guard held(lock);
            ++status.index;
            flying[source] = 0;
            report();
        }
        std::string why;
        Part part;
        bool ok = part.open_for(cache, *source, why);
        const auto sink = [&](std::string_view piece)
        {
            if (cancel.load() || stop.load() || !part.write(piece))
                return false;
            std::lock_guard held(lock);
            flying[source] = part.written;
            report();
            return true;
        };
        if (ok)
        {
            const int in = ::open(shipped.c_str(), O_RDONLY | O_NOFOLLOW);
            std::vector<char> buffer(1u << 20);
            ssize_t count = in < 0 ? -1 : 0;
            while (in >= 0 && (count = read(in, buffer.data(), buffer.size())) > 0 &&
                   sink(std::string_view(buffer.data(), static_cast<std::size_t>(count))))
            {
            }
            if (in >= 0)
                close(in);
            if (count != 0)
            {
                why = "Can't copy " + source->name + " from the manager";
                ok = false;
            }
        }
        ok = ok && part.finish(*source, why);
        std::lock_guard held(lock);
        flying.erase(source);
        if (!ok)
        {
            if (first_error.empty())
                first_error = why;
            stop = true;
            return false;
        }
        arrived += source->size;
        status.finished = source->name.substr(0, source->name.find(" ("));
        status.finished_bytes = source->size;
        status.finished_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        report();
        status.finished.clear();
        return true;
    };

    for (const auto &[source, shipped] : carried)
        if (cancel.load() || !fetch_one(source, shipped))
            break;
    // The downloads, in kDownloadPiece ranges taken by kParallelDownloads workers. Each piece
    // is its own file, written straight through: four workers writing one file at four
    // places spent 11 of every 11.1 s writing (run 7, 2.9 MB/s each). The worker that brings
    // a file's last piece joins its pieces in order into the file beside its final name,
    // hashing them as it goes, and the file takes its name only whole and right.
    struct Fetch
    {
        const Source *source = nullptr;
        std::string part, final_path;
        std::size_t pieces = 0, pieces_left = 0;
        std::chrono::steady_clock::time_point started{};
        bool begun = false, done = false;
    };
    std::vector<Fetch> fetches(downloads.size());
    struct Piece
    {
        std::size_t fetch;
        std::uint64_t offset, length;
    };
    std::vector<Piece> pieces;
    for (std::size_t i = 0; i < downloads.size() && first_error.empty(); ++i)
    {
        auto &fetch = fetches[i];
        fetch.source = downloads[i];
        fetch.final_path = cache + "/" + fetch.source->sha256;
        fetch.part = fetch.final_path + ".part";
        const auto size = fetch.source->size;
        for (std::uint64_t at = 0; at < size || (size == 0 && at == 0); at += piece_size)
        {
            pieces.push_back({i, at, std::min(piece_size, size - at)});
            ++fetch.pieces;
            if (size == 0)
                break;
        }
        fetch.pieces_left = fetch.pieces;
    }
    const auto piece_path = [&](const Fetch &fetch, std::uint64_t offset)
    { return fetch.part + "." + std::to_string(offset / piece_size); };
    // A file whose pieces are all in: joined in order into its .part, hashed as it goes,
    // synced, named; the pieces removed. Outside the lock.
    const auto finish = [&](Fetch &fetch, std::string &why)
    {
        unlink(fetch.part.c_str());
        const int out = ::open(fetch.part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
        bool ok = out >= 0;
        picosha2::hash256_one_by_one hasher;
        std::vector<char> buffer(1u << 20);
        std::uint64_t joined = 0;
        for (std::size_t n = 0; ok && n < fetch.pieces; ++n)
        {
            const auto path = piece_path(fetch, static_cast<std::uint64_t>(n) * piece_size);
            const int in = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
            ok = in >= 0;
            for (ssize_t got; ok && (got = read(in, buffer.data(), buffer.size())) != 0;)
            {
                if (got < 0 || cancel.load() || stop.load())
                {
                    ok = false;
                    break;
                }
                hasher.process(buffer.begin(), buffer.begin() + got);
                joined += static_cast<std::uint64_t>(got);
                for (const char *at = buffer.data(), *end = at + got; ok && at < end;)
                {
                    const ssize_t wrote = ::write(out, at, static_cast<std::size_t>(end - at));
                    ok = wrote > 0;
                    at += ok ? wrote : 0;
                }
            }
            if (in >= 0)
                close(in);
            unlink(path.c_str());
        }
        hasher.finish();
        ok = ok && fsync(out) == 0;
        const bool closed = out < 0 || close(out) == 0;
        if (ok && closed &&
            (joined != fetch.source->size || sha256_hex(hasher) != fetch.source->sha256))
        {
            why = fetch.source->name + " didn't arrive as the plan pins it (size or SHA-256)";
            unlink(fetch.part.c_str());
            return false;
        }
        if (!ok || !closed || rename(fetch.part.c_str(), fetch.final_path.c_str()) != 0)
        {
            why = "Can't put " + fetch.source->name + " in mods-cache: " + std::strerror(errno);
            unlink(fetch.part.c_str());
            return false;
        }
        return true;
    };
    if (first_error.empty() && !cancel.load() && !pieces.empty())
    {
        std::atomic<std::size_t> next{0};
        const auto worker = [&](std::size_t me)
        {
            for (std::size_t at; !stop.load() && !cancel.load() && (at = next++) < pieces.size();)
            {
                const auto piece = pieces[at];
                auto &fetch = fetches[piece.fetch];
                {
                    std::lock_guard held(lock);
                    if (!fetch.begun)
                    {
                        fetch.begun = true;
                        fetch.started = std::chrono::steady_clock::now();
                        ++status.index;
                        flying[fetch.source] = 0;
                        report();
                    }
                }
                std::uint64_t got = 0;
                const auto path = piece_path(fetch, piece.offset);
                unlink(path.c_str());
                const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
                using Clock = std::chrono::steady_clock;
                const auto piece_started = Clock::now();
                double writing = 0.0, waiting = 0.0; // seconds, for the log
                // Written kWriteBuffer at a time: curl hands over about 16 KiB at once, and
                // writes that small held each worker to 6 MB/s (run 8: 563 of 580 s writing).
                std::vector<char> pending;
                pending.reserve(kWriteBuffer);
                const auto flush = [&]
                {
                    const auto before = Clock::now();
                    for (const char *at = pending.data(), *end = at + pending.size(); at < end;)
                    {
                        const ssize_t wrote = ::write(fd, at, static_cast<std::size_t>(end - at));
                        if (wrote <= 0)
                            return false;
                        at += wrote;
                    }
                    pending.clear();
                    writing += std::chrono::duration<double>(Clock::now() - before).count();
                    return true;
                };
                const auto sink = [&](std::string_view bytes)
                {
                    if (fd < 0 || cancel.load() || stop.load() || got + bytes.size() > piece.length)
                        return false;
                    pending.insert(pending.end(), bytes.begin(), bytes.end());
                    got += bytes.size();
                    if (pending.size() >= kWriteBuffer && !flush())
                        return false;
                    const auto before = Clock::now();
                    std::lock_guard held(lock);
                    waiting += std::chrono::duration<double>(Clock::now() - before).count();
                    flying[fetch.source] += bytes.size();
                    report();
                    return true;
                };
                const bool ranged = piece.length != fetch.source->size;
                std::string failed, why;
                bool ok = fd >= 0 &&
                          download(me, fetch.source->url, piece.offset, piece.length, ranged, sink, failed) &&
                          got == piece.length && flush();
                const bool closed = fd < 0 || close(fd) == 0;
                if (!ok)
                    why = fd < 0 ? "Can't write to mods-cache: " + std::string(std::strerror(errno))
                                 : "Can't download " + fetch.source->name +
                                       (failed.empty() ? "" : ": " + failed);
                else if (!closed)
                {
                    why = "Can't write " + fetch.source->name + " to mods-cache";
                    ok = false;
                }
                bool last = false;
                {
                    std::lock_guard held(lock);
                    last = ok && --fetch.pieces_left == 0;
                    if (ranged)
                    {
                        char line[200];
                        std::snprintf(line, sizeof line,
                                      "%s piece at %llu MB, worker %zu: %.1f s in all, %.1f s "
                                      "writing, %.1f s waiting for the lock",
                                      fetch.source->name.substr(0, fetch.source->name.find(" (")).c_str(),
                                      static_cast<unsigned long long>(piece.offset >> 20), me,
                                      std::chrono::duration<double>(Clock::now() - piece_started).count(),
                                      writing, waiting);
                        status.piece = line;
                        report();
                        status.piece.clear();
                    }
                }
                if (last)
                    ok = finish(fetch, why);
                std::lock_guard held(lock);
                if (!ok)
                {
                    if (first_error.empty())
                        first_error = why;
                    stop = true;
                    continue;
                }
                if (!last)
                    continue;
                fetch.done = true;
                flying.erase(fetch.source);
                arrived += fetch.source->size;
                status.finished = fetch.source->name.substr(0, fetch.source->name.find(" ("));
                status.finished_bytes = fetch.source->size;
                status.finished_seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - fetch.started)
                        .count();
                report();
                status.finished.clear();
            }
        };
        // All on threads of their own, each its worker number for the caller's connection.
        std::vector<std::thread> threads;
        const auto count = std::min<std::size_t>(kParallelDownloads, pieces.size());
        for (std::size_t i = 0; i < count; ++i)
            threads.emplace_back(worker, i);
        for (auto &thread : threads)
            thread.join();
    }
    // What didn't arrive whole leaves nothing behind: its pieces and its .part.
    for (auto &fetch : fetches)
        if (!fetch.done && !fetch.part.empty())
        {
            for (std::size_t n = 0; n < fetch.pieces; ++n)
                unlink(piece_path(fetch, static_cast<std::uint64_t>(n) * piece_size).c_str());
            unlink(fetch.part.c_str());
        }
    if (cancel.load())
    {
        error = "Cancelled";
        return false;
    }
    if (!first_error.empty())
    {
        error = first_error;
        return false;
    }
    return true;
}
} // namespace store::library
