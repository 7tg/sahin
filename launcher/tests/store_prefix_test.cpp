// Prospero BlackBox - The shared prefix made from the bare prefix the manager carries.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/prefix.hpp"
#include "third_party/miniz/miniz.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace store::library;

namespace
{
std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A zip of (name, body) entries; a name ending in '/' is a folder.
void zip(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &entries)
{
    mz_zip_archive archive{};
    assert(mz_zip_writer_init_file(&archive, path.string().c_str(), 0));
    for (const auto &[name, body] : entries)
        assert(mz_zip_writer_add_mem(&archive, name.c_str(), body.data(), body.size(), MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&archive) && mz_zip_writer_end(&archive));
}

const std::vector<std::pair<std::string, std::string>> kPrefix{
    {"dosdevices/", ""},
    {"dosdevices/.pw-symlinks", "c:\t../drive_c\nz:\t/\n"},
    {"drive_c/", ""},
    {"drive_c/windows/", ""},
    {"drive_c/windows/system32/", ""},
    {"drive_c/windows/system32/d3d9.dll", std::string(70000, 'd')},
    {"drive_c/users/prospero/Saved Games/", ""},
    {"system.reg", "WINE REGISTRY Version 2\n"},
    {"user.reg", "WINE REGISTRY Version 2\n"},
};
} // namespace

int main()
{
    char temporary[] = "/tmp/prospero-prefix-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path root = fs::canonical(temporary), win = root / "win", bare = root / "bare.zip";
    std::atomic<bool> go{false};
    std::string error;
    zip(bare, kPrefix);

    // Made once, whole, with its link table as a file; progress reaches the total.
    std::uint64_t last = 0, total = 0;
    const auto progress = [&](std::uint64_t done, std::uint64_t all)
    {
        last = done;
        total = all;
    };
    assert(!prefix_ready(win.string(), "shared"));
    if (!ensure_prefix(win.string(), "shared", bare.string(), progress, go, error))
    {
        std::cerr << "ensure_prefix: " << error << "\n";
        return 1;
    }
    const auto prefix = win / "prefixes" / "shared";
    assert(prefix_ready(win.string(), "shared"));
    assert(read(prefix / "dosdevices" / ".pw-symlinks") == "c:\t../drive_c\nz:\t/\n");
    assert(read(prefix / "drive_c" / "windows" / "system32" / "d3d9.dll") == std::string(70000, 'd'));
    assert(fs::is_directory(prefix / "drive_c" / "users" / "prospero" / "Saved Games"));
    assert(last == total && total > 70000);
    assert(!fs::exists(win / "prefixes" / "shared.unpacking"));

    // An existing prefix is never touched, whatever the zip holds now.
    std::ofstream(prefix / "user.reg", std::ios::binary) << "the game's settings";
    zip(bare, {{"system.reg", "x"}, {"user.reg", "y"}, {"drive_c/windows/", ""}});
    assert(ensure_prefix(win.string(), "shared", bare.string(), progress, go, error));
    assert(read(prefix / "user.reg") == "the game's settings");

    // Refused, nothing left behind: a path out of the prefix, a zip that isn't a prefix,
    // a folder in the way that isn't a prefix, a name that isn't plain, no zip at all.
    zip(bare, {{"../escape.txt", "x"}, {"system.reg", "x"}});
    assert(!ensure_prefix(win.string(), "other", bare.string(), progress, go, error));
    assert(error.find("holds an entry it may not") != std::string::npos);
    assert(!fs::exists(win / "prefixes" / "other.unpacking") && !fs::exists(root / "escape.txt"));
    zip(bare, {{"drive_c/windows/a.dll", "a"}, {"user.reg", "u"}});
    assert(!ensure_prefix(win.string(), "other", bare.string(), progress, go, error));
    assert(error.find("isn't a Wine prefix") != std::string::npos);
    assert(!fs::exists(win / "prefixes" / "other") && !fs::exists(win / "prefixes" / "other.unpacking"));
    fs::create_directories(win / "prefixes" / "half" / "drive_c");
    zip(bare, kPrefix);
    assert(!ensure_prefix(win.string(), "half", bare.string(), progress, go, error));
    assert(error.find("isn't a Wine prefix; it was left as it is") != std::string::npos);
    assert(!ensure_prefix(win.string(), "../out", bare.string(), progress, go, error));
    assert(!ensure_prefix(win.string(), "new", (root / "missing.zip").string(), progress, go, error));
    assert(error.find("can't be read") != std::string::npos);

    // Cancelled while unpacking: nothing left.
    std::atomic<bool> stop{true};
    assert(!ensure_prefix(win.string(), "new", bare.string(), progress, stop, error) && error == "Cancelled");
    assert(!fs::exists(win / "prefixes" / "new") && !fs::exists(win / "prefixes" / "new.unpacking"));

    fs::remove_all(root);
    std::cout << "Prefix checks passed\n";
}
