// Prospero BlackBox - A game installed into the shared prefix from its base copy and a plan.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/game_install.hpp"
#include "core/save_file.hpp"
#include "library/tree.hpp"
#include "library/rtpatch.hpp"
#include "library/upgrade.hpp"
#include "library/wine_registry.hpp"
#include "third_party/miniz/miniz.h"
#include "third_party/yyjson/yyjson.h"
#include "library/sha256_hex.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <strings.h>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

namespace store::library
{
namespace
{
bool is_directory(const std::string &path)
{
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_file(const std::string &path, std::uint64_t *size = nullptr)
{
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    if (size)
        *size = static_cast<std::uint64_t>(info.st_size);
    return true;
}

std::vector<std::string> names_in(const std::string &folder)
{
    std::vector<std::string> names;
    if (DIR *directory = opendir(folder.c_str()))
    {
        while (const dirent *entry = readdir(directory))
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                names.emplace_back(entry->d_name);
        closedir(directory);
    }
    return names;
}

// root/relative, each part spelled as the folder already spells it (the base copy may
// say GLOBAL or global; Wine doesn't mind, the console's file system does). Missing
// folders on the way are made when make is set.
bool resolve(const std::string &root, const std::string &relative, bool make, std::string &out)
{
    out = root;
    std::size_t start = 0;
    while (start < relative.size())
    {
        auto end = relative.find('/', start);
        const bool last = end == std::string::npos;
        end = last ? relative.size() : end;
        std::string part = relative.substr(start, end - start);
        for (const auto &name : names_in(out))
            if (strcasecmp(name.c_str(), part.c_str()) == 0)
            {
                part = name;
                break;
            }
        out += "/" + part;
        if (!last && make && !is_directory(out) && mkdir(out.c_str(), 0777) != 0)
            return false;
        start = end + 1;
    }
    return true;
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

// Bytes to a new file at path (whatever was there is replaced), synced.
class Output
{
  public:
    explicit Output(const std::string &path) : path_(path)
    {
        unlink(path.c_str());
        file_ = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    }
    ~Output()
    {
        if (file_ >= 0)
            close(file_);
    }
    bool ok() const { return file_ >= 0; }
    // Told the bytes of each write, for a step's progress.
    void count_with(const std::function<void(std::uint64_t)> &moved) { moved_ = &moved; }
    // Written in pieces of up to 1 MiB: a zip is unpacked in 64 KiB pieces, and writes that
    // small ran at 18 MB/s on the console where 1 MiB copies ran at 135 MB/s.
    bool write(const void *data, std::size_t size)
    {
        if (moved_ && *moved_)
            (*moved_)(size);
        const auto *bytes = static_cast<const char *>(data);
        if (pending_.size() + size > kPiece && !flush())
            return false;
        if (size >= kPiece)
            return put(bytes, size);
        pending_.insert(pending_.end(), bytes, bytes + size);
        return true;
    }
    bool finish()
    {
        const bool flushed = flush();
        const bool synced = flushed && fsync(file_) == 0;
        const bool closed = close(file_) == 0;
        file_ = -1;
        return synced && closed;
    }

  private:
    static constexpr std::size_t kPiece = 1u << 20;
    bool put(const char *bytes, std::size_t size)
    {
        while (size > 0)
        {
            const ssize_t step = ::write(file_, bytes, size);
            if (step <= 0)
                return false;
            bytes += step;
            size -= static_cast<std::size_t>(step);
        }
        return true;
    }
    bool flush()
    {
        const bool ok = put(pending_.data(), pending_.size());
        pending_.clear();
        return ok;
    }
    std::string path_;
    int file_ = -1;
    std::vector<char> pending_;
    const std::function<void(std::uint64_t)> *moved_ = nullptr;
};

bool copy_one(const std::string &from, const std::string &to, std::string &error,
              const std::function<void(std::uint64_t)> &moved = {})
{
    const int in = open(from.c_str(), O_RDONLY | O_NOFOLLOW);
    Output out(to);
    out.count_with(moved);
    if (in < 0 || !out.ok())
    {
        if (in >= 0)
            close(in);
        error = "Can't write " + to + ": " + std::strerror(errno);
        return false;
    }
    std::vector<char> buffer(1u << 20);
    ssize_t count = 0;
    bool ok = true;
    while (ok && (count = read(in, buffer.data(), buffer.size())) > 0)
        ok = out.write(buffer.data(), static_cast<std::size_t>(count));
    close(in);
    if (!ok || count != 0 || !out.finish())
    {
        error = "Can't write " + to + ": " + std::strerror(errno ? errno : EIO);
        return false;
    }
    return true;
}

std::size_t to_output(void *opaque, mz_uint64, const void *data, std::size_t size)
{
    return static_cast<Output *>(opaque)->write(data, size) ? size : 0;
}

struct Zip
{
    mz_zip_archive archive{};
    bool open = false;
    std::FILE *file = nullptr;
    ~Zip()
    {
        if (open)
            mz_zip_reader_end(&archive);
        if (file)
            std::fclose(file);
    }
    // Read through a 1 MiB buffer: miniz asks for 64 KiB at a time.
    bool open_buffered(const std::string &path)
    {
        file = std::fopen(path.c_str(), "rb");
        if (!file || std::setvbuf(file, nullptr, _IOFBF, 1u << 20) != 0)
            return false;
        open = mz_zip_reader_init_cfile(&archive, file, 0, 0);
        return open;
    }
};

// Where a zip member goes for a placement (relative to the game folder), or "" when the
// placement doesn't take it.
std::string member_target(const Placement &placement, const std::string &name)
{
    if (placement.member.ends_with('/'))
        return name.starts_with(placement.member) ? placement.to + name.substr(placement.member.size())
                                                  : std::string();
    return name == placement.member ? placement.to : std::string();
}

// The bytes a placement writes: the file's size, or its members' unpacked sizes.
std::uint64_t placement_bytes(const Placement &placement, const std::string &file)
{
    if (placement.member.empty())
    {
        struct stat info{};
        return stat(file.c_str(), &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    }
    Zip zip;
    zip.open = mz_zip_reader_init_file(&zip.archive, file.c_str(), 0);
    std::uint64_t total = 0;
    const auto count = zip.open ? mz_zip_reader_get_num_files(&zip.archive) : 0;
    for (mz_uint index = 0; index < count; ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&zip.archive, index, &stat) && !stat.m_is_directory &&
            !member_target(placement, stat.m_filename).empty())
            total += stat.m_uncomp_size;
    }
    return total;
}

// One placement: the source's file, or what its zip holds, under root. moved is told the
// bytes as they are written.
bool place(const Placement &placement, const std::string &file, const std::string &root,
           std::vector<std::string> *written, std::string &error,
           const std::function<void(std::uint64_t)> &moved = {})
{
    const auto target = [&](const std::string &relative, std::string &path)
    {
        if (!resolve(root, relative, true, path))
        {
            error = "Can't make the folders for " + relative + ": " + std::strerror(errno);
            return false;
        }
        if (written)
            written->push_back(relative);
        return true;
    };
    std::string path;
    if (placement.member.empty())
        return target(placement.to, path) && copy_one(file, path, error, moved);
    Zip zip;
    if (!zip.open_buffered(file))
    {
        error = file + " is not a zip";
        return false;
    }
    std::size_t placed = 0;
    const auto count = mz_zip_reader_get_num_files(&zip.archive);
    for (mz_uint index = 0; index < count; ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip.archive, index, &stat) || stat.m_is_directory)
            continue;
        const std::string name = stat.m_filename;
        const std::string relative = member_target(placement, name);
        if (relative.empty())
            continue;
        if (!safe_relative(relative))
        {
            error = "The zip holds " + name + ", which isn't a plain path";
            return false;
        }
        if (!target(relative, path))
            return false;
        Output out(path);
        out.count_with(moved);
        if (!out.ok() || !mz_zip_reader_extract_to_callback(&zip.archive, index, to_output, &out, 0) ||
            !out.finish())
        {
            error = "Can't unpack " + name + " to " + relative;
            return false;
        }
        ++placed;
    }
    if (placed == 0)
    {
        error = "The zip has no " + placement.member;
        return false;
    }
    return true;
}

bool edit_ini(const std::string &path, const IniEdit &edit, std::string &error)
{
    std::string text;
    if (!hui::save::read_file(path, &text, 4u << 20))
    {
        error = "Can't read " + edit.file;
        return false;
    }
    for (const auto &[key, value] : edit.values)
    {
        // The value after "key =", up to the spaces or comment that follow it: the rest of
        // the line, comments included, stays as it was.
        bool found = false;
        std::size_t at = 0;
        while (at < text.size() && !found)
        {
            auto end = text.find('\n', at);
            end = end == std::string::npos ? text.size() : end;
            std::size_t cursor = at;
            while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
            if (text.compare(cursor, key.size(), key) == 0)
            {
                std::size_t after = cursor + key.size();
                while (after < end && (text[after] == ' ' || text[after] == '\t'))
                    ++after;
                if (after < end && text[after] == '=')
                {
                    ++after;
                    while (after < end && (text[after] == ' ' || text[after] == '\t'))
                        ++after;
                    std::size_t stop = after;
                    while (stop < end && text[stop] != ' ' && text[stop] != '\t' &&
                           text[stop] != ';' && text[stop] != '#' && text[stop] != '\r')
                        ++stop;
                    text.replace(after, stop - after, value);
                    found = true;
                }
            }
            at = end + 1;
        }
        if (!found)
        {
            error = edit.file + " has no " + key;
            return false;
        }
    }
    if (edit.lf)
        std::erase(text, '\r');
    if (const auto failed = hui::save::write_atomic(path, text); !failed.empty())
    {
        error = "Can't write " + edit.file + ": " + failed;
        return false;
    }
    return true;
}

bool patch_bytes(const std::string &path, const BytePatch &patch, std::string &error)
{
    const int file = open(path.c_str(), O_RDWR | O_NOFOLLOW);
    std::string found(patch.from.size(), '\0');
    const auto at = static_cast<off_t>(patch.offset);
    const bool read_ok =
        file >= 0 && pread(file, found.data(), found.size(), at) == static_cast<ssize_t>(found.size());
    if (!read_ok || found != patch.from)
    {
        if (file >= 0)
            close(file);
        error = patch.file + " is not the file its patch is for";
        return false;
    }
    const bool ok = pwrite(file, patch.to.data(), patch.to.size(), at) ==
                        static_cast<ssize_t>(patch.to.size()) &&
                    fsync(file) == 0;
    if (close(file) != 0 || !ok)
    {
        error = "Can't patch " + patch.file + ": " + std::strerror(errno ? errno : EIO);
        return false;
    }
    return true;
}

std::string json_text(const std::string &text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            out += escaped;
        }
        else
            out += c;
    }
    return out + "\"";
}

