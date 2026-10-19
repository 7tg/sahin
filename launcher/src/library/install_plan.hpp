// Prospero BlackBox - A game's install plan: what goes into the prefix, read from data.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "library/game_profile.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace store::library
{
// One file the install needs, known by its SHA-256 and size. The install takes it from
// mods-cache/<sha256>[.ext]; url or drive say where it can be downloaded (P3), and a
// source with neither is one the owner puts in mods-cache by hand.
struct Source
{
    std::string id;   // how placements name it
    std::string name; // for the page and the log
    std::string sha256;
    std::uint64_t size = 0;
    std::string url;   // https download, or empty
    std::string drive; // Google Drive file id, or empty
};

// A source's file, or what a zip source holds, put somewhere:
// member empty: the file itself, at to.
// member ending in '/': every file under it, at to + the rest of its path.
// otherwise: that one member, at to.
// to is relative to the game's folder (game files) or the prefix's C:\windows
// (system files), '/' between parts, matched without regard to case.
struct Placement
{
    std::string source; // a Source id
    std::string member;
    std::string to;
    bool keep_builtin = false; // system files: Wine's own copy is kept as <name>.builtin
};

// A file the install writes itself (dxvk.conf), or text added to the end of one a
// placement put there (append).
struct TextFile
{
    std::string to;
    std::string body;
    bool append = false;
};

// Bytes changed in a placed file: at offset, from must be there and becomes to (hex).
struct BytePatch
{
    std::string file;
    std::uint64_t offset = 0;
    std::string from, to; // raw bytes, the same length
};

// key = value lines changed in an ini the placements put there; every key must be in it.
struct IniEdit
{
    std::string file;
    std::vector<std::pair<std::string, std::string>> values;
    bool lf = false; // its line ends written as \n alone
};

// Attributes set on one element of an XML file the game keeps in the Windows user's folder
// (drive_c/users/<user> in the prefix, every one but Public): a setting the game saved there
// that the page's must win over (Blur's picture size in AppData/Roaming/Bizarre Creations/
// Blur/settings.xml, which it writes once the player saves its Graphics page and then reads
// before the desktop's size). Only a file that is already there is changed, and only the
// values of the attributes (one missing from the element is added after the others); every
// other byte stays. A file without the element is left alone. In a value, {width} and
// {height} are the profile's picture size.
//   "user_files": [{"file": "AppData/Roaming/...", "element": "shared_graphical_settings",
//                   "attributes": {"width": "{width}", "height": "{height}"}}]
struct UserFile
{
    std::string file; // relative to the user's folder
    std::string element;
    std::vector<std::pair<std::string, std::string>> attributes; // in the plan's order
};

struct RegistryValue
{
    std::string key;  // single backslashes, relative to the file's root
    std::string name; // empty for the default value
    bool dword = false;
    std::string text;         // a string value
    std::uint32_t number = 0; // a dword value
};

struct InstallPlan
{
    std::string game;    // base-games/<game>, C:\Games\<game>
    std::string title;   // for the page and the log
    std::string release; // the base copy's release the plan installs over
    std::string prefix;  // /data/prospero-win/prefixes/<prefix>
    std::vector<Source> sources;
    std::vector<Placement> game_files;
    std::vector<Placement> system_files;
    std::vector<BytePatch> patches;
    std::vector<IniEdit> ini;
    std::vector<TextFile> texts;
    // Files of the finished game folder and the SHA-256 they must have: checked before the
    // new folder takes the old one's place.
    std::vector<std::pair<std::string, std::string>> expect;
    std::vector<RegistryValue> machine; // system.reg
    std::vector<RegistryValue> user;    // user.reg
    std::vector<UserFile> user_files;   // the game's own settings, kept to the profile's
    GameProfile profile;
    std::string profile_after; // profiles.lst: listed after this line, when it's there
    // Pieces of the finished setup the plan can't install yet (no pinned source):
    // an install with any refuses to start unless it is told to go without them.
    std::vector<std::string> pending;
    // Paths in the game's folder that go from the copy before the plan's files are added
    // (case-insensitive; a folder goes with what is in it): what earlier mod installs left in
    // the user's copy. The copy itself is never changed.
    std::vector<std::string> strip;
    // Older releases of the base copy this plan installs from: each one's upgrade (a
    // Source) brings a copy of it up to release first. "kind": "zip" (the default) is our
    // own upgrade file (upgrade.hpp); "rtpatch" is the publisher's patch (rtpatch.hpp).
    struct Upgrade
    {
        std::string from, source;
        bool rtpatch = false;
    };
    std::vector<Upgrade> upgrades;
    // What the user may add at Install (Blur's "Amax client extras"): each adds its files,
    // edits and expected hashes to the plan's, and its dll_overrides to the profile's.
    // Its sources are in the plan's list; only the chosen options' are fetched.
    struct Option
    {
        std::string id, title, description;
        bool on = false; // the plan's default ("default")
        std::vector<Placement> game_files, system_files;
        std::vector<TextFile> texts;
        std::vector<IniEdit> ini;
        std::vector<std::pair<std::string, std::string>> expect;
        std::string dll_overrides; // appended to the profile's
    };
    std::vector<Option> options;
    // Ini edits for the DualSense touchpad as the mouse ("touchpad": {"ini": [...]}; the game
    // draws its own cursor), applied after the plan's own when it is chosen.
    std::vector<IniEdit> touchpad_ini;

    const Source *source(const std::string &id) const;
    const Option *option(const std::string &id) const;
};

// The plan in text, checked whole: every placement names a known source, paths stay
// inside their folder, hashes are 64 hex digits. False with error set otherwise.
bool parse_plan(std::string_view text, InstallPlan &plan, std::string &error);
bool load_plan(const std::string &path, InstallPlan &plan, std::string &error);

// A relative path the install may write: '/' between non-empty parts, none of them
// "." or "..", no backslashes, nothing that isn't printable ASCII.
bool safe_relative(std::string_view path);
} // namespace store::library
