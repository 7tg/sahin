// Prospero BlackBox - Bounded catalog icon cache; caller runs on a worker.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalog/catalog.hpp"
#include "core/image.hpp"

namespace store::catalog
{
class Icons
{
  public:
    explicit Icons(std::string root) : root_(std::move(root))
    {
    }
    bool cached(const Entry &entry, hui::Image &out) const;
    bool store(const Entry &entry, std::string_view encoded, hui::Image &out) const;
    static std::string key(const Entry &entry);

  private:
    std::string root_;
};
} // namespace store::catalog