bool apply_registry(WineRegistry &registry, const std::vector<RegistryValue> &values,
                    std::time_t now, std::string &error)
{
    for (const auto &value : values)
        if (value.dword ? !registry.set_dword(value.key, value.name, value.number, now)
                        : !registry.set_string(value.key, value.name, value.text, now))
        {
            error = "Can't set " + value.key + "\\" + value.name + " (not printable ASCII)";
            return false;
        }
    return true;
}
} // namespace

std::string find_cached(const std::string &cache, const Source &source,
                        const std::atomic<bool> &cancel)
{
    for (const auto &name : names_in(cache))
    {
        if (!name.starts_with(source.sha256) ||
            (name.size() > 64 && name[64] != '.'))
            continue;
        const auto path = cache + "/" + name;
        std::uint64_t size = 0;
        std::string hex;
        if (is_file(path, &size) && size == source.size && sha256_of(path, hex, cancel) &&
            hex == source.sha256)
            return path;
    }
    return {};
}

std::vector<const Source *> missing_sources(const InstallPlan &plan, const std::string &cache,
                                            const std::atomic<bool> &cancel)
{
    std::vector<const Source *> missing;
    std::set<std::string> seen;
    for (const auto *list : {&plan.game_files, &plan.system_files})
        for (const auto &placement : *list)
            if (seen.insert(placement.source).second)
                if (const auto *source = plan.source(placement.source);
                    source && find_cached(cache, *source, cancel).empty())
                    missing.push_back(source);
    return missing;
}

