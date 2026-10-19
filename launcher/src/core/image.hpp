// ps5-homebrew-ui - Bounded PNG decode for worker-loaded UI artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
namespace hui
{
struct Image
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
// At most 2 MiB encoded, 1024 pixels per side, and 32 MiB decoder memory.
// Decode on a worker. The output is unchanged on failure.
bool decode_png(std::string_view encoded, Image &out);
} // namespace hui
