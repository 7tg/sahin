// Prospero BlackBox - A game's base copy brought up to its current release by our own diff.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <string>

namespace store::library
{
// An upgrade file (made on a PC by tools/make-upgrade.py from a clean copy of each release)
// is a zip holding upgrade.json and one ops file per changed file:
//
//   upgrade.json  {"schema": 1, "game": "BLUR", "from": "1.0", "to": "1.1",
//                  "files": [{"path": "GLOBAL/GAMEPLAY.BIN", "from_sha256": "..." (absent
//                             for a new file), "sha256": "...", "size": 2105216,
//                             "ops": "ops/0"}],
//                  "remove": ["path", ...]}
//   ops/<n>       records, until the end: 'C' u64 offset, u64 length (bytes of the old
//                 file) or 'A' u64 length, then that many bytes; little-endian
//
// It carries only what changed, so it is no use without the old release. A publisher's
// own patch can be used instead where its format is known (rtpatch.hpp); this is the
// fallback when it isn't.

// Applies the upgrade to the game's folder (a copy, never the base copy itself): every
// old file is checked before it changes and every new one after; a changed file replaces
// the old one only once it is whole. False with error set, the folder then half-changed
// (the caller throws the copy away).
bool apply_upgrade(const std::string &upgrade, const std::string &folder, const std::string &game,
                   const std::string &from, const std::string &to,
                   const std::atomic<bool> &cancel, std::string &error);
} // namespace store::library
