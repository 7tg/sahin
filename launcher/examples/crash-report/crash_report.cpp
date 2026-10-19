// ps5-native-app-boilerplate - ProsperoEden's crash/restart flow for native apps.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "crash_report.hpp"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <ucontext.h>
#endif

namespace crash_report
{
namespace
{
constexpr int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP};
std::atomic<bool> reporting{false}, reported{false}, stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);
char report_path[256]{}, note_path[256]{}, app_version[48]{};
bool installed = false, after_crash = false;
pthread_t worker{};
void (*leave_app)(bool) = nullptr;
struct sigaction previous[7]{};

void pause(unsigned milliseconds)
{
    timespec time{static_cast<time_t>(milliseconds / 1000),
                  static_cast<long>(milliseconds % 1000) * 1000000};
    while (::nanosleep(&time, &time) != 0 && errno == EINTR)
    {
    }
}

// As in Eden, no formatter, allocation, mutex, or stdio stream is used here.
struct Text
{
    char bytes[2048]{};
    std::size_t size = 0;
    void put(const char *text)
    {
        while (*text && size + 1 < sizeof(bytes))
            bytes[size++] = *text++;
    }
    void hex(std::uint64_t number)
    {
        char value[17]{};
        for (int i = 15; i >= 0; --i)
        {
            value[i] = "0123456789abcdef"[number & 15];
            number >>= 4;
        }
        put(value);
    }
};

bool write_file(const char *path, const char *data, std::size_t size)
{
    const int file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0)
        return false;
    std::size_t done = 0;
    while (done < size)
    {
        const auto count = ::write(file, data + done, size - done);
        if (count > 0)
            done += static_cast<std::size_t>(count);
        else if (count == 0 || errno != EINTR)
            break;
    }
    const bool synced = ::fsync(file) == 0;
    return ::close(file) == 0 && synced && done == size;
}

void standard_action(int signal)
{
    struct sigaction action
    {
    };
    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(signal, &action, nullptr);
    ::kill(::getpid(), signal);
}

void handle(int signal, siginfo_t *info, void *context)
{
    if (reporting.exchange(true))
    {
        standard_action(signal);
        return;
    }
    Text text;
    text.put("Native application crash report\nversion: ");
    text.put(app_version);
    text.put("\nsignal: 0x");
    text.hex(static_cast<std::uint64_t>(signal));
    text.put("\nfault address: 0x");
    text.hex(info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
    if (context)
    {
        std::uint64_t pc = 0, sp = 0, bp = 0;
#ifdef __linux__
        const auto &registers = static_cast<ucontext_t *>(context)->uc_mcontext.gregs;
        pc = static_cast<std::uint64_t>(registers[REG_RIP]);
        sp = static_cast<std::uint64_t>(registers[REG_RSP]);
        bp = static_cast<std::uint64_t>(registers[REG_RBP]);
#else
        // Eden-qualified FreeBSD context: machine context begins at byte 64.
        const auto *registers = static_cast<const std::uint64_t *>(context) + 8;
        pc = registers[20];
        sp = registers[23];
        bp = registers[9];
#endif
        text.put("\nrip: 0x");
        text.hex(pc);
        text.put("\nrsp: 0x");
        text.hex(sp);
        text.put("\nrbp: 0x");
        text.hex(bp);
    }
    text.put(after_crash ? "\naction: close (restart loop prevented)\n" : "\naction: restart\n");
    if (write_file(report_path, text.bytes, text.size))
        write_file(note_path, "1\n", 2);
    reported.store(true, std::memory_order_release);
    for (int i = 0; i < 60; ++i)
        pause(100);
    standard_action(signal);
}

void *restart_worker(void *)
{
    while (!reported.load(std::memory_order_acquire) && !stopping.load())
        pause(100);
    if (reported.load(std::memory_order_acquire))
        leave_app(!after_crash);
    return nullptr;
}
} // namespace

bool install(const char *directory, const char *version, void (*lifecycle)(bool))
{
    if (installed || !directory || !version || !lifecycle || std::strlen(directory) > 210 ||
        std::strlen(version) >= sizeof(app_version))
        return false;
    const auto length = std::strlen(directory);
    std::memcpy(report_path, directory, length);
    std::memcpy(report_path + length, "/crash-latest.txt", sizeof("/crash-latest.txt"));
    std::memcpy(note_path, directory, length);
    std::memcpy(note_path + length, "/crash-pending", sizeof("/crash-pending"));
    std::memcpy(app_version, version, std::strlen(version) + 1);
    after_crash = ::access(note_path, F_OK) == 0;
    if (after_crash)
        ::unlink(note_path);
    leave_app = lifecycle;
    reporting.store(false);
    reported.store(false);
    stopping.store(false);
    if (pthread_create(&worker, nullptr, restart_worker, nullptr) != 0)
        return false;
    struct sigaction action
    {
    };
    action.sa_sigaction = handle;
    action.sa_flags = SA_SIGINFO;
    ::sigemptyset(&action.sa_mask);
    unsigned count = 0;
    for (; count < 7; ++count)
        if (::sigaction(signals[count], &action, &previous[count]) != 0)
            break;
    if (count != 7)
    {
        while (count > 0)
        {
            --count;
            ::sigaction(signals[count], &previous[count], nullptr);
        }
        stopping.store(true);
        pthread_join(worker, nullptr);
        return false;
    }
    installed = true;
    return true;
}

bool recovered()
{
    return after_crash;
}
void stop()
{
    if (!installed)
        return;
    for (unsigned i = 0; i < 7; ++i)
        ::sigaction(signals[i], &previous[i], nullptr);
    stopping.store(true);
    pthread_join(worker, nullptr);
    installed = false;
}
} // namespace crash_report
