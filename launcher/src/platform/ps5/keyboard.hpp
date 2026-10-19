// Prospero BlackBox - USB keyboards, read through the console's keyboard library.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace store::ps5
{
class Keyboards
{
  public:
    // Loads the library (system module 0x0106) and readies it. Call before the
    // store leaves its sandbox: afterwards the console refuses to load modules.
    // Returns the result code, for the log.
    int prepare();
    // Opens the keyboards of user (and, every few seconds, ones plugged in later).
    void open(std::int32_t user, std::int64_t now_us);
    // The controller buttons the held keys stand for (store::keyboard_buttons);
    // connected is true while any keyboard is plugged in.
    std::uint32_t buttons(bool &connected);
    void close();
    int opened() const
    {
        return opened_;
    }

  private:
    static constexpr int kSlots = 4;
    bool ready_ = false;
    std::int32_t user_ = -1;
    std::int32_t handles_[kSlots] = {-1, -1, -1, -1};
    std::uint32_t held_[kSlots] = {};
    bool connected_[kSlots] = {};
    std::int64_t next_scan_us_ = 0;
    int opened_ = 0;
};
} // namespace store::ps5
