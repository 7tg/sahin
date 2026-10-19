// ps5-homebrew-ui - Bounded QR artwork for links and pairing codes.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/image.hpp"
namespace hui
{
// Encode at four pixels per module, including a four-module white quiet zone.
// At most 512 UTF-8 bytes; call on a worker. Output is unchanged on failure.
bool encode_qr(std::string_view text, Image &out);
} // namespace hui
