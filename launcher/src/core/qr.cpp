// ps5-homebrew-ui - QR encoding through Project Nayuki's C implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/qr.hpp"
#include "third_party/qrcodegen/qrcodegen.h"
#include <array>
#include <string>
#include <utility>
namespace hui
{
bool encode_qr(std::string_view text, Image &out)
{
    if (text.empty() || text.size() > 512 || text.find('\0') != std::string_view::npos)
        return false;
    constexpr int version = 20, quiet = 4, scale = 4;
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(version)> temporary{}, code{};
    if (!qrcodegen_encodeText(std::string(text).c_str(), temporary.data(), code.data(),
                              qrcodegen_Ecc_MEDIUM, 1, version, qrcodegen_Mask_AUTO, true))
        return false;
    const int modules = qrcodegen_getSize(code.data());
    Image image;
    image.width = image.height = (modules + 2 * quiet) * scale;
    image.rgba.assign(static_cast<std::size_t>(image.width * image.height) * 4, 255);
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (qrcodegen_getModule(code.data(), x, y))
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx)
                    {
                        const auto pixel =
                            static_cast<std::size_t>(((y + quiet) * scale + dy) * image.width +
                                                     (x + quiet) * scale + dx) *
                            4;
                        image.rgba[pixel] = image.rgba[pixel + 1] = image.rgba[pixel + 2] = 0;
                    }
    out = std::move(image);
    return true;
}
} // namespace hui