std::string record_path(const InstallPlan &plan, const InstallPaths &paths)
{
    return paths.data_root + "/installs/" + plan.prefix + "/" + plan.game + ".json";
}

InstallPlan effective_plan(const InstallPlan &plan, const InstallOptions &options)
{
    InstallPlan out = plan;
    if (!options.prefix.empty())
        out.prefix = out.profile.prefix = options.prefix;
    if (!options.profile_id.empty())
        out.profile.id = options.profile_id;
    if (!options.profile_name.empty())
        out.profile.name = options.profile_name;
    for (const auto &option : plan.options)
    {
        if (std::find(options.chosen.begin(), options.chosen.end(), option.id) == options.chosen.end())
            continue;
        out.game_files.insert(out.game_files.end(), option.game_files.begin(), option.game_files.end());
        out.system_files.insert(out.system_files.end(), option.system_files.begin(),
                                option.system_files.end());
        out.texts.insert(out.texts.end(), option.texts.begin(), option.texts.end());
        out.ini.insert(out.ini.end(), option.ini.begin(), option.ini.end());
        out.expect.insert(out.expect.end(), option.expect.begin(), option.expect.end());
        out.profile.dll_overrides += option.dll_overrides;
    }
    out.options.clear(); // added: an install of the result has none left to choose
    if (options.cursor == Cursor::touchpad)
    {
        out.ini.insert(out.ini.end(), plan.touchpad_ini.begin(), plan.touchpad_ini.end());
        out.profile.touchpad_mouse = true;
    }
    out.touchpad_ini.clear();
    if (options.overlay == Overlay::fps)
        out.profile.show_fps = true;
    else if (options.overlay == Overlay::full)
        out.profile.debug_env.emplace_back("DXVK_HUD", kFullOverlay);
    return out;
}

