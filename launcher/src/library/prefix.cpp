// Prospero BlackBox - The shared Wine prefix, made from the bare prefix the manager carries.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/prefix.hpp"
#include "core/save_file.hpp"
#include "library/install_plan.hpp"
#include "library/tree.hpp"
#include "third_party/miniz/miniz.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace store::library
{
namespace
{
// Files unpacked at once, as the app installer does (install/archive.cpp).
constexpr std::size_t kUnpackWorkers = 8;
bool is_file(const std::string &path)
{
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_dir(const std::string &path)
{
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// path's folders under root, made as needed (path is already a safe relative path).
bool make_parents(const std::string &root, const std::string &path)
{
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
    {
        const auto folder = root + "/" + path.substr(0, slash);
        if (mkdir(folder.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

// What the unpacking workers share: the bytes written, the first failure, and the progress
// callback, called under the lock.
struct Shared
{
    std::atomic<std::uint64_t> written{0};
    std::atomic<bool> failed{false};
    std::mutex lock;
    std::string why;
    const std::atomic<bool> *cancel = nullptr;
    const std::function<void(std::uint64_t, std::uint64_t)> *progress = nullptr;
    std::uint64_t total = 0;
    void fail(const std::string &reason)
    {
        std::lock_guard held(lock);
        if (!failed.exchange(true))
            why = reason;
    }
};

struct Sink
{
    int fd = -1;
    Shared *shared = nullptr;
};

size_t write_piece(void *opaque, mz_uint64, const void *data, size_t size)
{
    auto &sink = *static_cast<Sink *>(opaque);
    auto &shared = *sink.shared;
    if (shared.cancel->load() || shared.failed.load())
        return 0;
    const char *bytes = static_cast<const char *>(data);
    std::size_t left = size;
    while (left > 0)
    {
        const ssize_t step = write(sink.fd, bytes, left);
        if (step <= 0)
            return 0;
        bytes += step;
        left -= static_cast<std::size_t>(step);
    }
    const auto written = shared.written += size;
    if (*shared.progress)
    {
        std::lock_guard held(shared.lock);
        (*shared.progress)(written, shared.total);
    }
    return size;
}

struct Zip
{
    mz_zip_archive archive{};
    bool open = false;
    ~Zip()
    {
        if (open)
            mz_zip_reader_end(&archive);
    }
};
} // namespace

bool prefix_ready(const std::string &win_root, const std::string &name)
{
    const auto prefix = win_root + "/prefixes/" + name;
    return is_file(prefix + "/system.reg") && is_file(prefix + "/user.reg") &&
           is_dir(prefix + "/drive_c/windows");
}

bool ensure_prefix(const std::string &win_root, const std::string &name, const std::string &zip,
                   const std::function<void(std::uint64_t done, std::uint64_t total)> &progress,
                   const std::atomic<bool> &cancel, std::string &error)
{
    if (prefix_ready(win_root, name))
        return true;
    const auto prefixes = win_root + "/prefixes";
    const auto final_path = prefixes + "/" + name;
    if (!safe_relative(name) || name.find('/') != std::string::npos)
    {
        error = "The prefix name " + name + " isn't a plain name";
        return false;
    }
    if (access(final_path.c_str(), F_OK) == 0)
    {
        error = final_path + " is there but isn't a Wine prefix; it was left as it is";
        return false;
    }
    Zip archive;
    archive.open = mz_zip_reader_init_file(&archive.archive, zip.c_str(), 0);
    if (!archive.open)
    {
        error = "The bare prefix the manager carries can't be read (" + zip + ")";
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&archive.archive);
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive.archive, i, &stat))
        {
            error = "The bare prefix is damaged";
            return false;
        }
        std::string path = stat.m_filename;
        if (!path.empty() && path.back() == '/')
            path.pop_back();
        const unsigned mode = stat.m_external_attr >> 16;
        if (!safe_relative(path) || S_ISLNK(mode) || stat.m_is_encrypted || !stat.m_is_supported)
        {
            error = "The bare prefix holds an entry it may not: " + std::string(stat.m_filename);
            return false;
        }
        total += stat.m_uncomp_size;
    }
    const auto building = final_path + ".unpacking";
    if (!hui::save::ensure_directory(win_root) || !hui::save::ensure_directory(prefixes) ||
        (access(building.c_str(), F_OK) == 0 && !remove_all(building)) ||
        mkdir(building.c_str(), 0777) != 0)
    {
        error = "Can't make " + building + ": " + std::strerror(errno);
        return false;
    }
    const auto fail = [&](const std::string &why)
    {
        error = cancel.load() ? "Cancelled" : why;
        remove_all(building);
        return false;
    };
    // Every folder first, in order (the zip's own, and each file's parents); then the files,
    // kUnpackWorkers at once, each worker with its own reader of the zip: creating a file costs
    // about a tenth of a second on the console's storage whatever its size (install/archive.cpp),
    // and one at a time the prefix took 34 s for 131 MB.
    std::vector<mz_uint> files;
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&archive.archive, i, &stat);
        std::string path = stat.m_filename;
        const bool directory = mz_zip_reader_is_file_a_directory(&archive.archive, i);
        if (directory && !path.empty() && path.back() == '/')
            path.pop_back();
        if (!make_parents(building, path))
            return fail("Can't make the folders for " + path + ": " + std::strerror(errno));
        if (!directory)
        {
            files.push_back(i);
            continue;
        }
        const auto target = building + "/" + path;
        if (mkdir(target.c_str(), 0777) != 0 && errno != EEXIST)
            return fail("Can't make " + path + ": " + std::strerror(errno));
    }
    Shared shared;
    shared.cancel = &cancel;
    shared.progress = &progress;
    shared.total = total;
    std::atomic<std::size_t> next{0};
    const auto worker = [&]
    {
        Zip reader;
        reader.open = mz_zip_reader_init_file(&reader.archive, zip.c_str(), 0);
        if (!reader.open)
        {
            shared.fail("The bare prefix the manager carries can't be read (" + zip + ")");
            return;
        }
        for (std::size_t at; !shared.failed.load() && !cancel.load() && (at = next++) < files.size();)
        {
            const mz_uint i = files[at];
            mz_zip_archive_file_stat stat{};
            mz_zip_reader_file_stat(&reader.archive, i, &stat);
            const std::string path = stat.m_filename;
            const auto target = building + "/" + path;
            Sink sink{::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666), &shared};
            if (sink.fd < 0)
            {
                shared.fail("Can't write " + path + ": " + std::strerror(errno));
                return;
            }
            // Whole: the file's own length is the member's.
            const bool ok =
                mz_zip_reader_extract_to_callback(&reader.archive, i, write_piece, &sink, 0) &&
                static_cast<std::uint64_t>(lseek(sink.fd, 0, SEEK_CUR)) == stat.m_uncomp_size &&
                fsync(sink.fd) == 0;
            if (close(sink.fd) != 0 || !ok)
            {
                shared.fail("Can't unpack " + path);
                return;
            }
        }
    };
    {
        std::vector<std::thread> threads;
        const auto workers = std::min<std::size_t>(kUnpackWorkers, files.size());
        for (std::size_t i = 0; i < workers; ++i)
            threads.emplace_back(worker);
        for (auto &thread : threads)
            thread.join();
    }
    if (cancel.load() || shared.failed.load())
        return fail(shared.why.empty() ? "Cancelled" : shared.why);
    if (!prefix_ready(win_root, name + ".unpacking"))
        return fail("The bare prefix isn't a Wine prefix (no system.reg, user.reg or drive_c/windows)");
    if (rename(building.c_str(), final_path.c_str()) != 0)
        return fail("Can't put the prefix in place: " + std::string(std::strerror(errno)));
    return true;
}
} // namespace store::library
