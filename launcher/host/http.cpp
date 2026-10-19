// Prospero BlackBox - Host bindings for the checked curl transport.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/curl_request.hpp"

namespace store::net
{
void Control::cancel()
{
    cancelled.store(true);
}
Response request_once(const std::string &url, std::uint64_t limit, const Sink &sink,
                      Control &control, const std::string &etag)
{
    return curl_request(url, limit, sink, control, etag);
}
} // namespace store::net