const char *overlay_name(Overlay overlay)
{
    return overlay == Overlay::fps ? "fps" : overlay == Overlay::full ? "full" : "off";
}

Overlay overlay_named(std::string_view name)
{
    return name == "fps" ? Overlay::fps : name == "full" ? Overlay::full : Overlay::off;
}

const char *cursor_name(Cursor cursor)
{
    return cursor == Cursor::touchpad ? "touchpad" : "off";
}

Cursor cursor_named(std::string_view name)
{
    return name == "touchpad" ? Cursor::touchpad : Cursor::off;
}

bool apply_ini(const InstallPlan &plan, const InstallPaths &paths, std::string &error)
{
    std::string games;
    if (!resolve(paths.win_root + "/prefixes/" + plan.prefix, "drive_c/Games", false, games) ||
        !is_directory(games + "/" + plan.game))
    {
        error = plan.title + " isn't installed in " + plan.prefix;
        return false;
    }
    for (const auto &edit : plan.ini)
    {
        std::string path;
        if (!resolve(games + "/" + plan.game, edit.file, false, path) || !edit_ini(path, edit, error))
        {
            if (error.empty())
                error = "Can't find " + edit.file;
            return false;
        }
    }
    return true;
}

std::vector<OptionView> option_views(const InstallPlan &plan, const InstallPaths &paths,
                                     const InstallOptions &options)
{
    std::vector<OptionView> views;
    for (const auto &option : plan.options)
        views.push_back({option.id, option.title, option.description, option.on});
    // The record's "options", when there is a record that reads.
    std::string text;
    if (views.empty() ||
        !hui::save::read_file(record_path(effective_plan(plan, options), paths), &text, 1u << 20))
        return views;
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc{
        yyjson_read(text.data(), text.size(), 0), yyjson_doc_free};
    auto *chosen = doc ? yyjson_obj_get(yyjson_doc_get_root(doc.get()), "options") : nullptr;
    if (!yyjson_is_arr(chosen))
        return views; // a record from before options: all off, as it was installed
    for (auto &view : views)
    {
        view.on = false;
        std::size_t index = 0, count = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(chosen, index, count, item)
            if (yyjson_is_str(item) && view.id == yyjson_get_str(item))
                view.on = true;
    }
    return views;
}

