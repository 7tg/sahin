// Prospero BlackBox - The user's clean copy of a game: found, checked, imported.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/base_game.hpp"
#include "library/tree.hpp"
#include "third_party/miniz/miniz.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store::library
{
namespace
{
// ---- the games ----------------------------------------------------------------

// Blur (2010) for PC: one release, 1.2. The exe is never checked (any DRM-free exe is
// accepted); cache/root.pak tells a known copy, and an unknown one is installed with a warning.
constexpr const char *kBlurFolders[] = {"cache", "levels", "UI"};
constexpr const char *kBlurFiles[] = {"Blur.exe", "binkw32.dll", "cache/root.pak",
                                      "cache/gamedata.pak", "cache/boot.pak"};
constexpr Release kBlurReleases[] = {
    {"1.2", 559652864, 0x1deb5c87, false},
};
constexpr const char *kBlurModFiles[] = {"scripts", "amax", "dinput8.dll", "d3d9.dll", "dsound.dll"};
constexpr Manifest kBlur = []
{
    Manifest m{"BLUR", kBlurFolders, kBlurFiles, "cache/root.pak", kBlurReleases, kBlurModFiles};
    m.mods_left_out = true;
    m.accept_unknown = true;
    return m;
}();

constexpr const Manifest *kManifests[] = {&kBlur};

// ---- files ----------------------------------------------------------------------

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

// One name in a folder, matched without regard to case ("" when none).
std::string find_name(const std::string &folder, const std::string &name)
{
    DIR *directory = opendir(folder.c_str());
    if (!directory)
        return {};
    std::string found;
    while (const dirent *entry = readdir(directory))
        if (strcasecmp(entry->d_name, name.c_str()) == 0)
        {
            found = entry->d_name;
            if (found == name)
                break; // an exact match wins over one that differs only in case
        }
    closedir(directory);
    return found;
}

bool crc_of(const std::string &path, std::uint32_t &crc)
{
    const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (file < 0)
        return false;
    std::vector<unsigned char> buffer(1u << 20);
    mz_ulong value = mz_crc32(0, nullptr, 0);
    ssize_t count = 0;
    while ((count = read(file, buffer.data(), buffer.size())) > 0)
        value = mz_crc32(value, buffer.data(), static_cast<size_t>(count));
    close(file);
    crc = static_cast<std::uint32_t>(value);
    return count == 0;
}

std::string join(const std::vector<std::string> &items)
{
    std::string text;
    for (std::size_t i = 0; i < items.size(); ++i)
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return text;
}

} // namespace

const Manifest *manifest_for(const std::string &folder)
{
    for (const auto *manifest : kManifests)
        if (folder == manifest->folder)
            return manifest;
    return nullptr;
}

std::string find_entry(const std::string &folder, const std::string &relative)
{
    std::string path = folder;
    std::size_t at = 0;
    while (at <= relative.size())
    {
        const auto slash = std::min(relative.find('/', at), relative.size());
        const auto name = find_name(path, relative.substr(at, slash - at));
        if (name.empty())
            return {};
        path += "/" + name;
        at = slash + 1;
    }
    return path;
}

BaseGame inspect(const std::string &folder, const Manifest &manifest)
{
    BaseGame result;
    result.path = folder;
    if (!is_directory(folder))
        return result;
    std::vector<std::string> missing;
    for (const auto *name : manifest.folders)
        if (const auto path = find_entry(folder, name); path.empty() || !is_directory(path))
            missing.emplace_back(name);
    for (const auto *name : manifest.files)
        if (const auto path = find_entry(folder, name); path.empty() || !is_file(path))
            missing.emplace_back(name);
    if (!missing.empty())
    {
        result.state = State::incomplete;
        result.detail = "Missing " + join(missing) + ". Copy the whole game folder.";
        return result;
    }
    std::vector<std::string> mods;
    for (const auto *name : manifest.mod_files)
        if (!find_entry(folder, name).empty())
            mods.emplace_back(name);
    std::string left; // what the page adds about mods that are left out
    if (!mods.empty() && manifest.mods_left_out)
        left = " Mods found (" + join(mods) + ") are left out: the install adds its own.";
    else if (!mods.empty())
    {
        result.state = State::modified;
        result.detail = "Mods are installed in it (" + join(mods) +
                        "). Copy a clean install: the mods are added for you.";
        return result;
    }
    const auto version_path = find_entry(folder, manifest.version_file);
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    if (!is_file(version_path, &size) || !crc_of(version_path, crc))
    {
        result.state = State::incomplete;
        result.detail = std::string("Can't read ") + manifest.version_file + ".";
        return result;
    }
    for (const auto &release : manifest.releases)
        if (release.size == size && release.crc == crc)
        {
            result.state = release.needs_patch ? State::needs_patch : State::ready;
            result.version = release.version;
            result.detail = release.needs_patch
                                ? "Version " + result.version + ": upgraded to " +
                                      manifest.releases.front().version + " as it installs."
                                : "Version " + result.version + ".";
            result.detail += left;
            return result;
        }
    if (manifest.accept_unknown)
    {
        result.state = State::ready;
        result.version = manifest.releases.front().version;
        result.warning = std::string(manifest.version_file) +
                         " is not the one we know: installing anyway.";
        result.detail = "Version " + result.version + " (unchecked)." + left;
        return result;
    }
    result.state = State::unknown;
    result.detail = std::string(manifest.version_file) +
                    " matches no release we know: the copy is modified, or it is a release we "
                    "haven't seen. Copy a clean install.";
    return result;
}

BaseGame locate(const std::string &data_root, std::span<const std::string> roots,
                const Manifest &manifest)
{
    const auto base = data_root + "/base-games/" + manifest.folder;
    if (is_directory(base))
        return inspect(base, manifest);
    for (const auto &root : roots)
        if (const auto path = find_entry(root, manifest.folder); !path.empty() && is_directory(path))
        {
            auto found = inspect(path, manifest);
            found.on_usb = true;
            return found;
        }
    BaseGame absent;
    absent.path = base;
    absent.detail = "Put a clean install in " + base + " over FTP, or on a USB drive in a folder "
                    "named " + manifest.folder + ".";
    return absent;
}

std::vector<std::string> usb_roots()
{
    std::vector<std::string> roots;
    for (int i = 0; i < 8; ++i)
        if (const auto path = "/mnt/usb" + std::to_string(i); is_directory(path))
            roots.push_back(path);
    for (int i = 0; i < 2; ++i)
        if (const auto path = "/mnt/ext" + std::to_string(i); is_directory(path))
            roots.push_back(path);
    return roots;
}

bool import_base(const std::string &from, const std::string &data_root, const Manifest &manifest,
                 const std::function<void(const ImportProgress &)> &progress,
                 const std::atomic<bool> &cancel, std::string &error)
{
    const auto games = data_root + "/base-games";
    const auto target = games + "/" + manifest.folder, work = target + ".importing";
    if (is_directory(target))
    {
        error = target + " already exists";
        return false;
    }
    if (mkdir(games.c_str(), 0777) != 0 && errno != EEXIST)
    {
        error = "Can't make " + games + ": " + std::strerror(errno);
        return false;
    }
    // A copy left by an earlier attempt is started again.
    if (!copy_tree(from, work, progress, cancel, error))
        return false;
    // Checked as a whole before it takes the place of the base copy.
    const auto copy = inspect(work, manifest);
    if (copy.state != State::ready && copy.state != State::needs_patch)
        error = "The copy didn't check out: " + copy.detail;
    else if (rename(work.c_str(), target.c_str()) != 0)
        error = "Can't move the copy into place: " + std::string(std::strerror(errno));
    else
        return true;
    remove_all(work);
    return false;
}
} // namespace store::library
