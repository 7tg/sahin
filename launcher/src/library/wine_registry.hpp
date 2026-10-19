// Prospero BlackBox - A Wine prefix's registry files (system.reg, user.reg), edited as text.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace store::library
{
// One of Wine's registry files. The console has no regedit (it can't start processes),
// so values are written into the file while nothing runs from the prefix. Only the
// values set here change: every other line, comments and timestamps included, is
// written back as it was read.
//
// Keys are written as Windows writes them, one backslash between parts, relative to
// the file's root (system.reg: HKEY_LOCAL_MACHINE; user.reg: HKEY_CURRENT_USER), e.g.
// "Software\Wow6432Node\EA Games\Need for Speed Most Wanted". Names and keys match
// without regard to ASCII case, as Windows matches them. The empty name is the key's
// default value (@).
class WineRegistry
{
  public:
    // False with error set (and nothing changed) when text isn't a Wine registry file.
    bool parse(std::string_view text, std::string &error);
    std::string text() const;

    // now stamps a key the file doesn't have yet. Keys, names and strings must be
    // printable ASCII: false (and nothing changed) otherwise.
    bool set_string(const std::string &key, const std::string &name, const std::string &value,
                    std::time_t now);
    bool set_dword(const std::string &key, const std::string &name, std::uint32_t value,
                   std::time_t now);
    std::optional<std::string> string_value(const std::string &key, const std::string &name) const;
    std::optional<std::uint32_t> dword(const std::string &key, const std::string &name) const;
    bool has_key(const std::string &key) const;

  private:
    // One line, or one value with its continuation lines, newlines included. Comment,
    // blank and unknown lines are kept as they are and never match a name.
    struct Item
    {
        bool value = false;
        std::string name; // unescaped; empty for @
        std::string lines;
    };
    struct Key
    {
        std::string name; // unescaped, single backslashes
        std::string line; // the [..] line as read
        std::vector<Item> items;
    };
    bool read(std::string_view text, std::string &error);
    Key *find(const std::string &key);
    const Key *find(const std::string &key) const;
    const Item *find_value(const std::string &key, const std::string &name) const;
    bool set_raw(const std::string &key, const std::string &name, const std::string &data,
                 std::time_t now);
    std::string preamble_;
    std::vector<Key> keys_;
};

// The file read whole; false with error set when it can't be read or parsed.
bool load_registry(const std::string &path, WineRegistry &registry, std::string &error);
// Written beside path, synced, then renamed over it.
bool save_registry(const std::string &path, const WineRegistry &registry, std::string &error);

// Most Wanted's (and the Widescreen Fix's) packed resolution: 0x80000000 | w << 16 | h.
constexpr std::uint32_t packed_resolution(std::uint32_t width, std::uint32_t height)
{
    return 0x80000000u | (width << 16) | height;
}
} // namespace store::library
