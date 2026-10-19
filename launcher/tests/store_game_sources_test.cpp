// Prospero BlackBox - A plan's sources put in mods-cache: shipped, downloaded, refused.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/game_sources.hpp"
#include "third_party/picosha2/picosha2.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace store::library;

namespace
{
std::string sha(const std::string &body)
{
    return picosha2::hash256_hex_string(body);
}

void put(const fs::path &path, const std::string &body)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << body;
}

std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string source(const std::string &id, const std::string &body, const std::string &extra = "")
{
    return "\"" + id + "\": {\"name\": \"" + id + "\", \"sha256\": \"" + sha(body) +
           "\", \"size\": " + std::to_string(body.size()) + extra + "}";
}
} // namespace

int main()
{
    char temporary[] = "/tmp/prospero-sources-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path root = fs::canonical(temporary), cache = root / "mods-cache", bundled = root / "bundle";
    // fetched varies byte to byte, so a piece put at the wrong offset changes its hash.
    std::string fetched(300000, '\0');
    for (std::size_t i = 0; i < fetched.size(); ++i)
        fetched[i] = static_cast<char>((i * 7919u) >> 3);
    const std::string shipped = "a file the manager carries", upgrade = "an upgrade",
                      by_hand = "nobody can download this";

    // The plan: a shipped file, a downloadable one, an upgrade (only an upgrade source),
    // and one with no way to get it, which a second plan leaves out.
    const auto plan_text = [&](bool with_by_hand)
    {
        return "{\"schema\": 1, \"game\": \"GAME\", \"title\": \"The Game\", \"release\": \"2.0\","
               " \"prefix\": \"shared\", \"sources\": {" +
               source("shipped", shipped) + ", " +
               source("fetched", fetched, ", \"url\": \"https://example.com/fetched.zip\"") + ", " +
               source("upgrade", upgrade, ", \"url\": \"https://example.com/upgrade.zip\"") + ", " +
               source("hand", by_hand) + "}, \"game_files\": ["
               "{\"source\": \"shipped\", \"to\": \"a.bin\"}, {\"source\": \"fetched\", \"to\": \"b.bin\"}" +
               std::string(with_by_hand ? ", {\"source\": \"hand\", \"to\": \"c.bin\"}" : "") +
               "], \"upgrades\": [{\"from\": \"1.0\", \"source\": \"upgrade\"}],"
               " \"profile\": {\"id\": \"the-game\", \"name\": \"The Game\","
               " \"executable\": \"C:\\\\Games\\\\GAME\\\\game.exe\","
               " \"working_directory\": \"C:\\\\Games\\\\GAME\", \"width\": 1920, \"height\": 1080}}";
    };
    InstallPlan plan, full;
    std::string error;
    if (!parse_plan(plan_text(false), plan, error) || !parse_plan(plan_text(true), full, error))
    {
        std::cerr << "plan: " << error << "\n";
        return 1;
    }
    assert(needed_sources(plan).size() == 3 && needed_sources(full).size() == 4);

    put(bundled / sha(shipped), shipped);
    std::map<std::string, std::string> served{{"https://example.com/fetched.zip", fetched},
                                              {"https://example.com/upgrade.zip", upgrade}};
    std::vector<std::string> asked;
    std::mutex asking; // downloads run on several threads at once
    std::size_t ranges = 0; // ranged requests asked for
    const Download download = [&](std::size_t, const std::string &url, std::uint64_t offset,
                                  std::uint64_t length,
                                  bool ranged, const std::function<bool(std::string_view)> &sink,
                                  std::string &why)
    {
        {
            std::lock_guard held(asking);
            asked.push_back(url);
            ranges += ranged ? 1 : 0;
        }
        const auto found = served.find(url);
        if (found == served.end())
        {
            why = "404";
            return false;
        }
        const std::string &whole = found->second;
        if (offset > whole.size() || (!ranged && (offset != 0 || length != whole.size())))
        {
            why = "416";
            return false;
        }
        const std::string body = whole.substr(offset, length);
        for (std::size_t at = 0; at < body.size(); at += 65536) // in pieces, as a download comes
            if (!sink(std::string_view(body).substr(at, 65536)))
                return false;
        return true;
    };
    std::atomic<bool> go{false};
    std::size_t last_count = 0;
    std::uint64_t most = 0, all_last = 0, all_total = 0;
    bool all_rising = true;
    const auto progress = [&](const FetchStatus &status)
    {
        last_count = status.count;
        most = std::max(most, status.done);
        all_rising = all_rising && status.all_done >= all_last && status.all_done <= status.all_total;
        all_last = status.all_done;
        all_total = status.all_total;
    };

    // Everything arrives: shipped from the bundle, the rest downloaded; the cache holds
    // whole files under their SHA-256 and nothing half-written.
    assert(fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error));
    assert(last_count == 3 && most >= fetched.size() && most <= all_total);
    // The bytes of all of them: only ever rising, and reaching the sum of the three.
    assert(all_rising && all_total == shipped.size() + fetched.size() + upgrade.size() &&
           all_last == all_total);
    assert(read(cache / sha(shipped)) == shipped && read(cache / sha(fetched)) == fetched &&
           read(cache / sha(upgrade)) == upgrade);
    assert(asked.size() == 2); // the shipped one was never downloaded
    for (const auto &entry : fs::directory_iterator(cache))
        assert(entry.path().filename().string().find(".part") == std::string::npos);

    assert(ranges == 0); // each file smaller than a piece: whole requests

    // In ranges: the big file in 70,000-byte pieces (the last one shorter), put together
    // right; the small ones still whole.
    fs::remove_all(cache);
    asked.clear();
    assert(fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error, 70000));
    assert(ranges == 5 && asked.size() == 6 && read(cache / sha(fetched)) == fetched &&
           read(cache / sha(upgrade)) == upgrade);
    for (const auto &entry : fs::directory_iterator(cache))
        assert(entry.path().filename().string().find(".part") == std::string::npos);

    // Run again: nothing is fetched (all cached).
    asked.clear();
    assert(fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error) && asked.empty());

    // A download that comes out wrong is refused and leaves nothing behind.
    fs::remove(cache / sha(fetched));
    served["https://example.com/fetched.zip"] = std::string(300000, 'x');
    assert(!fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error));
    assert(error.find("didn't arrive as the plan pins it") != std::string::npos);
    assert(!fs::exists(cache / sha(fetched)) && !fs::exists(cache / (sha(fetched) + ".part")));
    // In ranges too: its pieces and its joined file are gone.
    assert(!fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error, 70000));
    assert(error.find("didn't arrive as the plan pins it") != std::string::npos);
    for (const auto &entry : fs::directory_iterator(cache))
        assert(entry.path().filename().string().find(".part") == std::string::npos);
    served["https://example.com/fetched.zip"] = fetched;

    // A failed download says why; a source nobody can download is named.
    served.erase("https://example.com/fetched.zip");
    assert(!fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error));
    assert(error == "Can't download fetched: 404");
    served["https://example.com/fetched.zip"] = fetched;
    assert(!fetch_sources(full, cache.string(), bundled.string(), download, progress, go, error));
    assert(error == "hand can't be downloaded: put it in mods-cache by hand");
    // ...but one put there by hand counts.
    put(cache / (sha(by_hand) + ".zip"), by_hand);
    assert(fetch_sources(full, cache.string(), bundled.string(), download, progress, go, error));

    // A wrong file in the bundle is not taken from it.
    fs::remove_all(cache);
    put(bundled / sha(shipped), "not the shipped file");
    assert(!fetch_sources(plan, cache.string(), bundled.string(), download, progress, go, error));
    assert(error == "shipped can't be downloaded: put it in mods-cache by hand");

    // Cancelled: nothing more is fetched.
    put(bundled / sha(shipped), shipped);
    std::atomic<bool> stop{true};
    assert(!fetch_sources(plan, cache.string(), bundled.string(), download, progress, stop, error) &&
           error == "Cancelled");

    fs::remove_all(root);
    std::cout << "Game sources checks passed\n";
}
