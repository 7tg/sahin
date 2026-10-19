// Prospero BlackBox - A game's base copy brought up to its current release by our own diff.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/upgrade.hpp"
#include "library/base_game.hpp"
#include "library/install_plan.hpp"
#include "third_party/miniz/miniz.h"
#include "library/sha256_hex.hpp"
#include "third_party/yyjson/yyjson.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace store::library
{
namespace
{
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

bool member(Zip &zip, const std::string &name, std::string &out)
{
    std::size_t size = 0;
    void *data = mz_zip_reader_extract_file_to_heap(&zip.archive, name.c_str(), &size, 0);
    if (!data)
        return false;
    out.assign(static_cast<const char *>(data), size);
    mz_free(data);
    return true;
}

std::string text(yyjson_val *object, const char *key)
{
    auto *value = yyjson_obj_get(object, key);
    return yyjson_is_str(value) ? std::string(yyjson_get_str(value), yyjson_get_len(value))
                                : std::string();
}

bool sha256_of(const std::string &path, std::string &hex, const std::atomic<bool> &cancel)
{
    const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (file < 0)
        return false;
    picosha2::hash256_one_by_one hasher;
    std::vector<unsigned char> buffer(1u << 20);
    ssize_t count = 0;
    while (!cancel.load() && (count = read(file, buffer.data(), buffer.size())) > 0)
        hasher.process(buffer.begin(), buffer.begin() + count);
    close(file);
    if (count != 0 || cancel.load())
        return false;
    hasher.finish();
    hex = sha256_hex(hasher);
    return true;
}

std::uint64_t u64(const std::string &ops, std::size_t at)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = value << 8 | static_cast<unsigned char>(ops[at + static_cast<std::size_t>(i)]);
    return value;
}

// The new file, built from the old one (old < 0 for a new file) and the ops, into out;
// its SHA-256 in hex.
bool rebuild(int old, std::uint64_t old_size, const std::string &ops, int out, std::string &hex,
             const std::atomic<bool> &cancel, std::string &why)
{
    picosha2::hash256_one_by_one hasher;
    std::vector<char> buffer(1u << 20);
    const auto emit = [&](const char *data, std::size_t size)
    {
        hasher.process(reinterpret_cast<const unsigned char *>(data),
                       reinterpret_cast<const unsigned char *>(data) + size);
        while (size > 0)
        {
            const ssize_t step = write(out, data, size);
            if (step <= 0)
                return false;
            data += step;
            size -= static_cast<std::size_t>(step);
        }
        return true;
    };
    std::size_t at = 0;
    while (at < ops.size())
    {
        if (cancel.load())
        {
            why = "Cancelled";
            return false;
        }
        const char kind = ops[at];
        if ((kind != 'C' && kind != 'A') || ops.size() - at < (kind == 'C' ? 17u : 9u))
        {
            why = "a damaged record";
            return false;
        }
        if (kind == 'A')
        {
            const auto length = u64(ops, at + 1);
            if (length > ops.size() - at - 9)
            {
                why = "a damaged record";
                return false;
            }
            if (!emit(ops.data() + at + 9, static_cast<std::size_t>(length)))
            {
                why = std::strerror(errno);
                return false;
            }
            at += 9 + static_cast<std::size_t>(length);
            continue;
        }
        auto offset = u64(ops, at + 1), length = u64(ops, at + 9);
        at += 17;
        if (old < 0 || offset > old_size || length > old_size - offset)
        {
            why = "a copy from outside the old file";
            return false;
        }
        while (length > 0)
        {
            const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(length, buffer.size()));
            const ssize_t got = pread(old, buffer.data(), step, static_cast<off_t>(offset));
            if (got != static_cast<ssize_t>(step) || !emit(buffer.data(), step))
            {
                why = got != static_cast<ssize_t>(step) ? "a short read" : std::strerror(errno);
                return false;
            }
            offset += step;
            length -= step;
        }
    }
    hasher.finish();
    hex = sha256_hex(hasher);
    return true;
}
} // namespace

