// ps5-homebrew-ui - PNG decoder with bounded input, dimensions and allocations.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/image.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
std::mutex decoder_mutex;
std::size_t allocated = 0;
constexpr std::size_t budget = 32u << 20;
struct alignas(std::max_align_t) Allocation
{
    std::size_t size;
};
void *image_allocate(std::size_t size)
{
    if (size > budget - allocated)
        return nullptr;
    auto *block = static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
    if (!block)
        return nullptr;
    block->size = size;
    allocated += size;
    return block + 1;
}
void image_free(void *pointer)
{
    if (!pointer)
        return;
    auto *block = static_cast<Allocation *>(pointer) - 1;
    allocated -= block->size;
    std::free(block);
}
void *image_reallocate(void *pointer, std::size_t size)
{
    if (!pointer)
        return image_allocate(size);
    auto *block = static_cast<Allocation *>(pointer) - 1;
    const auto old_size = block->size;
    if (size > budget - allocated + old_size)
        return nullptr;
    auto *next = static_cast<Allocation *>(std::realloc(block, sizeof(Allocation) + size));
    if (!next)
        return nullptr;
    next->size = size;
    allocated = allocated - old_size + size;
    return next + 1;
}
} // namespace

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 1024
#define STBI_MALLOC(size) image_allocate(size)
#define STBI_REALLOC(pointer, size) image_reallocate(pointer, size)
#define STBI_FREE(pointer) image_free(pointer)
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

namespace hui
{
bool decode_png(std::string_view encoded, Image &out)
{
    constexpr unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (encoded.size() < sizeof(png) || encoded.size() > (2u << 20) ||
        std::memcmp(encoded.data(), png, sizeof(png)) != 0)
        return false;
    // ponytail: one decoder at a time; use per-decoder allocators if parallel decode is needed.
    std::lock_guard lock(decoder_mutex);
    int width = 0, height = 0, channels = 0;
    const auto *bytes = reinterpret_cast<const stbi_uc *>(encoded.data());
    const auto size = static_cast<int>(encoded.size());
    if (!stbi_info_from_memory(bytes, size, &width, &height, &channels) || width <= 0 ||
        height <= 0 || width > 1024 || height > 1024)
        return false;
    auto *pixels = stbi_load_from_memory(bytes, size, &width, &height, &channels, 4);
    if (!pixels)
        return false;
    Image next;
    next.width = width;
    next.height = height;
    next.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    out = std::move(next);
    return true;
}
} // namespace hui
