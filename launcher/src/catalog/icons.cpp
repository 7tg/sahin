// Prospero BlackBox - Icons are untrusted images, never executable paths.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog/icons.hpp"
#include "core/save_file.hpp"

namespace store::catalog
{
std::string Icons::key(const Entry &entry)
{
    return title_id(entry.id) && api_url(entry.icon)
               ? sha256(entry.id + "\n" + entry.icon + "\n" + entry.icon_hash)
               : std::string{};
}
bool Icons::cached(const Entry &entry, hui::Image &out) const
{
    const auto name = key(entry);
    std::string encoded;
    return !root_.empty() && !name.empty() &&
           hui::save::read_file(root_ + "/" + name, &encoded, 2u << 20) &&
           hui::decode_png(encoded, out);
}
bool Icons::store(const Entry &entry, std::string_view encoded, hui::Image &out) const
{
    const auto name = key(entry);
    if (name.empty() || !hui::decode_png(encoded, out))
        return false;
    // An unavailable cache must not hide a successfully decoded icon.
    if (!root_.empty() && hui::save::ensure_directory(root_))
        (void)hui::save::write_atomic(root_ + "/" + name, encoded);
    return true;
}
} // namespace store::catalog
