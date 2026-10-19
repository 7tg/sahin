// Prospero BlackBox - Inventory ownership, scan depth, unsafe metadata and cancellation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/inventory.hpp"
#include "catalog/catalog.hpp"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
std::string metadata(const std::string &id)
{
    return "{\"titleId\":\"" + id +
           "\",\"contentVersion\":\"01.000.001\","
           "\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":"
           "\"Example App\"}}}";
}
void title(const fs::path &path, const std::string &id)
{
    fs::create_directories(path / "sce_sys");
    std::ofstream(path / "sce_sys/param.json") << metadata(id);
}
int main()
{
    char temporary[] = "/tmp/prospero-inventory-XXXXXX";
    assert(mkdtemp(temporary));
    // Its real path: the scan refuses symlinks, and macOS's /tmp is one.
    const fs::path root = fs::canonical(temporary), apps = root / "apps", receipts = root / "receipts";
    fs::create_directory(receipts);
    title(apps / "PPSA99010", "PPSA99010");
    title(apps / "PPSA99020", "PPSA99020");
    title(apps / "group/PPSA99040", "PPSA99040");
    title(apps / "backports/PPSA99050", "PPSA99050");
    title(root / "manual/PPSA99030", "PPSA99030");
    std::ofstream(apps / "example.FFPKG") << "unchanged image";
    fs::create_directory_symlink(apps / "PPSA99010", apps / "linked");
    const auto receipt =
        "{\"schema\":1,\"titleId\":\"PPSA99010\",\"location\":\"" + apps.string() +
        "\",\"contentVersion\":\"01.000.001\",\"releaseTag\":\"v1\",\"sha256\":\"" +
        std::string(64, 'a') + "\",\"installedAt\":\"2026-10-02T00:00:00Z\"}";
    std::ofstream(receipts / "PPSA99010.json") << receipt;
    std::atomic<bool> cancelled{false};
    store::system::ScanPolicy policy;
    policy.roots = {apps.string()};
    policy.manual = {(root / "manual/PPSA99030").string()};
    auto result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(result.complete && result.apps.size() == 4);
    const auto managed = std::find_if(result.apps.begin(), result.apps.end(),
                                      [](const auto &app) { return app.managed; });
    assert(managed != result.apps.end() && managed->id == "PPSA99010" &&
           managed->name == "Example App");
    assert(std::count_if(result.apps.begin(), result.apps.end(),
                         [](const auto &app) { return app.managed; }) == 1);
    assert(std::count_if(result.apps.begin(), result.apps.end(),
                         [](const auto &app) { return app.image; }) == 1);
    auto wrong_version = receipt;
    wrong_version.replace(wrong_version.find("01.000.001"), 10, "01.000.002");
    std::ofstream(receipts / "PPSA99010.json") << wrong_version;
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(std::none_of(result.apps.begin(), result.apps.end(),
                        [](const auto &app) { return app.managed; }));
    auto wrong_location = receipt;
    wrong_location.replace(wrong_location.find(apps.string()), apps.string().size(), root.string());
    std::ofstream(receipts / "PPSA99010.json") << wrong_location;
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(std::none_of(result.apps.begin(), result.apps.end(),
                        [](const auto &app) { return app.managed; }));
    std::ofstream(receipts / "PPSA99010.json") << receipt;
    policy.depth = 2;
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(result.complete && result.apps.size() == 5); // Backports and symlinks stay excluded.
    title(apps / "duplicate", "PPSA99010");
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(std::count_if(result.apps.begin(), result.apps.end(),
                         [](const auto &app) { return app.duplicate; }) == 2);
    assert(std::none_of(result.apps.begin(), result.apps.end(),
                        [](const auto &app) { return app.managed; }));
    fs::remove_all(apps / "duplicate");
    std::ofstream(receipts / "PPSA99010.json") << "{\"schema\":1,\"schema\":1}";
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(std::none_of(result.apps.begin(), result.apps.end(),
                        [](const auto &app) { return app.managed; }));
    const auto param = apps / "PPSA99020/sce_sys/param.json";
    fs::remove(param);
    fs::create_symlink(apps / "PPSA99010/sce_sys/param.json", param);
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(!result.complete && !result.errors.empty());
    assert(fs::file_size(apps / "example.FFPKG") == 15);
    cancelled = true;
    result = store::system::scan_installed(policy, receipts.string(), cancelled);
    assert(!result.complete && result.apps.empty());
    store::catalog::Entry entry;
    std::string error;
    assert(!store::catalog::parse_installed("{\"titleId\":\"PPSA99010\",\"titleId\":\"PPSA99020\"}",
                                            entry, error));
    assert(!store::catalog::parse_installed(
        "{\"titleId\":\"../../etc\",\"contentVersion\":\"01.000.001\"}", entry, error));
    assert(!store::catalog::parse_installed(std::string(65537, ' '), entry, error));
    fs::remove_all(root);
}
