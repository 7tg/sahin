// ps5-native-app-boilerplate - Link-only CommonDialog import facade.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

// The public SDK omits this module. The native builder turns this symbol into
// a system import; this body must never be linked into or shipped with the app.
extern "C" int sceCommonDialogInitialize()
{
    return 0;
}
