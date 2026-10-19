// Prospero BlackBox - The store replacing itself, through the boilerplate's self-update kit.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalog/catalog.hpp"
#include "install/transaction.hpp"
#include "net/http.hpp"
#include <string>

namespace store::system
{
// Updates the running store to entry (its own verified catalog record): the
// archive is downloaded here and streamed to the kit's helper (self-updater.elf,
// sent to the payload loader), which checks and unpacks it beside the store.
// When the helper is ready the go-ahead is given and true is returned: the store
// must then close, and the helper swaps the files of the store's folder in place
// (the folder ShadowMountPlus mounted stays) and posts a system notification.
// False with error when nothing changed (the archive stays where it was).
bool update_self(const catalog::Entry &entry, const std::string &installed,
                 install::Progress &progress, net::Control &control, std::string &error);
} // namespace store::system
