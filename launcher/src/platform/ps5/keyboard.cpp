// Prospero BlackBox - USB keyboards, read through the console's keyboard library.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/keyboard.hpp"
#include "app/keyboard_map.hpp"
#include <cstddef>

namespace
{
// The library's state record, as ProsperoLight verified it on a console.
struct KeyboardState
{
    std::uint64_t timestamp_us;
    std::uint8_t intercepted;
    std::uint8_t reserved0[7];
    std::uint8_t connected;
    std::uint8_t reserved1[3];
    std::int32_t length;
    std::uint32_t leds;
    std::uint32_t modifiers;
    std::uint16_t keys[16];
    std::uint8_t reserved2[32];
};
static_assert(sizeof(KeyboardState) == 96);
static_assert(offsetof(KeyboardState, connected) == 0x10);
static_assert(offsetof(KeyboardState, modifiers) == 0x1c);
static_assert(offsetof(KeyboardState, keys) == 0x20);
} // namespace

extern "C"
{
    int sceSysmoduleLoadModule(std::uint16_t module_id);
    int sceKeyboardInit(void);
    int sceKeyboardOpen(std::int32_t user, std::int32_t type, std::int32_t index,
                        const void *parameters);
    int sceKeyboardRead(std::int32_t handle, void *data, std::int32_t capacity);
    int sceKeyboardClose(std::int32_t handle);
}

namespace store::ps5
{
int Keyboards::prepare()
{
    const int module = sceSysmoduleLoadModule(0x0106);
    if (module < 0)
        return module;
    const int init = sceKeyboardInit();
    ready_ = init >= 0;
    return init;
}

void Keyboards::open(std::int32_t user, std::int64_t now_us)
{
    if (!ready_ || user < 0 || now_us < next_scan_us_)
        return;
    user_ = user;
    next_scan_us_ = now_us + 3000000;
    const std::uint64_t parameters = 0;
    for (int slot = 0; slot < kSlots; ++slot)
        if (handles_[slot] < 0)
        {
            const int handle = sceKeyboardOpen(user_, 0, slot, &parameters);
            if (handle >= 0)
            {
                handles_[slot] = handle;
                ++opened_;
            }
        }
}

std::uint32_t Keyboards::buttons(bool &connected)
{
    connected = false;
    std::uint32_t all = 0;
    KeyboardState samples[16];
    for (int slot = 0; slot < kSlots; ++slot)
    {
        if (handles_[slot] < 0)
            continue;
        const int count = sceKeyboardRead(handles_[slot], samples, 16);
        // The newest sample says what is held now; earlier ones are already past.
        if (count > 0)
        {
            const KeyboardState &latest = samples[count > 16 ? 15 : count - 1];
            connected_[slot] = latest.connected != 0;
            held_[slot] = latest.connected && !latest.intercepted
                              ? keyboard_buttons(latest.keys, latest.modifiers)
                              : 0;
        }
        connected = connected || connected_[slot];
        all |= held_[slot];
    }
    return all;
}

void Keyboards::close()
{
    for (auto &handle : handles_)
        if (handle >= 0)
        {
            (void)sceKeyboardClose(handle);
            handle = -1;
        }
}
} // namespace store::ps5
