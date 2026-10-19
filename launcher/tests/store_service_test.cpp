// Prospero BlackBox - The service: the built-in catalog, settings and the installer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/service.hpp"
#include "catalog/icons.hpp"
#include "core/save_file.hpp"
#include "third_party/miniz/miniz.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
namespace
{
store::Update next(store::Service &service, store::Update::Kind kind)
{
    // The notice arrives with the catalog it follows: keep what a call didn't ask for.
    static std::vector<store::Update> waiting;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (auto update = waiting.begin(); update != waiting.end(); ++update)
            if (update->kind == kind)
            {
                auto found = std::move(*update);
                waiting.erase(waiting.begin(), update + 1);
                return found;
            }
        waiting.clear();
        service.take(waiting);
        if (!waiting.empty())
            continue;
        std::this_thread::sleep_for(5ms);
    }
    assert(false && "worker response timed out");
    return {};
}
} // namespace
namespace store::net
{
void Control::cancel()
{
    cancelled = true;
}
Response fetch(const std::string &, Purpose, std::size_t, std::string &, Control &,
               const std::string &)
{
    Response failure;
    failure.error = "The test has no network";
    return failure;
}
Response get(const std::string &, Purpose, std::uint64_t, const Sink &, Control &,
             const std::string &)
{
    Response failure;
    failure.error = "The test has no network";
    return failure;
}
} // namespace store::net

// The installer through the service: one worker, a queue, progress the frame
// can read without waiting, and a result for every request.
static void check_installer()
{
    namespace fs = std::filesystem;
    char path[] = "/tmp/prospero-installer-XXXXXX";
    assert(mkdtemp(path));
    const fs::path root = fs::canonical(path); // macOS's /tmp is a symlink
    fs::create_directories(root / "apps");
    const std::string id = "PPSA99500", location = (root / "apps").string();
    // The smallest valid app archive.
    mz_zip_archive archive{};
    assert(mz_zip_writer_init_heap(&archive, 0, 0));
    const std::string program(300000, 'p'),
        param = "{\"titleId\":\"" + id + "\",\"contentVersion\":\"01.000.001\"}";
    assert(mz_zip_writer_add_mem(&archive, (id + "/eboot.bin").c_str(), program.data(),
                                 program.size(), 6));
    assert(mz_zip_writer_add_mem(&archive, (id + "/sce_sys/param.json").c_str(), param.data(),
                                 param.size(), 6));
    void *bytes = nullptr;
    std::size_t size = 0;
    assert(mz_zip_writer_finalize_heap_archive(&archive, &bytes, &size));
    const std::string artifact(static_cast<const char *>(bytes), size);
    mz_free(bytes);
    mz_zip_writer_end(&archive);

    std::atomic<bool> hold{false};
    store::install::Environment environment;
    environment.root = (root / "state").string();
    environment.work = (root / "work").string();
    environment.self = "PPSA99400";
    environment.policy.roots = {location};
    environment.fetch = [&](const std::string &, std::uint64_t, const store::net::Sink &sink,
                            store::net::Control &control)
    {
        store::net::Response response;
        sink(std::string_view(artifact).substr(0, artifact.size() / 2));
        while (hold && !control.cancelled)
            std::this_thread::sleep_for(5ms);
        if (control.cancelled || !sink(std::string_view(artifact).substr(artifact.size() / 2)))
            response.error = "Cancelled";
        else
            response.status = 200;
        return response;
    };
    store::catalog::Entry entry;
    entry.id = id;
    entry.name = "Example";
    entry.status = "available";
    entry.format = "zip";
    entry.version = "v1";
    entry.content_version = "01.000.001";
    entry.artifact = "https://github.com/example/app/releases/download/v1/" + id + ".zip";
    entry.digest = store::catalog::sha256(artifact);
    entry.size = artifact.size();

    store::Service service("");
    assert(!service.request_install(entry, location)); // No installer: nothing is queued.
    service.installer = true;
    service.installer_environment = environment;
    assert(service.start());
    const auto ask = [&](const auto &request)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!request() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    };
    // A held download shows its progress, and a cancel leaves nothing behind.
    hold = true;
    ask([&] { return service.request_install(entry, location); });
    store::JobView view;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           !(service.job(view) && view.id == id && view.done > 0))
        std::this_thread::sleep_for(1ms);
    assert(view.id == id && view.phase == store::install::Phase::downloading);
    assert(view.done == artifact.size() / 2 && view.total == artifact.size());
    ask([&] { return service.cancel_job(id); });
    auto done = next(service, store::Update::Kind::job);
    assert(!done.ok && done.entry.id == id && done.message == "Example: cancelled");
    assert(!fs::exists(root / "apps" / id) && !fs::exists(root / "state/journal.json"));
    // The next request is not cancelled by the previous one's cancel.
    hold = false;
    ask([&] { return service.request_install(entry, location); });
    done = next(service, store::Update::Kind::job);
    assert(done.ok && done.message == "Example installed");
    assert(fs::file_size(root / "apps" / id / "eboot.bin") == program.size());
    assert(fs::exists(root / "state/receipts" / (id + ".json")));
    // Without a running-app check an uninstall is refused, and says so.
    ask([&] { return service.request_uninstall(id, location); });
    done = next(service, store::Update::Kind::job);
    assert(!done.ok && done.detail.find("running") != std::string::npos);
    assert(fs::exists(root / "apps" / id / "eboot.bin"));
    assert(service.job(view) && view.id.empty() && view.waiting.empty());
    service.stop();
    fs::remove_all(root);
}

