// Prospero BlackBox - The built-in catalog: the Black Box Need for Speed games.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog/blackbox.hpp"

namespace store::catalog
{
namespace
{
constexpr Game kGames[] = {
    {"PPSA99401", "NFSU", "Underground", "2003-11-17", "nfs-underground",
     "Street racing in Olympic City: tuners, drag and drift, and the cars of 2003.", false,
     {0x060b1f, 0x1b3f8f, 0x3fd0ff, 0x02121c}}, // night-blue, cyan neon
    {"PPSA99402", "NFSU2", "Underground 2", "2004-11-09", "nfs-underground-2",
     "Free roam through Bayview with deeper tuning and the Underground crew.", false,
     {0x0a0d0a, 0x4b2a7a, 0x9be22d, 0x0e1a02}}, // black, purple, acid green
    {"PPSA99403", "NFSMW", "Most Wanted", "2005-11-15", "nfs-most-wanted-2005",
     "Climb the Blacklist in Rockport with the cops on your tail. Installed as 1.3 with "
     "widescreen, DualSense and keyboard fixes, at 4K.",
     true, {0x101010, 0x3a3a34, 0xf6a800, 0x1c1400}}, // charcoal, Most Wanted amber
    {"PPSA99404", "NFSC", "Carbon", "2006-10-31", "nfs-carbon",
     "Canyon duels and crews in Palmont City.", false,
     {0x0c0a08, 0x5a3412, 0xff8a1f, 0x1f0e00}}, // black, Carbon orange
    {"PPSA99405", "NFSPS", "ProStreet", "2007-11-14", "nfs-prostreet",
     "Organised race days on closed circuits, with damage and tuning.", false,
     {0x121214, 0x5c1010, 0xe8202a, 0xffffff}}, // black and white, race red
    {"PPSA99406", "NFSUC", "Undercover", "2008-11-18", "nfs-undercover",
     "Undercover in the Tri-City Bay Area, back to the open world.", false,
     {0x081018, 0x1f4a66, 0x4fb8e6, 0x02131c}}, // steel, Undercover blue
};
} // namespace

std::span<const Game> blackbox_games()
{
    return kGames;
}

const Palette &blackbox_palette(std::string_view id)
{
    for (const auto &game : kGames)
        if (id == game.id)
            return game.palette;
    return kManagerPalette;
}

Snapshot blackbox_snapshot(const std::string &app_folder)
{
    Snapshot snapshot;
    for (const auto &game : kGames)
    {
        Entry entry;
        entry.id = game.id;
        entry.name = game.name;
        entry.author = "EA Black Box";
        entry.kind = "game";
        entry.status = game.available ? "available" : "coming_soon";
        entry.released = game.released;
        entry.description = game.description;
        entry.icon = app_folder + "/assets/games/" + game.art + ".png";
        snapshot.entries.push_back(std::move(entry));
    }
    // Built into the app: nothing to verify or download.
    snapshot.verified = true;
    snapshot.accepted = true;
    snapshot.online = false;
    return snapshot;
}
} // namespace store::catalog
