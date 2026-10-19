// Prospero BlackBox - Bounded installed-app inventory; ownership requires a matching receipt.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/inventory.hpp"
#include "catalog/catalog.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace store::system
{
namespace
{
bool checked_path(const std::string &path)
{
    if (!clean_absolute_path(path))
    {
        errno = EINVAL;
        return false;
    }
    std::size_t end = 1;
    for (;;)
    {
        end = path.find('/', end);
        struct stat info
        {
        };
        if (lstat(path.substr(0, end).c_str(), &info) != 0)
            return false;
        if (S_ISLNK(info.st_mode))
        {
            errno = ELOOP;
            return false;
        }
        if (end == std::string::npos)
            return true;
        ++end;
    }
}
bool read_regular(const std::string &path, std::size_t limit, std::string &body)
{
    if (!checked_path(path))
        return false;
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat info
    {
    };
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
              static_cast<std::uint64_t>(info.st_size) <= limit;
    std::string bytes;
    char buffer[4096];
    while (ok)
    {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            ok = count == 0;
            break;
        }
        if (static_cast<std::size_t>(count) > limit - bytes.size())
        {
            ok = false;
            break;
        }
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    if (ok)
        body = std::move(bytes);
    return ok;
}
bool image_file(std::string name)
{
    for (auto &c : name)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 'a' - 'A');
    return name.ends_with(".ffpkg") || name.ends_with(".exfat") || name.ends_with(".ffpfs") ||
           name.ends_with(".ffpfsc");
}
std::string basename(const std::string &path)
{
    return path.substr(path.find_last_of('/') + 1);
}
} // namespace

Inventory scan_installed(const ScanPolicy &policy, const std::string &receipts,
                         const std::atomic<bool> &cancelled)
{
    Inventory result;
    if (policy.depth < 1 || policy.depth > 2 || policy.roots.size() > 256 ||
        policy.manual.size() > 256)
    {
        result.complete = false;
        result.errors.push_back("Invalid installed scan policy");
        return result;
    }
    std::vector<std::pair<std::string, unsigned>> pending;
    for (const auto &root : policy.roots)
        pending.emplace_back(root, policy.depth + (root == "/mnt/shadowmnt/pfsc" ||
                                                           root.starts_with("/mnt/shadowmnt/pfsc/")
                                                       ? 1u
                                                       : 0u));
    for (const auto &path : policy.manual)
        pending.emplace_back(path, 0);
    std::set<std::string> seen;
    std::size_t visited = 0;
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        if (cancelled.load() || visited >= 16384 || result.apps.size() >= 4096)
        {
            result.complete = false;
            result.errors.push_back(cancelled.load() ? "Installed scan cancelled"
                                                     : "Installed scan limit reached");
            break;
        }
        const auto [path, depth] = pending[index];
        if (!seen.insert(path).second)
            continue;
        ++visited;
        struct stat info
        {
        };
        if (!checked_path(path) || lstat(path.c_str(), &info) != 0)
        {
            if (errno != ENOENT && errno != ENOTDIR && errno != ELOOP)
            {
                result.complete = false;
                result.errors.push_back(path + ": " + std::strerror(errno));
            }
            continue;
        }
        if (S_ISREG(info.st_mode) && image_file(path))
        {
            InstalledApp app;
            app.path = path;
            app.name = basename(path);
            app.image = true;
            app.reason = "Image installed outside Sahin. Not managed by this app.";
            result.apps.push_back(std::move(app));
            continue;
        }
        if (!S_ISDIR(info.st_mode))
            continue;
        std::string body, error;
        catalog::Entry metadata;
        const auto metadata_path = path + "/sce_sys/param.json";
        struct stat metadata_info
        {
        };
        const int metadata_status = lstat(metadata_path.c_str(), &metadata_info);
        if (metadata_status == 0)
        {
            if (!read_regular(metadata_path, catalog::kDetailLimit, body) ||
                !catalog::parse_installed(body, metadata, error))
            {
                result.complete = false;
                result.errors.push_back(
                    path + ": " + (error.empty() ? "App metadata is unreadable or unsafe" : error));
                continue;
            }
            InstalledApp app;
            app.id = metadata.id;
            app.name = metadata.name;
            app.version = metadata.content_version;
            app.path = path;
            catalog::Receipt receipt;
            const auto parent = path.substr(0, path.find_last_of('/'));
            app.managed = !receipts.empty() && basename(path) == app.id &&
                          read_regular(receipts + "/" + app.id + ".json", 16 * 1024, body) &&
                          catalog::parse_receipt(body, receipt, error) && receipt.id == app.id &&
                          clean_absolute_path(receipt.location) && receipt.location == parent &&
                          receipt.content_version == app.version;
            if (!app.managed)
                app.reason = "Installed outside Sahin. Not managed by this app.";
            result.apps.push_back(std::move(app));
            continue;
        }
        if (errno != ENOENT && errno != ENOTDIR)
        {
            result.complete = false;
            result.errors.push_back(path + ": App metadata could not be inspected");
            continue;
        }
        if (depth == 0)
            continue;
        std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(path.c_str()), closedir);
        if (!directory)
        {
            result.complete = false;
            result.errors.push_back(path + ": " + std::strerror(errno));
            continue;
        }
        for (;;)
        {
            errno = 0;
            const auto *entry = readdir(directory.get());
            if (!entry)
            {
                if (errno)
                {
                    result.complete = false;
                    result.errors.push_back(path + ": " + std::strerror(errno));
                }
                break;
            }
            if (entry->d_name[0] == '.')
                continue;
            if (std::string_view(entry->d_name) == "backports" &&
                std::find(policy.roots.begin(), policy.roots.end(), path) != policy.roots.end() &&
                path != "/mnt/shadowmnt" && !path.starts_with("/mnt/shadowmnt/"))
                continue;
            if (cancelled.load() || pending.size() >= 16384)
            {
                result.complete = false;
                result.errors.push_back("Installed scan interrupted or limit reached");
                break;
            }
            pending.emplace_back(path + "/" + entry->d_name, depth - 1);
        }
    }
    std::map<std::string, unsigned> counts;
    for (const auto &app : result.apps)
        if (!app.id.empty())
            ++counts[app.id];
    for (auto &app : result.apps)
        if (!app.id.empty() && counts[app.id] > 1)
        {
            app.duplicate = true;
            app.managed = false;
            app.reason = "Duplicate title ID. Resolve the extra copy before managing this app.";
        }
    std::sort(result.apps.begin(), result.apps.end(),
              [](const auto &a, const auto &b) { return a.path < b.path; });
    return result;
}
} // namespace store::system