int main()
{
    char path[] = "/tmp/prospero-service-XXXXXX";
    assert(mkdtemp(path));
    const std::string root = std::filesystem::canonical(path).string(); // macOS's /tmp is a symlink
    store::Service service(root, "01.000.000");
    assert(service.start());
    // The collection is built in: six games, Most Wanted the only one to install.
    const auto listed = next(service, store::Update::Kind::catalog);
    const auto &games = listed.snapshot.entries;
    assert(games.size() == 6 && !listed.snapshot.online && listed.snapshot.accepted);
    for (const auto &game : games)
        assert(game.status == (game.id == "PPSA99403" ? "available" : "coming_soon") &&
               game.icon.ends_with(".png"));
    assert(service.request_detail("PPSA99403"));
    const auto page = next(service, store::Update::Kind::detail);
    assert(page.entry.id == "PPSA99403" && page.entry.name == "Most Wanted");
    assert(service.request_detail("PPSA99001"));
    const auto unknown = next(service, store::Update::Kind::error);
    assert(unknown.entry.id == "PPSA99001" && !unknown.message.empty());
    const auto before = std::chrono::steady_clock::now();
    service.stop();
    assert(std::chrono::steady_clock::now() - before < 1s);
    // Closing right after the start must still persist next-launch settings.
    store::Service closing(root);
    assert(closing.start());
    const std::string settings = "catalog_url=https://dev.example/api/v1/\nverify_signatures=0\n";
    while (!closing.save_settings(settings))
        std::this_thread::sleep_for(1ms);
    closing.stop();
    std::string saved;
    assert(hui::save::read_file(root + "/settings.txt", &saved) && saved == settings);
    // A store without a folder of its own (not elevated) still keeps settings where it was
    // told it can write them.
    store::Service sandboxed("");
    sandboxed.settings_file = root + "/sandboxed-settings.txt";
    assert(sandboxed.start());
    while (!sandboxed.save_settings("debug=1\n"))
        std::this_thread::sleep_for(1ms);
    sandboxed.stop();
    saved.clear();
    assert(hui::save::read_file(root + "/sandboxed-settings.txt", &saved) && saved == "debug=1\n");
    std::filesystem::remove_all(root);
    check_installer();
}
