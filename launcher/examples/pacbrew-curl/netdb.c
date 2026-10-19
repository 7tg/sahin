// ProsperoRadio curl guide - getaddrinfo and friends on the system resolver.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// In the SDK's import stubs, getaddrinfo, freeaddrinfo, gai_strerror,
// gethostbyname and getnameinfo come from libScePosixForWebKit. A native
// title does not load that module, so the imports stay null and the first
// call jumps to address 0 (SIGSEGV). Defining them in the app makes the
// linker use these instead of the stubs.
//
// Scope: IPv4 only, one address per name, looked up with sceNetResolver on
// whichever thread libcurl calls from. That is all libcurl needs.

#include <arpa/inet.h>
#include <errno.h>
#include <fnmatch.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetPoolDestroy(int pool);
extern int sceNetResolverCreate(const char *name, int pool, int flags);
extern int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address,
                                   int timeout_us, int retries, int flags);
extern int sceNetResolverDestroy(int resolver);

static int lookup(const char *name, struct in_addr *address)
{
    if (inet_pton(AF_INET, name, address) == 1)
        return 0; /* already a dotted address */

    /* A small pool and resolver per lookup: simple and thread safe. */
    const int pool = sceNetPoolCreate("app_dns", 16 * 1024, 0);
    if (pool < 0)
        return EAI_MEMORY;
    int result = EAI_FAIL;
    const int resolver = sceNetResolverCreate("app_dns", pool, 0);
    if (resolver >= 0)
    {
        /* 5 s per try, 2 retries. */
        result =
            sceNetResolverStartNtoa(resolver, name, address, 5000000, 2, 0) < 0 ? EAI_NONAME : 0;
        sceNetResolverDestroy(resolver);
    }
    sceNetPoolDestroy(pool);
    return result;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    *result = NULL;
    const int family = hints != NULL ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY; /* libcurl then falls back to IPv4 */

    unsigned long port = 0;
    if (service != NULL)
    {
        char *end = NULL;
        errno = 0;
        port = strtoul(service, &end, 10);
        if (service[0] < '0' || service[0] > '9' || *end != '\0' || errno != 0 || port > 65535)
            return EAI_SERVICE;
    }

    struct in_addr address;
    address.s_addr = htonl(INADDR_LOOPBACK);
    if (node != NULL)
    {
        if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST) != 0)
        {
            if (inet_pton(AF_INET, node, &address) != 1)
                return EAI_NONAME;
        }
        else
        {
            const int failed = lookup(node, &address);
            if (failed != 0)
                return failed;
        }
    }
    else if (hints != NULL && (hints->ai_flags & AI_PASSIVE) != 0)
    {
        address.s_addr = htonl(INADDR_ANY);
    }

    /* One allocation: the entry followed by its address, so freeaddrinfo is
     * a single free per entry. */
    struct addrinfo *entry = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
    if (entry == NULL)
        return EAI_MEMORY;
    struct sockaddr_in *in = (struct sockaddr_in *)(entry + 1);
#ifndef __linux__
    in->sin_len = sizeof(*in); /* BSD sockaddr has a length field */
#endif
    in->sin_family = AF_INET;
    in->sin_port = htons((uint16_t)port);
    in->sin_addr = address;
    entry->ai_family = AF_INET;
    entry->ai_socktype =
        hints != NULL && hints->ai_socktype != 0 ? hints->ai_socktype : SOCK_STREAM;
    entry->ai_protocol = hints != NULL ? hints->ai_protocol : 0;
    entry->ai_addrlen = sizeof(*in);
    entry->ai_addr = (struct sockaddr *)in;
    *result = entry;
    return 0;
}

void freeaddrinfo(struct addrinfo *entry)
{
    while (entry != NULL)
    {
        struct addrinfo *next = entry->ai_next;
        free(entry);
        entry = next;
    }
}

const char *gai_strerror(int code)
{
    switch (code)
    {
    case 0:
        return "no error";
    case EAI_NONAME:
        return "the name was not found";
    case EAI_FAMILY:
        return "address family not supported";
    case EAI_MEMORY:
        return "out of memory";
    default:
        return "the name lookup failed";
    }
}

/* Referenced by the archives but not used by libcurl when getaddrinfo exists. */
struct hostent *gethostbyname(const char *name)
{
    (void)name;
    return NULL;
}

/* Numeric only: enough for libcurl's logging of the connected address. */
#ifdef __linux__
typedef socklen_t name_length;
#else
typedef size_t name_length;
#endif
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, name_length host_size,
                char *service, name_length service_size, int flags)
{
    (void)flags;
    if (address == NULL || address->sa_family != AF_INET || length < sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (host != NULL && host_size != 0 &&
        inet_ntop(AF_INET, &in->sin_addr, host, (socklen_t)host_size) == NULL)
        return EAI_FAIL;
    if (service != NULL && service_size != 0)
        snprintf(service, service_size, "%u", (unsigned)ntohs(in->sin_port));
    return 0;
}

/* Also imported from the same module. '*' and '?' only, which is all
 * libcurl's wildcard matching asks for. */
int fnmatch(const char *pattern, const char *text, int flags)
{
    (void)flags;
    for (; *pattern != '\0'; ++pattern, ++text)
    {
        if (*pattern == '*')
        {
            while (*pattern == '*')
                ++pattern;
            if (*pattern == '\0')
                return 0;
            for (; *text != '\0'; ++text)
            {
                if (fnmatch(pattern, text, flags) == 0)
                    return 0;
            }
            return FNM_NOMATCH;
        }
        if (*text == '\0' || (*pattern != '?' && *pattern != *text))
            return FNM_NOMATCH;
    }
    return *text == '\0' ? 0 : FNM_NOMATCH;
}
