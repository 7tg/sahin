// Prospero BlackBox - The console's list of installed titles (its home screen).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace store::system
{
// Readies the console's app-install service. Called once at start-up; the
// result code is only logged.
int prepare_title_registry();

// Takes an uninstalled app off the home screen: the console's own title
// entry and its copies under /user/app and /user/appmeta. Nothing under /data
// (where apps keep their own files) is touched. 0 when the console accepted it;
// otherwise its error code, and the tile stays until ShadowMountPlus or a
// restart catches up. Call only once the app's folder is gone.
int unregister_title(const std::string &title_id);
} // namespace store::system
