// Prospero BlackBox - The built-in catalog: the Black Box Need for Speed games.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalog/catalog.hpp"
#include <cstdint>
#include <span>
#include <string_view>
#include <string>

namespace store::catalog
{
// A game's colours, from its own menus (0xRRGGBB): the interface takes them on.
struct Palette
{
    std::uint32_t deep;      // the dark everything sits on
    std::uint32_t mid;       // its mid tone: plates, raised surfaces
    std::uint32_t accent;    // calls to action, the focus, highlights
    std::uint32_t on_accent; // text on the accent
};

struct Game
{
    const char *id;          // its own Home-screen title, PPSA99404
    const char *folder;      // base-games/<folder> and the USB folder name
    const char *name;
    const char *released;    // ISO date, for sorting
    const char *art;         // assets/games/<art>.png in the app folder
    const char *description;
    bool available;          // false: listed, greyed out as in progress
    Palette palette;
};

std::span<const Game> blackbox_games();

// The built-in game (PPSA99404), whether or not its base copy
// has been looked up yet.
inline bool blackbox_game(std::string_view id)
{
    for (const auto &game : blackbox_games())
        if (id == game.id)
            return true;
    return false;
}

// The manager's own colours, before a game is in focus.
inline constexpr Palette kManagerPalette{0x0e0f24, 0x42358f, 0xffd166, 0x241a05};
// A game's palette by title ID; the manager's for any other.
const Palette &blackbox_palette(std::string_view id);

// The catalog the store shows: every game, its icon a path in the app folder.
Snapshot blackbox_snapshot(const std::string &app_folder);

// An icon that names a file in the app folder rather than an address.
inline bool local_icon(const std::string &icon)
{
    return !icon.empty() && icon.front() == '/';
}
} // namespace store::catalog
