// Prospero BlackBox - An app's ambient picture, made from its own icon.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace store
{
// Every app gets a picture of its own without shipping one: the icon's two
// main colours, painted as soft light over Farlight's dark in a 16:9 field
// that stays dark enough for white words. 96 x 54 pixels (20 KB as a
// texture), made once when the icon arrives. `vivid` receives the main
// colour as 0xRRGGBB, or 0 for a grey or black icon (the field is then
// Farlight's own violet).
inline hui::Image make_ambient(const hui::Image &icon, std::uint32_t &vivid)
{
    struct Rgb
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };
    const auto mix = [](Rgb a, Rgb b, float k)
    { return Rgb{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; };
    const auto light = [](Rgb c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
    const Rgb deep{0x0e / 255.0f, 0x0f / 255.0f, 0x24 / 255.0f};
    const Rgb violet{0x42 / 255.0f, 0x35 / 255.0f, 0x8f / 255.0f};
    const Rgb blue{0x24 / 255.0f, 0x4a / 255.0f, 0xb8 / 255.0f};
    hui::Image out;
    vivid = 0;
    if (icon.width <= 0 || icon.height <= 0 ||
        icon.rgba.size() < static_cast<std::size_t>(icon.width) * icon.height * 4)
        return out;

    // The colourful pixels by hue, 12 buckets of 30 degrees, weighted by
    // saturation and brightness: greys, blacks and whites do not vote.
    struct Bucket
    {
        Rgb sum;
        float weight = 0.0f;
    } buckets[12];
    for (int y = 0; y < icon.height; y += 4)
        for (int x = 0; x < icon.width; x += 4)
        {
            const std::uint8_t *p = &icon.rgba[(static_cast<std::size_t>(y) * icon.width + x) * 4];
            if (p[3] < 128)
                continue;
            const Rgb c{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
            const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
            const float chroma = hi - lo;
            if (hi < 0.2f || chroma < 0.18f)
                continue;
            float hue = hi == c.r   ? std::fmod((c.g - c.b) / chroma + 6.0f, 6.0f)
                        : hi == c.g ? (c.b - c.r) / chroma + 2.0f
                                    : (c.r - c.g) / chroma + 4.0f;
            Bucket &bucket = buckets[static_cast<int>(hue * 2.0f) % 12];
            const float w = chroma * hi;
            bucket.sum = {bucket.sum.r + c.r * w, bucket.sum.g + c.g * w, bucket.sum.b + c.b * w};
            bucket.weight += w;
        }
    // The main colour is the strongest hue; the second, the strongest at
    // least 60 degrees away with a real share of the icon.
    int first = -1, second = -1;
    for (int i = 0; i < 12; ++i)
        if (buckets[i].weight > 0.0f && (first < 0 || buckets[i].weight > buckets[first].weight))
            first = i;
    if (first >= 0)
        for (int i = 0; i < 12; ++i)
        {
            const int apart = std::min((i - first + 12) % 12, (first - i + 12) % 12);
            if (apart >= 2 && buckets[i].weight >= 0.12f * buckets[first].weight &&
                (second < 0 || buckets[i].weight > buckets[second].weight))
                second = i;
        }
    // A bucket's colour at full brightness: the field decides how much light it gets.
    const auto colour_of = [&](int i)
    {
        const Bucket &b = buckets[i];
        Rgb c{b.sum.r / b.weight, b.sum.g / b.weight, b.sum.b / b.weight};
        const float hi = std::max({c.r, c.g, c.b});
        return Rgb{c.r / hi, c.g / hi, c.b / hi};
    };
    Rgb main = violet, other = blue;
    if (first >= 0)
    {
        main = colour_of(first);
        other = second >= 0 ? colour_of(second) : mix(main, violet, 0.55f);
        const Rgb raw = colour_of(first);
        vivid = (static_cast<std::uint32_t>(raw.r * 255.0f + 0.5f) << 16) |
                (static_cast<std::uint32_t>(raw.g * 255.0f + 0.5f) << 8) |
                static_cast<std::uint32_t>(raw.b * 255.0f + 0.5f);
    }
    else
    {
        main = mix(violet, Rgb{1.0f, 1.0f, 1.0f}, 0.25f);
        other = blue;
    }

    // Paint: Farlight's dark, the main colour high on the right (where the
    // stage puts the icon), the second low on the left, both meeting low right.
    constexpr int kW = 96, kH = 54;
    constexpr float kAspect = static_cast<float>(kW) / kH;
    struct Blob
    {
        Rgb colour;
        float x, y, radius, strength;
    };
    const Blob blobs[] = {
        {main, 0.80f, 0.28f, 0.78f, 0.82f},
        {other, 0.14f, 0.96f, 0.85f, 0.62f},
        {mix(main, other, 0.5f), 1.04f, 1.06f, 0.62f, 0.5f},
    };
    out.width = kW;
    out.height = kH;
    out.rgba.resize(static_cast<std::size_t>(kW) * kH * 4);
    const Rgb base = mix(deep, main, 0.14f);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
        {
            const float u = (x + 0.5f) / kW, v = (y + 0.5f) / kH;
            Rgb c = base;
            for (const Blob &blob : blobs)
            {
                const float dx = (u - blob.x) * kAspect, dy = v - blob.y;
                const float d = std::sqrt(dx * dx + dy * dy) / blob.radius;
                if (d >= 1.0f)
                    continue;
                const float fall = (1.0f - d * d) * (1.0f - d * d);
                c = mix(c, blob.colour, blob.strength * fall);
            }
            // Never brighter than the words allow.
            const float l = light(c);
            if (l > 0.36f)
                c = Rgb{c.r * 0.36f / l, c.g * 0.36f / l, c.b * 0.36f / l};
            std::uint8_t *px = &out.rgba[(static_cast<std::size_t>(y) * kW + x) * 4];
            px[0] = static_cast<std::uint8_t>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[1] = static_cast<std::uint8_t>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[2] = static_cast<std::uint8_t>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[3] = 255;
        }
    return out;
}

// A grey or black icon's colour: the average of its opaque pixels, 0xRRGGBB.
inline std::uint32_t average_colour(const hui::Image &icon)
{
    double sum[3] = {0, 0, 0}, weight = 0;
    if (icon.rgba.size() < static_cast<std::size_t>(icon.width) * icon.height * 4)
        return 0x42358f;
    for (std::size_t i = 0; i + 3 < icon.rgba.size(); i += 4 * 7)
    {
        const double a = icon.rgba[i + 3] / 255.0;
        for (int c = 0; c < 3; ++c)
            sum[c] += icon.rgba[i + c] * a;
        weight += a;
    }
    if (weight <= 0)
        return 0x42358f;
    return (static_cast<std::uint32_t>(sum[0] / weight) << 16) |
           (static_cast<std::uint32_t>(sum[1] / weight) << 8) |
           static_cast<std::uint32_t>(sum[2] / weight);
}
} // namespace store
