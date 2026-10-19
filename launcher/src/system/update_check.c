// Prospero BlackBox - The boilerplate's update check, over the store's own HTTPS.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
// The store is elevated, where the console's own HTTPS can't reach public
// sites, so the kit's decision code is used with the store's transport.
#define UPDATE_CHECK_NO_NETWORK 1
#include "../../examples/update-check/update_check.c"
