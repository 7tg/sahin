// Sahin - The launcher's installer run on the host against a folder (the host install check).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// host-install PLAN.json DATA_ROOT WIN_ROOT [--bare-prefix ZIP] [--size WxH] [option...]
//
// The same steps as the console's install job (app/service.cpp): the plan's chosen options'
// sources fetched into DATA_ROOT/mods-cache (downloaded with the host's curl), the prefix made
// from the bare-prefix zip when WIN_ROOT/prefixes/<prefix> isn't one yet, then install_game
// from DATA_ROOT/base-games/<game>. Each stage is printed; exit 0 when installed, 1 otherwise.
#include "library/base_game.hpp"
#include "library/game_install.hpp"
#include "library/game_sources.hpp"
#include "library/install_plan.hpp"
#include "library/prefix.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace
{
using namespace store::library;

std::string quoted(const std::string &text)
{
    std::string out = "'";
    for (const char c : text)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The host's curl, its body piped to the sink: only ever the plan's pinned https URLs, and
// fetch_sources hashes what arrives before it keeps it.
bool curl_download(std::size_t, const std::string &url, std::uint64_t offset, std::uint64_t length,
                   bool ranged, const std::function<bool(std::string_view)> &sink, std::string &error)
{
    std::string command = "curl -fsSL --retry 3 --proto =https";
    if (ranged)
        command += " -r " + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
    command += " " + quoted(url);
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        error = "can't run curl";
        return false;
    }
    std::vector<char> buffer(1 << 16);
    std::uint64_t got = 0;
    bool stopped = false;
    std::size_t read = 0;
    while ((read = fread(buffer.data(), 1, buffer.size(), pipe)) > 0)
    {
        got += read;
        if (got > length || !sink(std::string_view(buffer.data(), read)))
        {
            stopped = true;
            break;
        }
    }
    const int status = pclose(pipe);
    if (stopped)
    {
        error = got > length ? "more bytes than expected from " + url : "stopped";
        return false;
    }
    if (status != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        error = "curl failed on " + url;
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s PLAN.json DATA_ROOT WIN_ROOT [--bare-prefix ZIP] "
                             "[--size WxH] [option...]\n",
                     argv[0]);
        return 2;
    }
    InstallPlan plan;
    std::string error;
    if (!load_plan(argv[1], plan, error))
    {
        std::fprintf(stderr, "plan: %s\n", error.c_str());
        return 1;
    }
    const Manifest *manifest = manifest_for(plan.game);
    if (manifest == nullptr)
    {
        std::fprintf(stderr, "no base-game manifest for %s\n", plan.game.c_str());
        return 1;
    }
    InstallPaths paths{argv[2], argv[3]};
    InstallOptions options;
    options.now = 0;
    options.app_version = "host-install";
    std::string bare;
    for (int i = 4; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--bare-prefix" && i + 1 < argc)
            bare = argv[++i];
        else if (arg == "--size" && i + 1 < argc)
        {
            const auto size = resolution_named(argv[++i]);
            options.width = size.width;
            options.height = size.height;
        }
        else
            options.chosen.push_back(arg);
    }
    std::atomic<bool> cancel{false};
    std::printf("== %s: options", plan.title.c_str());
    for (const auto &id : options.chosen)
        std::printf(" %s", id.c_str());
    std::printf("\n");

    std::printf("[%s]\n", kGameStages[1]);
    const auto chosen_plan = effective_plan(plan, options);
    std::string last;
    if (!fetch_sources(
            chosen_plan, paths.data_root + "/mods-cache", "", curl_download,
            [&](const FetchStatus &status)
            {
                if (!status.finished.empty())
                    std::printf("  fetched %s (%llu bytes, %.1f s)\n", status.finished.c_str(),
                                static_cast<unsigned long long>(status.finished_bytes),
                                status.finished_seconds);
            },
            cancel, error))
    {
        std::fprintf(stderr, "downloads: %s\n", error.c_str());
        return 1;
    }
    for (const auto *source : needed_sources(chosen_plan))
    {
        options.verified.insert(source->sha256);
        std::printf("  %s ready\n", source->name.c_str());
    }

    std::printf("[%s]\n", kGameStages[2]);
    if (prefix_ready(paths.win_root, chosen_plan.prefix))
        std::printf("  using the prefix %s\n", chosen_plan.prefix.c_str());
    else
    {
        if (bare.empty())
        {
            std::fprintf(stderr, "no prefix %s and no --bare-prefix\n", chosen_plan.prefix.c_str());
            return 1;
        }
        std::printf("  making the prefix %s from %s\n", chosen_plan.prefix.c_str(), bare.c_str());
        if (!ensure_prefix(paths.win_root, chosen_plan.prefix, bare,
                           [](std::uint64_t, std::uint64_t) {}, cancel, error))
        {
            std::fprintf(stderr, "prefix: %s\n", error.c_str());
            return 1;
        }
    }

    const bool installed = install_game(
        plan, *manifest, paths, options,
        [&](const InstallStatus &status)
        {
            if (status.step == last)
                return;
            last = status.step;
            std::printf("  %s\n", status.step.c_str());
            std::fflush(stdout);
        },
        cancel, error);
    if (!installed)
    {
        std::fprintf(stderr, "install: %s\n", error.c_str());
        return 1;
    }
    std::printf("installed\n");
    return 0;
}
