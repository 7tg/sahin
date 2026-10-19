// Prospero BlackBox - Starts the file worker through the console's payload loader.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef STORE_FILE_WORKER
#include "system/worker_launch.hpp"
#include "system/app_folder.hpp"
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <vector>

namespace
{
struct NetSockaddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

extern "C"
{
    int sceKernelOpen(const char *path, int flags, mode_t mode);
    int sceKernelClose(int descriptor);
    std::int64_t sceKernelRead(int descriptor, void *buffer, std::size_t length);
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetSocketClose(int socket);
}

bool send_all(int socket, const std::uint8_t *data, std::size_t size)
{
    while (size)
    {
        const int count = sceNetSend(socket, data, size, 0);
        if (count <= 0)
            return false;
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

// The loader reads the program up to the end of its section table and gives
// the rest of the connection to the program as its standard input and output.
bool submit(int socket, int program)
{
    constexpr int socket_level = 0xffff;
    // The worker reports five times a second; half a minute of silence means it is gone.
    constexpr int timeout_us = 30'000'000;
    constexpr int connect_us = 5'000'000;
    if (sceNetSetsockopt(socket, socket_level, 0x1105, &timeout_us, sizeof(timeout_us)) < 0 ||
        sceNetSetsockopt(socket, socket_level, 0x1106, &timeout_us, sizeof(timeout_us)) < 0 ||
        sceNetSetsockopt(socket, socket_level, 0x1109, &connect_us, sizeof(connect_us)) < 0)
    {
        std::printf("[STORE] worker: socket options refused\n");
        return false;
    }
    constexpr std::uint16_t port = 9021;
    const NetSockaddrIn address{sizeof(NetSockaddrIn),
                                2,
                                static_cast<std::uint16_t>((port << 8) | (port >> 8)),
                                0x0100007f,
                                0,
                                {0}};
    if (const int result = sceNetConnect(socket, &address, sizeof(address)); result < 0)
    {
        std::printf("[STORE] worker: the loader did not answer (0x%08x)\n",
                    static_cast<unsigned>(result));
        return false;
    }
    std::vector<std::uint8_t> buffer(65536);
    for (;;)
    {
        const auto count = sceKernelRead(program, buffer.data(), buffer.size());
        if (count == 0)
            return true;
        if (count < 0 || !send_all(socket, buffer.data(), static_cast<std::size_t>(count)))
        {
            std::printf("[STORE] worker: the program could not be sent\n");
            return false;
        }
    }
}
} // namespace

namespace store::system
{
bool launch_worker(install::Channel &channel)
{
    return launch_program("store-worker.elf", channel);
}

bool launch_program(const char *file, install::Channel &channel)
{
    // Once the store has left its sandbox, /app0 may no longer be its own folder:
    // app_folder() finds it wherever it is mounted.
    int program = -1;
    program = sceKernelOpen((app_folder() + "/" + file).c_str(), O_RDONLY, 0);
    if (program < 0)
    {
        std::printf("[STORE] worker: the program is missing (0x%08x)\n",
                    static_cast<unsigned>(program));
        return false;
    }
    const int socket = sceNetSocket("store_worker", 2, 1, 6);
    if (socket < 0)
        std::printf("[STORE] worker: no socket (0x%08x)\n", static_cast<unsigned>(socket));
    const bool sent = socket >= 0 && submit(socket, program);
    (void)sceKernelClose(program);
    if (!sent)
    {
        if (socket >= 0)
            (void)sceNetSocketClose(socket);
        return false;
    }
    channel.send = [socket](const void *data, std::size_t size)
    { return static_cast<long>(sceNetSend(socket, data, size, 0)); };
    channel.receive = [socket](void *data, std::size_t size)
    { return static_cast<long>(sceNetRecv(socket, data, size, 0)); };
    channel.close = [socket] { (void)sceNetSocketClose(socket); };
    return true;
}
} // namespace store::system
#endif
