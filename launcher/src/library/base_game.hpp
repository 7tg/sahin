// Prospero BlackBox - The user's clean copy of a game: found, checked, imported.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "library/tree.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace store::library
{
// One known release of a game, told apart by a data file the patches change (never by
// the executable: it is cracked in too many ways, and the install replaces it anyway).
struct Release
{
    const char *version;   // "1.3"
    std::uint64_t size;    // of Manifest::version_file
    std::uint32_t crc;     // CRC-32 of it
    bool needs_patch;      // the install patches it up to the current release first
};

// What a clean copy of a game holds. Paths are relative and matched without regard to
// case (the PC's file system ignores it; FTP and USB copies keep whatever it was).
struct Manifest
{
    const char *folder;                       // base-games/<folder>, USB:/<folder>
    std::span<const char *const> folders;     // must be there
    std::span<const char *const> files;       // must be there
    const char *version_file;                 // the file Release tells versions by
    std::span<const Release> releases;
    std::span<const char *const> mod_files;   // signs of mods: not a clean copy
};

// The manifest for a base-games folder name (NFSMW...), or nullptr.
const Manifest *manifest_for(const std::string &folder);

enum class State
{
    absent,      // nowhere to be found
    ready,       // the current release
    needs_patch, // an older release the install patches
    unknown,     // the version file matches no release we know (modified, or a release we haven't seen)
    incomplete,  // folders or files missing
    modified     // mods are installed in it
};

struct BaseGame
{
    State state = State::absent;
    std::string path;    // the folder that was inspected
    bool on_usb = false; // found on a USB drive, not in base-games yet
    std::string version; // the release, when known
    std::string detail;  // for the page: what is missing or wrong
};

// The game's folder resolved without regard to case ("" when there is none).
std::string find_entry(const std::string &folder, const std::string &relative);

// Checks one folder against the manifest.
BaseGame inspect(const std::string &folder, const Manifest &manifest);

// base-games/<folder> under data_root first; else the first USB root that has one.
BaseGame locate(const std::string &data_root, std::span<const std::string> usb_roots,
                const Manifest &manifest);

// Where USB and extended drives are mounted on the console.
std::vector<std::string> usb_roots();

using ImportProgress = CopyProgress;

// Copies a whole folder to base-games/<folder>: into <folder>.importing first, then
// renamed into place, so base-games never holds half a copy. Links and special files
// are refused. False with error set on failure or cancel (the partial copy is removed).
bool import_base(const std::string &from, const std::string &data_root, const Manifest &manifest,
                 const std::function<void(const ImportProgress &)> &progress,
                 const std::atomic<bool> &cancel, std::string &error);
} // namespace store::library
