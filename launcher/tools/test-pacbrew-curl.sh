#!/usr/bin/env bash
# ps5-native-app-boilerplate - Host checks for PacBrew curl compatibility.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
flags=(-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror)
clang -std=c11 "${flags[@]}" -DCURL_COMPAT_TIME_TEST -Dgmtime_r=curl_test_gmtime_r \
    -c examples/pacbrew-curl/compat.c -o build/tests/curl-time.o
clang++ -std=c++20 "${flags[@]}" tests/test_curl_time.cpp build/tests/curl-time.o -o build/tests/curl-time
build/tests/curl-time
names=()
for name in getaddrinfo freeaddrinfo gai_strerror gethostbyname getnameinfo fnmatch; do
    names+=("-D$name=curl_test_$name")
done
clang -std=c11 "${flags[@]}" -D_DEFAULT_SOURCE "${names[@]}" -c examples/pacbrew-curl/netdb.c -o build/tests/curl-dns.o
clang -std=c11 "${flags[@]}" tests/test_curl_dns.c build/tests/curl-dns.o -o build/tests/curl-dns
build/tests/curl-dns
