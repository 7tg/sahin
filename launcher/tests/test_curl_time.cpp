// ps5-native-app-boilerplate - Compare certificate date conversion with host libc.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <limits>
extern "C" tm *curl_test_gmtime_r(const time_t *, tm *);
int main()
{
    const time_t cases[] = {
        -62167219200LL, -2208988801LL, -86401,        -1, 0, 1, 86400, 951782400, 1709164800,
        2147483648LL,   4107542400LL,  253402300799LL};
    for (const auto value : cases)
    {
        tm expected{}, actual{};
        assert(gmtime_r(&value, &expected) && curl_test_gmtime_r(&value, &actual));
        assert(expected.tm_year == actual.tm_year && expected.tm_mon == actual.tm_mon &&
               expected.tm_mday == actual.tm_mday && expected.tm_yday == actual.tm_yday &&
               expected.tm_wday == actual.tm_wday && expected.tm_hour == actual.tm_hour &&
               expected.tm_min == actual.tm_min && expected.tm_sec == actual.tm_sec);
    }
    const time_t maximum = std::numeric_limits<time_t>::max();
    tm out{};
    assert(!curl_test_gmtime_r(&maximum, &out) && errno == EOVERFLOW);
}
