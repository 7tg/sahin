// Prospero BlackBox - Checked curl transport, shared by host and native fallback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "net/http.hpp"
#include <curl/curl.h>

namespace store::net
{
Response curl_request(const std::string &url, std::uint64_t limit, const Sink &sink,
                      Control &control, const std::string &etag, const char *ca_path = nullptr);
}
