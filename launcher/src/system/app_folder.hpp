// Prospero BlackBox - Where the running store's own files are.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace store::system
{
// The store's own folder (eboot.bin, assets, the worker and helper programs), as
// ProsperoEden finds its own (97526dc): /app0 while it is still mounted; else the
// console's mount of the running app, /system_ex/app/<TITLEID>, which follows
// ShadowMountPlus wherever the store is installed (internal, extended or USB
// drive, or its manual list); else the sandbox's mount of it. The first that
// holds eboot.bin wins; /app0 when none does. Found once, then kept.
const std::string &app_folder();
// The running app's title ID: the game's in a game's own app (BLACKBOX_GAME), else the store's.
const char *own_title();
} // namespace store::system