bool apply_upgrade(const std::string &upgrade, const std::string &folder, const std::string &game,
                   const std::string &from, const std::string &to,
                   const std::atomic<bool> &cancel, std::string &error)
{
    Zip zip;
    zip.open = mz_zip_reader_init_file(&zip.archive, upgrade.c_str(), 0);
    std::string manifest;
    if (!zip.open || !member(zip, "upgrade.json", manifest))
    {
        error = "The upgrade file is damaged";
        return false;
    }
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc{
        yyjson_read(manifest.data(), manifest.size(), 0), yyjson_doc_free};
    auto *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    auto *files = root ? yyjson_obj_get(root, "files") : nullptr;
    if (!root || yyjson_get_uint(yyjson_obj_get(root, "schema")) != 1 || !yyjson_is_arr(files) ||
        text(root, "game") != game || text(root, "from") != from || text(root, "to") != to)
    {
        error = "The upgrade file isn't " + game + " " + from + " to " + to;
        return false;
    }
    std::size_t index = 0, count = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(files, index, count, item)
    {
        const auto path = text(item, "path"), old_hash = text(item, "from_sha256"),
                   new_hash = text(item, "sha256"), ops_name = text(item, "ops");
        if (!safe_relative(path) || new_hash.size() != 64)
        {
            error = "The upgrade file names " + path + ", which isn't a plain path";
            return false;
        }
        // The old file, spelled as the copy spells it; a new file goes where its folder is.
        std::string target = find_entry(folder, path);
        const bool exists = !target.empty();
        if (!exists)
        {
            // Its folders as the copy spells them, the missing ones made.
            target = folder;
            std::size_t start = 0;
            for (auto slash = path.find('/'); slash != std::string::npos;
                 start = slash + 1, slash = path.find('/', start))
            {
                const auto part = path.substr(start, slash - start);
                const auto found = find_entry(target, part);
                target = found.empty() ? target + "/" + part : found;
                if (found.empty() && mkdir(target.c_str(), 0777) != 0 && errno != EEXIST)
                {
                    error = "Can't make the folders for " + path + ": " + std::strerror(errno);
                    return false;
                }
            }
            target += "/" + path.substr(start);
        }
        if (!old_hash.empty())
        {
            std::string found;
            if (!exists || !sha256_of(target, found, cancel) || found != old_hash)
            {
                error = cancel.load() ? "Cancelled"
                                      : path + " isn't the " + from + " file the upgrade is for";
                return false;
            }
        }
        std::string ops;
        if (!member(zip, ops_name, ops))
        {
            error = "The upgrade file lacks " + ops_name;
            return false;
        }
        const int old = old_hash.empty() ? -1 : open(target.c_str(), O_RDONLY | O_NOFOLLOW);
        struct stat info{};
        if (old >= 0)
            fstat(old, &info);
        const auto building = target + ".upgrading";
        unlink(building.c_str());
        const int out = open(building.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
        std::string made, why;
        bool ok = out >= 0 && (old >= 0 || old_hash.empty()) &&
                  rebuild(old, static_cast<std::uint64_t>(info.st_size), ops, out, made, cancel, why);
        ok = ok && fsync(out) == 0;
        if (old >= 0)
            close(old);
        if (out >= 0 && close(out) != 0)
            ok = false;
        if (!ok || made != new_hash || rename(building.c_str(), target.c_str()) != 0)
        {
            unlink(building.c_str());
            error = why == "Cancelled" ? why
                    : !ok             ? "Can't upgrade " + path + ": " + (why.empty() ? std::strerror(errno) : why)
                    : made != new_hash ? path + " didn't come out as " + to
                                       : "Can't put " + path + " in place: " + std::strerror(errno);
            return false;
        }
    }
    auto *remove = yyjson_obj_get(root, "remove");
    yyjson_arr_foreach(remove, index, count, item)
    {
        const std::string path(yyjson_get_str(item) ? yyjson_get_str(item) : "");
        const auto target = safe_relative(path) ? find_entry(folder, path) : std::string();
        if (!target.empty() && unlink(target.c_str()) != 0)
        {
            error = "Can't remove " + path + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}
} // namespace store::library
