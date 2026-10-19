// Prospero BlackBox - Whole folders: listed, copied with progress, removed.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/tree.hpp"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store::library
{
namespace
{
std::vector<std::string> names_in(DIR *directory, bool skip_litter)
{
    std::vector<std::string> names;
    while (const dirent *entry = readdir(directory))
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0 &&
            !(skip_litter && litter(entry->d_name)))
            names.emplace_back(entry->d_name);
    closedir(directory);
    return names;
}

bool walk_in(const std::string &root, const std::string &relative,
             std::vector<std::pair<std::string, std::uint64_t>> &files,
             std::vector<std::string> &dirs, std::string &error)
{
    const std::string folder = relative.empty() ? root : root + "/" + relative;
    DIR *directory = opendir(folder.c_str());
    if (!directory)
    {
        error = "Can't read " + folder + ": " + std::strerror(errno);
        return false;
    }
    for (const auto &name : names_in(directory, true))
    {
        const std::string inner = relative.empty() ? name : relative + "/" + name;
        struct stat info{};
        if (lstat((root + "/" + inner).c_str(), &info) != 0)
        {
            error = "Can't read " + inner;
            return false;
        }
        if (S_ISDIR(info.st_mode))
        {
            dirs.push_back(inner);
            if (!walk_in(root, inner, files, dirs, error))
                return false;
        }
        else if (S_ISREG(info.st_mode))
            files.emplace_back(inner, static_cast<std::uint64_t>(info.st_size));
        else
        {
            error = inner + " is a link or a special file; copy the game's real files";
            return false;
        }
    }
    return true;
}

// One file's bytes, synced; false on a short read or write, or a cancel.
bool copy_file(const std::string &from, const std::string &to, std::uint64_t size,
               std::vector<char> &buffer, CopyProgress &state,
               const std::function<void(const CopyProgress &)> &progress,
               const std::atomic<bool> &cancel, std::string &error)
{
    const int in = open(from.c_str(), O_RDONLY | O_NOFOLLOW);
    const int out =
        in < 0 ? -1 : open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    if (in < 0 || out < 0)
    {
        error = "Can't copy " + state.file + ": " + std::strerror(errno);
        if (in >= 0)
            close(in);
        return false;
    }
    std::uint64_t copied = 0;
    bool ok = true;
    while (ok)
    {
        if (cancel.load())
        {
            close(in);
            close(out);
            error = "Cancelled";
            return false;
        }
        const ssize_t count = read(in, buffer.data(), buffer.size());
        if (count <= 0)
        {
            ok = count == 0;
            break;
        }
        for (ssize_t written = 0; ok && written < count;)
        {
            const ssize_t step =
                write(out, buffer.data() + written, static_cast<size_t>(count - written));
            ok = step > 0;
            written += ok ? step : 0;
        }
        copied += static_cast<std::uint64_t>(count);
        state.done += static_cast<std::uint64_t>(count);
        progress(state);
    }
    ok = ok && fsync(out) == 0;
    close(in);
    if (close(out) != 0 || !ok || copied != size)
    {
        error = "Copying " + state.file + " failed: " + std::strerror(errno ? errno : EIO);
        return false;
    }
    return true;
}
} // namespace

bool litter(const std::string &name)
{
    return name.starts_with("._") || name == ".DS_Store" || name == ".Spotlight-V100" ||
           name == ".fseventsd" || name == ".Trashes" || name == "Thumbs.db" ||
           name == "desktop.ini";
}

bool walk(const std::string &root, std::vector<std::pair<std::string, std::uint64_t>> &files,
          std::vector<std::string> &dirs, std::string &error)
{
    return walk_in(root, "", files, dirs, error);
}

bool remove_all(const std::string &path)
{
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(info.st_mode))
        return unlink(path.c_str()) == 0;
    DIR *directory = opendir(path.c_str());
    if (!directory)
        return false;
    bool ok = true;
    for (const auto &name : names_in(directory, false))
        ok = remove_all(path + "/" + name) && ok;
    return rmdir(path.c_str()) == 0 && ok;
}

bool copy_tree(const std::string &from, const std::string &to,
               const std::function<void(const CopyProgress &)> &progress,
               const std::atomic<bool> &cancel, std::string &error)
{
    std::vector<std::pair<std::string, std::uint64_t>> files;
    std::vector<std::string> dirs;
    if (!walk(from, files, dirs, error))
        return false;
    CopyProgress state;
    for (const auto &file : files)
        state.total += file.second;
    if (!remove_all(to) || mkdir(to.c_str(), 0777) != 0)
    {
        error = "Can't make " + to + ": " + std::strerror(errno);
        return false;
    }
    const auto fail = [&]
    {
        remove_all(to);
        return false;
    };
    for (const auto &dir : dirs)
        if (mkdir((to + "/" + dir).c_str(), 0777) != 0)
        {
            error = "Can't make " + dir + ": " + std::strerror(errno);
            return fail();
        }
    std::vector<char> buffer(4u << 20);
    for (const auto &[name, size] : files)
    {
        state.file = name;
        progress(state);
        if (!copy_file(from + "/" + name, to + "/" + name, size, buffer, state, progress, cancel,
                       error))
            return fail();
    }
    state.file.clear();
    progress(state);
    return true;
}
} // namespace store::library
