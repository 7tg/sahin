// ps5-native-app-boilerplate - Explicit system trust for elevated HTTPS callers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
namespace https_trust
{
// Caller reads the console's immutable CA bundle. Does not change TLS options.
// Returns the platform result; -1 means malformed or oversized PEM input.
int load_pem_roots(int http_context, std::string_view pem);
// Capture the platform roots before changing the process's filesystem root.
// Leaves out unchanged on error; successful output can be imported after elevation.
int read_builtin_roots(int ssl_context, std::string &out);
// Read and import in the current filesystem view.
int load_builtin_roots(int ssl_context, int http_context);
} // namespace https_trust
