// Prospero BlackBox - Observe filesystem type, space and one directory rename.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/storage_probe.hpp"
#include "system/locations.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
// libkernel exports the underscored ABI; fstatfs belongs to libkernel_sys.
extern "C" int store_fstatfs(int, struct statfs *) __asm__("_fstatfs");
#endif

namespace store::system
{
namespace
{
int directory(const std::string &path, std::string *error = nullptr)
{
    if (!clean_absolute_path(path))
    {
        errno = EINVAL;
        return -1;
    }
    const auto drive = drive_root(path);
    const auto base = drive.empty() ? std::string("/") : drive;
    int fd = open(base.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0 && error)
        *error = "open " + base + ": " + std::strerror(errno);
    std::size_t start = base == "/" ? 1 : base.size() + 1;
    while (fd >= 0 && start < path.size())
    {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash - start);
#if defined(__linux__) && !defined(STORE_NATIVE_PATH_WALK)
        const int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
#else
        // Native _openat returns EINVAL on 6.02. Check every path component
        // through the qualified direct-open API, refusing symlinks throughout.
        const int next = open(path.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
#endif
        const int saved = errno;
        if (next < 0 && error)
            *error = "open component " + part + ": " + std::strerror(saved);
        close(fd);
        errno = saved;
        fd = next;
        if (slash == path.npos)
            break;
        start = slash + 1;
    }
    return fd;
}
} // namespace

bool available_space(const std::string &path, std::uint64_t &bytes)
{
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct statfs filesystem
    {
    };
#ifdef __linux__
    const int measured = fstatfs(fd, &filesystem);
#else
    const int measured = store_fstatfs(fd, &filesystem);
#endif
    close(fd);
    const auto blocks = static_cast<std::uint64_t>(filesystem.f_bavail);
    const auto block_size = static_cast<std::uint64_t>(filesystem.f_bsize);
    if (measured != 0 || block_size == 0 ||
        blocks > std::numeric_limits<std::uint64_t>::max() / block_size)
        return false;
    bytes = blocks * block_size;
    return true;
}

StorageProbe probe_storage(const std::string &root)
{
    StorageProbe result;
    const int fd = directory(root, &result.error);
    if (fd < 0)
    {
        if (result.error.empty())
            result.error = std::strerror(errno);
        return result;
    }
    struct statfs filesystem
    {
    };
#ifdef __linux__
    const int measured = fstatfs(fd, &filesystem);
    result.filesystem = std::to_string(filesystem.f_type);
#else
    const int measured = store_fstatfs(fd, &filesystem);
    result.filesystem.assign(filesystem.f_fstypename,
                             strnlen(filesystem.f_fstypename, sizeof(filesystem.f_fstypename)));
#endif
    if (measured != 0)
    {
        result.error = "Filesystem information unavailable";
        close(fd);
        return result;
    }
    const auto blocks = static_cast<std::uint64_t>(filesystem.f_bavail);
    const auto block_size = static_cast<std::uint64_t>(filesystem.f_bsize);
    if (block_size && blocks <= std::numeric_limits<std::uint64_t>::max() / block_size)
        result.available = blocks * block_size;
    static std::atomic<unsigned> serial{0};
    const auto first = root + "/.prospero-blackbox-probe-" + std::to_string(getpid()) + "-" +
                       std::to_string(serial.fetch_add(1));
    const auto second = first + "-renamed";
    struct stat before
    {
    }, after{}, parent{};
    const bool parent_known = fstat(fd, &parent) == 0;
    struct stat named_parent
    {
    };
    const int check = directory(root);
    const bool same_parent = check >= 0 && fstat(check, &named_parent) == 0 && parent_known &&
                             parent.st_dev == named_parent.st_dev &&
                             parent.st_ino == named_parent.st_ino;
    if (check >= 0)
        close(check);
    bool created = false, moved = false;
    if (!same_parent)
        result.error = "The install directory changed during the probe";
    else if (mkdir(first.c_str(), 0700) != 0)
        result.error = std::strerror(errno);
    else
    {
        created = true;
        const int child = directory(first);
        if (child < 0 || fstat(child, &before) != 0 || before.st_dev != parent.st_dev)
            result.error = "The test directory could not be verified";
        else
        {
            // Reserve the destination too: never overwrite another process's directory.
            if (mkdir(second.c_str(), 0700) != 0)
                result.error = "The rename destination is unavailable";
            else
            {
                if (rename(first.c_str(), second.c_str()) != 0)
                    result.error = std::strerror(errno);
                else
                {
                    moved = true;
                    const int renamed = directory(second);
                    if (renamed < 0 || fstat(renamed, &after) != 0 ||
                        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
                        fsync(fd) != 0)
                        result.error =
                            "The renamed directory could not be verified or synchronized";
                    else
                        result.renamed = true;
                    if (renamed >= 0)
                        close(renamed);
                }
                if (rmdir(second.c_str()) != 0)
                    result.error = "The probe directory could not be removed";
            }
        }
        if (child >= 0)
            close(child);
    }
    if (created && !moved && rmdir(first.c_str()) != 0)
        result.error = "The probe directory could not be removed";
    if (created && fsync(fd) != 0)
        result.error = "The probe cleanup could not be synchronized";
    close(fd);
    result.renamed = result.renamed && result.error.empty();
    return result;
}
} // namespace store::system