bool install_game(const InstallPlan &given, const Manifest &manifest, const InstallPaths &paths,
                  const InstallOptions &options,
                  const std::function<void(const InstallStatus &)> &progress,
                  const std::atomic<bool> &cancel, std::string &error)
{
    const auto plan = effective_plan(given, options);
    if (std::string unused; !profile_text(plan.profile, unused, error))
        return false;
    InstallStatus status;
    const auto step = [&](std::string name, std::uint64_t total = 0)
    {
        status = {std::move(name), 0, total};
        progress(status);
    };

    // ---- everything checked before anything changes ----
    step("Checking the base copy");
    const auto base = locate(paths.data_root, {}, manifest);
    if (base.on_usb || (base.state != State::ready && base.state != State::needs_patch))
    {
        error = base.state == State::absent ? "There's no base copy in base-games yet"
                                            : "The base copy isn't ready: " + base.detail;
        return false;
    }
    // An older release is brought up to the plan's by its upgrade file, in the copy.
    std::string upgrade;
    bool rtpatch = false;
    if (base.version != plan.release)
    {
        for (const auto &candidate : plan.upgrades)
            if (candidate.from == base.version)
            {
                upgrade = candidate.source;
                rtpatch = candidate.rtpatch;
            }
        if (upgrade.empty())
        {
            error = "The base copy is version " + base.version + ", and there's no upgrade from "
                    "it to " + plan.release + ". Copy a " + plan.release + " install.";
            return false;
        }
    }
    if (!plan.pending.empty() && !options.without_pending)
    {
        error = "Not everything can be installed yet:";
        for (const auto &piece : plan.pending)
            error += "\n- " + piece;
        return false;
    }
    const auto prefix = paths.win_root + "/prefixes/" + plan.prefix;
    std::string windows;
    if (!is_file(prefix + "/system.reg") || !is_file(prefix + "/user.reg") ||
        !resolve(prefix, "drive_c/windows", false, windows) || !is_directory(windows))
    {
        error = "The Wine prefix " + prefix + " isn't there (or has no registry)";
        return false;
    }
    WineRegistry machine, user;
    if (!load_registry(prefix + "/system.reg", machine, error) ||
        !load_registry(prefix + "/user.reg", user, error))
        return false;

    std::uint64_t needed = 0;
    std::vector<std::string> used;
    for (const auto *list : {&plan.game_files, &plan.system_files})
        for (const auto &placement : *list)
            used.push_back(placement.source);
    if (!upgrade.empty())
        used.push_back(upgrade);
    {
        std::set<std::string> counted;
        for (const auto &id : used)
            if (counted.insert(id).second)
                needed += plan.source(id)->size;
    }
    step("Checking the mod files", needed);
    const auto cache = paths.data_root + "/mods-cache";
    std::map<std::string, std::string> files; // source id -> its file in the cache
    std::vector<const Source *> missing;
    for (const auto &id : used)
        if (!files.count(id))
        {
            const auto *source = plan.source(id);
            std::string file;
            struct stat info{};
            const auto named = cache + "/" + source->sha256;
            if (options.verified.count(source->sha256) && lstat(named.c_str(), &info) == 0 &&
                S_ISREG(info.st_mode) && static_cast<std::uint64_t>(info.st_size) == source->size)
                file = named; // hashed whole by this job already
            else
                file = find_cached(cache, *source, cancel);
            if (cancel.load())
            {
                error = "Cancelled";
                return false;
            }
            if (file.empty())
                missing.push_back(source);
            files[id] = std::move(file);
            status.done += source->size;
            progress(status);
        }
    if (!missing.empty())
    {
        error = "mods-cache lacks " + std::to_string(missing.size()) + " file" +
                (missing.size() == 1 ? "" : "s") + ":";
        for (const auto *source : missing)
            error += "\n- " + source->name + " (" + source->sha256 + ")";
        return false;
    }

    // ---- the game's folder, built beside the old one ----
    std::string games;
    if (!resolve(prefix, "drive_c/Games", true, games) ||
        (!is_directory(games) && mkdir(games.c_str(), 0777) != 0))
    {
        error = "Can't make " + games + ": " + std::strerror(errno);
        return false;
    }
    const auto target = games + "/" + plan.game, work = target + ".installing",
               old = target + ".old";
    // A swap cut short: the old folder goes back if it's the only one.
    if (is_directory(old) && !is_directory(target))
        rename(old.c_str(), target.c_str());
    remove_all(old);

    step("Copying the base copy");
    if (!copy_tree(
            base.path, work,
            [&](const CopyProgress &copy)
            {
                status.done = copy.done;
                status.total = copy.total;
                progress(status);
            },
            cancel, error))
        return false;
    const auto fail = [&]
    {
        remove_all(work);
        return false;
    };
    // What earlier mod installs left in the copy goes: the plan adds its own.
    for (const auto &relative : plan.strip)
        if (const auto found = find_entry(work, relative); !found.empty())
            remove_all(found);
    if (!upgrade.empty())
    {
        step("Upgrading to " + plan.release);
        if (rtpatch ? !apply_rtpatch(files[upgrade], work, cancel, error)
                    : !apply_upgrade(files[upgrade], work, plan.game, base.version, plan.release, cancel,
                                     error))
            return fail();
        const auto upgraded = inspect(work, manifest);
        if (upgraded.state != State::ready || upgraded.version != plan.release)
        {
            error = "The upgraded copy isn't " + plan.release + ": " + upgraded.detail;
            return fail();
        }
    }
    // In bytes: the Xbox 360 zips are gigabytes each, so a count of files would sit still.
    std::uint64_t adding = 0;
    for (const auto &placement : plan.game_files)
        adding += placement_bytes(placement, files[placement.source]);
    step("Adding the mods", adding);
    std::uint64_t reported = 0;
    const std::function<void(std::uint64_t)> moved = [&](std::uint64_t bytes)
    {
        status.done = std::min(status.total, status.done + bytes);
        if (status.done - reported >= (8u << 20) || status.done == status.total)
        {
            reported = status.done;
            progress(status);
        }
    };
    for (const auto &placement : plan.game_files)
    {
        if (cancel.load())
        {
            error = "Cancelled";
            return fail();
        }
        if (!place(placement, files[placement.source], work, nullptr, error, moved))
            return fail();
    }
    status.done = status.total;
    progress(status);
    step("Settings");
    for (const auto &patch : plan.patches)
    {
        std::string path;
        if (!resolve(work, patch.file, false, path) || !patch_bytes(path, patch, error))
            return fail();
    }
    for (const auto &edit : plan.ini)
    {
        std::string path;
        if (!resolve(work, edit.file, false, path) || !edit_ini(path, edit, error))
            return fail();
    }
    for (const auto &text : plan.texts)
    {
        std::string path;
        std::string body;
        if (!resolve(work, text.to, true, path) ||
            (text.append && !hui::save::read_file(path, &body, 4u << 20)))
        {
            error = "Can't read " + text.to;
            return fail();
        }
        body += text.body;
        if (const auto failed = hui::save::write_atomic(path, body); !failed.empty())
        {
            error = "Can't write " + text.to + ": " + failed;
            return fail();
        }
    }
    step("Checking the result", plan.expect.size());
    for (const auto &[relative, hash] : plan.expect)
    {
        std::string path, found;
        if (!resolve(work, relative, false, path) || !sha256_of(path, found, cancel) ||
            found != hash)
        {
            error = cancel.load() ? "Cancelled"
                                  : relative + " didn't come out as it should (SHA-256 " +
                                        (found.empty() ? "unreadable" : found) + ")";
            return fail();
        }
        ++status.done;
        progress(status);
    }
    if (is_directory(target) && rename(target.c_str(), old.c_str()) != 0)
    {
        error = "Can't move the old install aside: " + std::string(std::strerror(errno));
        return fail();
    }
    if (rename(work.c_str(), target.c_str()) != 0)
    {
        error = "Can't move the install into place: " + std::string(std::strerror(errno));
        rename(old.c_str(), target.c_str());
        return fail();
    }
    remove_all(old);

    // ---- the prefix: system files, registry, profile ----
    step("Setting up Wine", plan.system_files.size());
    for (const auto &placement : plan.system_files)
    {
        std::vector<std::string> written;
        // Unpacked beside the prefix first, so Wine's own copy is only set aside once the
        // new one is whole.
        const auto staging = prefix + "/.blackbox-staging";
        if (!remove_all(staging) || mkdir(staging.c_str(), 0777) != 0 ||
            !place(placement, files[placement.source], staging, &written, error))
        {
            remove_all(staging);
            if (error.empty())
                error = "Can't make " + staging;
            return false;
        }
        for (const auto &relative : written)
        {
            std::string from, to;
            resolve(staging, relative, false, from);
            if (!resolve(windows, relative, true, to))
            {
                error = "Can't make the folders for " + relative;
                remove_all(staging);
                return false;
            }
            if (placement.keep_builtin && is_file(to) && !is_file(to + ".builtin") &&
                rename(to.c_str(), (to + ".builtin").c_str()) != 0)
            {
                error = "Can't keep Wine's own " + relative + ": " + std::strerror(errno);
                remove_all(staging);
                return false;
            }
            if (rename(from.c_str(), to.c_str()) != 0)
            {
                error = "Can't put " + relative + " in place: " + std::strerror(errno);
                remove_all(staging);
                return false;
            }
        }
        remove_all(staging);
        ++status.done;
        progress(status);
    }
    if (!apply_registry(machine, plan.machine, options.now, error) ||
        !apply_registry(user, plan.user, options.now, error) ||
        !save_registry(prefix + "/system.reg", machine, error) ||
        !save_registry(prefix + "/user.reg", user, error))
        return false;
    if (!install_profile(paths.win_root + "/profiles", plan.profile, plan.profile_after, error))
        return false;

    // ---- what was installed, for repair and updates ----
    std::string record = "{\n  \"schema\": 1,\n  \"game\": " + json_text(plan.game) +
                         ",\n  \"release\": " + json_text(plan.release) +
                         ",\n  \"base_release\": " + json_text(base.version) +
                         ",\n  \"prefix\": " + json_text(plan.prefix) +
                         ",\n  \"profile\": " + json_text(plan.profile.id) +
                         ",\n  \"installed_at\": " + std::to_string(static_cast<long long>(options.now)) +
                         ",\n  \"app_version\": " + json_text(options.app_version) +
                         ",\n  \"without\": [";
    for (std::size_t i = 0; options.without_pending && i < plan.pending.size(); ++i)
        record += (i ? ", " : "") + json_text(plan.pending[i]);
    record += "],\n  \"options\": [";
    bool listed = false;
    for (const auto &option : options.chosen)
        if (given.option(option))
        {
            record += (listed ? ", " : "") + json_text(option);
            listed = true;
        }
    record += "],\n  \"sources\": {";
    bool first = true;
    for (const auto &[id, file] : files)
    {
        record += std::string(first ? "\n" : ",\n") + "    " + json_text(id) + ": " +
                  json_text(plan.source(id)->sha256);
        first = false;
    }
    record += "\n  }\n}\n";
    if (!hui::save::ensure_directory(paths.data_root + "/installs") ||
        !hui::save::ensure_directory(paths.data_root + "/installs/" + plan.prefix))
    {
        error = "Can't make " + paths.data_root + "/installs/" + plan.prefix;
        return false;
    }
    if (const auto failed = hui::save::write_atomic(record_path(plan, paths), record);
        !failed.empty())
    {
        error = "Can't write the install record: " + failed;
        return false;
    }
    step("Installed");
    return true;
}

bool uninstall_game(const InstallPlan &given, const InstallPaths &paths,
                    const InstallOptions &options, std::string &error)
{
    const auto plan = effective_plan(given, options);
    // The profile first, so the launcher never offers a game that's half gone.
    if (!remove_profile(paths.win_root + "/profiles", plan.profile.id, error))
        return false;
    std::string games;
    resolve(paths.win_root + "/prefixes/" + plan.prefix, "drive_c/Games", false, games);
    for (const char *suffix : {"", ".installing", ".old"})
        if (!remove_all(games + "/" + plan.game + suffix))
        {
            error = "Can't remove " + games + "/" + plan.game + suffix;
            return false;
        }
    if (unlink(record_path(plan, paths).c_str()) != 0 && errno != ENOENT)
    {
        error = "Can't remove the install record: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}
} // namespace store::library
