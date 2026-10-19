// ProsperoRadio curl guide - libc entry points PacBrew's libcurl/OpenSSL/zstd need.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PacBrew's archives are built for the payload SDK's libc. A native title
// links against the console's libc import stubs, which do not export these.
// Each one is the smallest stand-in that keeps the library working. The set
// was found with tools in find_missing_symbols.sh; yours may differ with
// other package versions.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#ifndef CURL_COMPAT_TIME_TEST

/* libcurl looks up the user's home folder (.netrc and the like). There is no
 * user database: report "no such user". */
struct passwd;
int getpwuid_r(unsigned user, struct passwd *entry, char *buffer, size_t size,
               struct passwd **result)
{
    (void)user;
    (void)entry;
    (void)buffer;
    (void)size;
    *result = NULL;
    return ENOENT;
}

/* The archives call _setjmp/_longjmp; the console libc exports setjmp/longjmp.
 * Plain jumps keep the caller's frame intact (a C wrapper would not:
 * setjmp must not return through an extra frame). x86-64 only. */
__asm__(".text\n"
        ".globl _setjmp\n"
        "_setjmp:\n"
        "    jmp *setjmp@GOTPCREL(%rip)\n"
        ".globl _longjmp\n"
        "_longjmp:\n"
        "    jmp *longjmp@GOTPCREL(%rip)\n");

/* syslog is not used here. */
__attribute__((weak)) void openlog(const char *identifier, int option, int facility)
{
    (void)identifier;
    (void)option;
    (void)facility;
}

void closelog(void)
{
}

/* Referenced by OpenSSL's module-loading code; "not found" is fine. */
int dladdr(const void *address, void *info)
{
    (void)address;
    (void)info;
    return 0;
}

/* Only for IPv6 scope ids in URLs. */
unsigned if_nametoindex(const char *name)
{
    (void)name;
    return 0;
}

/* libcurl creates internal pipes with pipe2; emulate it with pipe + fcntl. */
int pipe2(int descriptors[2], int flags)
{
    if ((flags & ~(O_NONBLOCK | O_CLOEXEC)) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (pipe(descriptors) != 0)
        return -1;
    for (int i = 0; i < 2; ++i)
    {
        const int current = fcntl(descriptors[i], F_GETFL);
        if (current < 0 ||
            ((flags & O_NONBLOCK) != 0 &&
             fcntl(descriptors[i], F_SETFL, current | O_NONBLOCK) < 0) ||
            ((flags & O_CLOEXEC) != 0 && fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) < 0))
        {
            const int error = errno;
            close(descriptors[0]);
            close(descriptors[1]);
            errno = error;
            return -1;
        }
    }
    return 0;
}

/* Only QUIC/HTTP3 uses these, and curl falls back when they fail. */
int recvmmsg(int socket, void *messages, size_t count, int flags, const void *timeout)
{
    (void)socket;
    (void)messages;
    (void)count;
    (void)flags;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

int sendmmsg(int socket, void *messages, size_t count, int flags)
{
    (void)socket;
    (void)messages;
    (void)count;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

/* Referenced by the archives, never needed by an HTTP client: fail. */
__attribute__((weak)) FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

__attribute__((weak)) int pclose(FILE *stream)
{
    (void)stream;
    errno = ENOSYS;
    return -1;
}

/* zstd's optional tracing hooks, referenced by the archive. A begin that returns 0 means "not
 * traced". */
unsigned long long ZSTD_trace_compress_begin(const void *context)
{
    (void)context;
    return 0;
}

void ZSTD_trace_compress_end(unsigned long long trace, const void *record)
{
    (void)trace;
    (void)record;
}

unsigned long long ZSTD_trace_decompress_begin(const void *context)
{
    (void)context;
    return 0;
}

void ZSTD_trace_decompress_end(unsigned long long trace, const void *record)
{
    (void)trace;
    (void)record;
}

#endif

/* OpenSSL checks certificate validity dates with gmtime_r, so this one has to
 * be correct, not a stub. Days since 1970 to a civil date (Howard Hinnant's
 * algorithm). */
struct tm *gmtime_r(const time_t *when, struct tm *out)
{
    long long seconds = (long long)*when;
    long long days = seconds / 86400;
    long long rest = seconds % 86400;
    if (rest < 0)
    {
        rest += 86400;
        --days;
    }
    memset(out, 0, sizeof(*out));
    out->tm_hour = (int)(rest / 3600);
    out->tm_min = (int)(rest % 3600 / 60);
    out->tm_sec = (int)(rest % 60);
    out->tm_wday = (int)(((days % 7) + 11) % 7);
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    if (year - 1900 < INT_MIN || year - 1900 > INT_MAX)
    {
        errno = EOVERFLOW;
        return NULL;
    }
    out->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->tm_mon = (int)(month - 1);
    out->tm_year = (int)(year - 1900);
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    out->tm_yday = before[out->tm_mon] + out->tm_mday - 1 + (leap && out->tm_mon > 1 ? 1 : 0);
    return out;
}
