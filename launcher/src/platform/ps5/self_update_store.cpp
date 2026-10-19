// Prospero BlackBox - The store replacing itself, through the boilerplate's self-update kit.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/self_update_store.hpp"
#include "platform/ps5/system.hpp"
#include "system/worker_launch.hpp"
#include "self_update.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <vector>

namespace store::system
{
namespace
{
// The kit's view of the console, built on the store's own pieces: its libcurl
// transport (with its fixes and GitHub-only address rules) for the download and
// its loader connection for the helper. The catalog check is the store's own,
// so the kit's fetch, signature and sequence hooks are never used.
struct Context
{
    net::Control *control = nullptr;
};

int no_fetch(void *, const char *, char *, std::size_t, std::size_t *, int *)
{
    return -1;
}
int no_verify(void *, const unsigned char *, const unsigned char *, const void *, std::size_t)
{
    return 0;
}
int no_sequence(void *, std::uint64_t *)
{
    return 0;
}
int no_save(void *, std::uint64_t)
{
    return 0;
}
std::uint64_t now_ms(void *)
{
    return static_cast<std::uint64_t>(hui::sys::monotonic_us() / 1000);
}

int download(void *user, const char *url, std::uint64_t limit, self_update_sink sink,
             void *sink_user)
{
    auto &control = *static_cast<Context *>(user)->control;
    const std::string address = url;
#ifdef STORE_DEVELOPMENT
    // A scripted self-update reads its archive from the console, since no newer
    // store is published while it is being tested (development builds only).
    if (address.ends_with("/dev/PPSA99400.zip"))
    {
        const int file = open("/data/nfs-blackbox/dev/self.zip", O_RDONLY);
        if (file < 0)
            return -1;
        std::vector<char> piece(256 * 1024);
        std::uint64_t total = 0;
        bool ok = true;
        for (;;)
        {
            const auto count = read(file, piece.data(), piece.size());
            if (count <= 0)
            {
                ok = count == 0;
                break;
            }
            total += static_cast<std::uint64_t>(count);
            if (total > limit || control.cancelled.load() ||
                sink(sink_user, piece.data(), static_cast<std::size_t>(count)) != 1)
            {
                ok = false;
                break;
            }
        }
        close(file);
        return ok ? 0 : -1;
    }
#endif
    const auto response = net::get(
        address, net::Purpose::artifact, limit, [&](std::string_view piece)
        { return sink(sink_user, piece.data(), piece.size()) == 1; }, control);
    return response.ok() && response.status == 200 ? 0 : -1;
}

long channel_send(void *user, const void *data, std::size_t size)
{
    return static_cast<install::Channel *>(user)->send(data, size);
}
long channel_receive(void *user, void *data, std::size_t size)
{
    return static_cast<install::Channel *>(user)->receive(data, size);
}
void channel_close(void *user)
{
    auto *channel = static_cast<install::Channel *>(user);
    channel->close();
    delete channel;
}

int open_helper(void *, self_update_channel *out)
{
    auto channel = std::make_unique<install::Channel>();
    if (!launch_program("self-updater.elf", *channel))
        return 0;
    out->send = channel_send;
    out->receive = channel_receive;
    out->close = channel_close;
    out->user = channel.release();
    return 1;
}

template <std::size_t N> void copy(char (&out)[N], const std::string &text)
{
    std::snprintf(out, N, "%s", text.c_str());
}
} // namespace

bool update_self(const catalog::Entry &entry, const std::string &installed,
                 install::Progress &progress, net::Control &control, std::string &error)
{
    Context context{&control};
    const self_update_platform platform{no_fetch,    download, no_verify, open_helper,
                                        no_sequence, no_save,  now_ms,    &context};
    self_update_offer offer{};
    copy(offer.title, entry.id);
    copy(offer.name, entry.name);
    copy(offer.installed, installed);
    copy(offer.available, entry.content_version);
    copy(offer.version, entry.version);
    copy(offer.artifact, entry.artifact);
    copy(offer.sha256, entry.digest);
    offer.size = entry.size;
    self_update_job job{};
    if (self_update_start(&job, &platform, &offer) != 1)
    {
        error = "The update could not be started";
        return false;
    }
    bool applied = false;
    bool cancel_sent = false;
    for (;;)
    {
        self_update_status status{};
        self_update_poll(&job, &status);
        const auto phase =
            status.phase == SELF_UPDATE_UNPACKING ? install::Phase::unpacking
            : status.phase == SELF_UPDATE_READY || status.phase == SELF_UPDATE_APPLYING
                ? install::Phase::activating
                : install::Phase::downloading;
        progress.phase = static_cast<int>(phase);
        progress.done = status.done;
        progress.total = status.total;
        if (control.cancelled.load() && !cancel_sent && status.phase != SELF_UPDATE_READY &&
            status.phase < SELF_UPDATE_APPLYING)
        {
            self_update_cancel(&job);
            cancel_sent = true;
        }
        if (status.phase == SELF_UPDATE_READY)
        {
            if (control.cancelled.load())
            {
                self_update_cancel(&job);
                error = "Cancelled";
                break;
            }
            applied = self_update_apply(&job) == 1;
            if (!applied)
                error = "The update helper stopped";
            break;
        }
        if (status.phase == SELF_UPDATE_FAILED || status.phase == SELF_UPDATE_CANCELLED)
        {
            error = status.phase == SELF_UPDATE_CANCELLED || status.error[0] == '\0'
                        ? std::string("Cancelled")
                        : std::string(status.error);
            break;
        }
        hui::sys::sleep_us(100000);
    }
    self_update_finish(&job);
    return applied;
}
} // namespace store::system
