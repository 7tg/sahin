// Prospero BlackBox - Catalog cache and HTTPS never run on the render thread.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef STORE_DEBUG_TRACE
#include "diag/trace.hpp"
#endif
#include "app/ambient.hpp"
#include "system/title_registry.hpp"
#include "system/self_update_store.hpp"
#include "app/service.hpp"
#include "library/game_sources.hpp"
#include "library/prefix.hpp"
#include "library/tree.hpp"
#include "core/save_file.hpp"
#include "core/qr.hpp"
#include "platform/ps5/system.hpp"
#include "system/locations.hpp"
#include "system/running.hpp"
#include "system/storage_probe.hpp"
#ifdef STORE_FILE_WORKER
#include "system/worker_launch.hpp"
#endif
#include "install/files.hpp"
#include "catalog/icons.hpp"
#include "catalog/blackbox.hpp"
#include "third_party/yyjson/yyjson.h"
#include <chrono>
#include "system/app_folder.hpp"
#include "core/image.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <set>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store
{
namespace
{
// Built as one game's own app (BLACKBOX_GAME): its messages don't speak of the store.
#ifdef BLACKBOX_GAME
constexpr bool kOwnGame = true;
#else
constexpr bool kOwnGame = false;
#endif

// How long each part of a job took, in the log, with its bytes and speed when it moves
// bytes: "[STORE] time Copying the base copy: 41.2 s, 2950.1 MB, 71.6 MB/s".
class Timer
{
  public:
    void lap(const std::string &what, std::uint64_t bytes = 0)
    {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - start_).count();
        start_ = now;
        if (bytes == 0)
            hui::sys::log("[STORE] time %s: %.1f s", what.c_str(), seconds);
        else
            hui::sys::log("[STORE] time %s: %.1f s, %.1f MB, %.1f MB/s", what.c_str(), seconds,
                          static_cast<double>(bytes) / 1e6,
                          seconds > 0.0 ? static_cast<double>(bytes) / 1e6 / seconds : 0.0);
    }

  private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// The Home app the manager carries for a game: <bundle>/apps.json names its archive, size,
// SHA-256 and version (nfs-blackbox-ps5's tools/make-bundle.py writes it).
bool carried_app(const std::string &bundle, const catalog::Entry &game, catalog::Entry &app,
                 std::string &archive)
{
    std::string body;
    if (bundle.empty() || !install::read_small(bundle + "/apps.json", 64 * 1024, body))
        return false;
    yyjson_doc *doc = yyjson_read(body.data(), body.size(), 0);
    if (!doc)
        return false;
    yyjson_val *entry = yyjson_obj_get(yyjson_doc_get_root(doc), game.id.c_str());
    const auto text = [entry](const char *key)
    {
        const char *value = yyjson_get_str(yyjson_obj_get(entry, key));
        return std::string(value ? value : "");
    };
    const std::string file = text("file"), digest = text("sha256"), version = text("version");
    const std::uint64_t size = yyjson_get_uint(yyjson_obj_get(entry, "size"));
    yyjson_doc_free(doc);
    std::array<std::uint8_t, 32> bytes{};
    if (file.empty() || file.front() == '.' || file.find('/') != std::string::npos ||
        !catalog::hex_bytes(digest, bytes) || !catalog::version(version) || size == 0)
        return false;
    app = {};
    app.id = game.id;
    app.name = game.name;
    app.status = "available";
    app.format = "zip";
    app.digest = digest;
    app.size = size;
    app.content_version = version;
    app.version = "bundled-" + version;
    archive = bundle + "/" + file;
    app.artifact = archive;
    return true;
}

// The version of the app in <location>/<id>, when there is one.
bool folder_version(const std::string &location, const std::string &id, std::string &version)
{
    std::string body, error;
    catalog::Entry metadata;
    if (!install::read_small(location + "/" + id + "/sce_sys/param.json", catalog::kDetailLimit,
                             body) ||
        !catalog::parse_installed(body, metadata, error) || metadata.id != id)
        return false;
    version = metadata.content_version;
    return true;
}
} // namespace

Service::~Service()
{
    stop();
}
bool Service::start()
{
    if (started_)
        return true;
    started_ = pthread_create(&thread_, nullptr, entry, this) == 0;
    if (started_)
        icons_started_ = pthread_create(&icon_thread_, nullptr, icon_entry, this) == 0;
    if (started_ && !icons_started_)
        stop();
    if (started_ && installer)
    {
        const bool created = pthread_create(&install_thread_, nullptr, install_entry, this) == 0;
        std::lock_guard lock(mutex_);
        installer_started_ = created;
    }
    return started_;
}
void Service::stop()
{
    control_.cancel();
    icon_control_.cancel();
    bool joining = false;
    {
        // Under the lock, so a job starting now can't clear the cancellation.
        std::lock_guard lock(mutex_);
        job_control_.cancel();
        joining = installer_started_;
        installer_started_ = false;
    }
    if (started_)
        pthread_join(thread_, nullptr);
    if (icons_started_)
        pthread_join(icon_thread_, nullptr);
    if (joining)
        pthread_join(install_thread_, nullptr);
    control_.connection.reset();
    icon_control_.connection.reset();
    job_control_.connection.reset();
    icons_started_ = false;
    started_ = false;
    // A user may change the feed and close while the initial refresh is still retrying.
    if (settings_pending_ && !settings_path().empty())
    {
        const auto error = hui::save::write_atomic(settings_path(), settings_);
        if (!error.empty())
            hui::sys::log("[STORE] settings save failed: %s", error.c_str());
        settings_pending_ = false;
    }
}
bool Service::take(std::vector<Update> &updates)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || updates_.empty())
        return false;
    updates.swap(updates_);
    return true;
}
bool Service::request_detail(const std::string &id)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    detail_ = id;
    return true;
}
bool Service::request_icons(const std::vector<std::string> &ids)
{
    if (ids.size() > 16)
        return false;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    icons_ = ids;
    return true;
}
void Service::publish(Update update)
{
    // Backpressure stays on workers; never drop an image the screen is waiting for.
    while (!control_.cancelled.load())
    {
        {
            std::lock_guard lock(mutex_);
            if (updates_.size() < 8)
            {
                if (update.kind == Update::Kind::catalog)
                {
                    entries_ = update.snapshot.entries;
                    versions_ = update.snapshot.versions;
                    update.generation = ++generation_;
                    online_ = update.snapshot.online;
                }
                updates_.push_back(std::move(update));
                return;
            }
        }
        hui::sys::sleep_us(10000);
    }
}
void Service::report_frames(std::string report)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (lock.owns_lock())
        frame_report_ = std::move(report);
}
void *Service::entry(void *self)
{
    static_cast<Service *>(self)->run();
    return nullptr;
}

// What ShadowMountPlus scans on this console, from its own configuration.
// An existing configuration that can't be read is never replaced by defaults.
bool Service::load_policy(system::ScanPolicy &out, std::vector<std::string> *notes) const
{
    const auto note = [&](std::string text)
    {
        if (notes)
            notes->push_back(std::move(text));
    };
    if (installer_environment)
    {
        out = installer_environment->policy;
        return true;
    }
    if (root_ != "/data/nfs-blackbox")
        return false;
    std::string configuration, manual, policy_error;
    const bool configured =
        hui::save::read_file("/data/shadowmount/config.ini", &configuration, 256 * 1024);
    const bool listed = hui::save::read_file("/data/shadowmount/manual.lst", &manual, 256 * 1024);
    const auto unreadable = [](const char *path, bool read)
    {
        struct stat info
        {
        };
        return !read && (lstat(path, &info) == 0 || errno != ENOENT);
    };
    system::ScanPolicy policy;
    if (unreadable("/data/shadowmount/config.ini", configured) ||
        unreadable("/data/shadowmount/manual.lst", listed))
    {
        hui::sys::log("[STORE] storage policy error=Existing configuration is unreadable; "
                      "refusing default paths");
#ifdef STORE_DEBUG_TRACE
        diag::trace("ShadowMountPlus settings: config.ini or manual.lst exists but can't be read");
#endif
        note("config.ini or manual.lst in /data/shadowmount exists but can't be read, so "
             "installing is off. Check the file's permissions.");
        return false;
    }
    if (!system::scan_policy(configuration, manual, policy, policy_error))
    {
        hui::sys::log("[STORE] storage policy error=%s", policy_error.c_str());
#ifdef STORE_DEBUG_TRACE
        diag::trace("ShadowMountPlus settings refused: %s", policy_error.c_str());
#endif
        note("ShadowMountPlus's settings can't be used, so installing is off: " + policy_error +
             ".");
        return false;
    }
    hui::sys::log("[STORE] storage config=%d manual=%d roots=%zu entries=%zu depth=%u", configured,
                  listed, policy.roots.size(), policy.manual.size(), policy.depth);
#ifdef STORE_DEBUG_TRACE
    {
        // The scan paths outside removable drives, and the manual list: what a report needs.
        std::string roots, manual_entries;
        for (const auto &root : policy.roots)
            if (!root.starts_with("/mnt/") && roots.size() < 300)
                roots += (roots.empty() ? "" : ", ") + root;
        for (const auto &entry : policy.manual)
            if (manual_entries.size() < 300)
                manual_entries += (manual_entries.empty() ? "" : ", ") + entry;
        diag::trace("ShadowMountPlus settings: config.ini %s, manual.lst %s, scan depth %u, "
                    "%zu scan paths (not on drives: %s), %zu manual entries%s%s",
                    configured ? "found" : "none", listed ? "found" : "none", policy.depth,
                    policy.roots.size(), roots.empty() ? "none" : roots.c_str(),
                    policy.manual.size(), manual_entries.empty() ? "" : ": ",
                    manual_entries.c_str());
        for (const auto &line : policy.ignored)
            diag::trace("ShadowMountPlus settings: left out, not a path the store can read: %s",
                        line.c_str());
    }
#endif
    // What the store did about lines it can't use, and what the user may want to fix.
    for (const auto &line : policy.ignored)
        note("Left out, not a full path: " + line + ". Write it like /data/homebrew.");
    if (policy.fell_back)
        note("None of your scan paths could be read, so ShadowMountPlus's default folders "
             "are used (/data/homebrew and the homebrew folder on each drive).");
    for (const auto &entry : policy.manual)
    {
        // A manual entry is one game folder or one image. A folder of apps there does
        // nothing in ShadowMountPlus: it belongs in config.ini as scanpath=.
        struct stat info
        {
        };
        if (stat(entry.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
            stat((entry + "/sce_sys/param.json").c_str(), &info) != 0)
            note("manual.lst lists " + entry +
                 ", which is a folder and not one game. For a "
                 "folder of apps use scanpath= in config.ini.");
    }
    out = std::move(policy);
    return true;
}

bool Service::request_install(const catalog::Entry &entry, const std::string &location)
{
    return enqueue({Job::Kind::install, entry, location, {}, library::Overlay::off});
}

bool Service::request_adopt(const catalog::Entry &entry, const std::string &location)
{
    return enqueue({Job::Kind::adopt, entry, location, {}, library::Overlay::off});
}

bool Service::request_import(const catalog::Entry &entry)
{
    return enqueue({Job::Kind::import_base, entry, {}, {}, library::Overlay::off});
}

bool Service::request_game(const catalog::Entry &entry, bool remove, const std::string &location,
                           std::vector<std::string> options, library::Overlay overlay,
                           library::Cursor cursor)
{
    return enqueue({remove ? Job::Kind::uninstall_game : Job::Kind::install_game, entry, location,
                    std::move(options), overlay, cursor});
}

bool Service::request_game_settings(const catalog::Entry &entry, library::Overlay overlay,
                                    library::Cursor cursor)
{
    return enqueue({Job::Kind::game_settings, entry, {}, {}, overlay, cursor});
}

// An installed game's profile written again with its settings, and the plan's options its
// install chose (from the record), so nothing else in it changes.
bool Service::settings_job(const catalog::Entry &entry, library::Overlay overlay,
                           library::Cursor cursor, std::string &error)
{
    library::InstallPlan plan;
    const library::Manifest *manifest = nullptr;
    library::InstallOptions options;
    if (!game_plan(entry, plan, manifest, options, error))
        return false;
    options.overlay = overlay;
    options.cursor = cursor;
    const library::InstallPaths paths{root_, win_root};
    if (access(library::record_path(library::effective_plan(plan, options), paths).c_str(), F_OK) != 0)
        return true; // not installed: its next install writes the profile with them
    for (const auto &view : library::option_views(plan, paths, options))
        if (view.on)
            options.chosen.push_back(view.id);
    const auto chosen = library::effective_plan(plan, options);
    return library::install_profile(win_root + "/profiles", chosen.profile, chosen.profile_after, error) &&
           library::apply_ini(chosen, paths, error);
}

bool Service::game_plan(const catalog::Entry &entry, library::InstallPlan &plan,
                        const library::Manifest *&manifest, library::InstallOptions &options,
                        std::string &error) const
{
    manifest = nullptr;
    for (const auto &game : catalog::blackbox_games())
        if (entry.id == game.id && game.available)
            manifest = library::manifest_for(game.folder);
    if (!manifest || root_.empty() || plans.empty())
    {
        error = "This game can't be installed yet";
        return false;
    }
    if (!library::load_plan(plans + "/" + manifest->folder + ".json", plan, error))
        return false;
    options = {};
    options.now = std::time(nullptr);
    options.app_version = version_;
    // The profile keeps the plan's id whatever the prefix: the game's Home app starts
    // that id, so it plays whichever install the manager made last.
    if (!game_prefix.empty() && game_prefix != plan.prefix)
    {
        options.prefix = game_prefix;
        options.profile_name = plan.profile.name + " (" + game_prefix + ")";
    }
    return true;
}

// The game's Home app, from the copy the manager carries: installed when missing, updated
// when the carried one is newer, through the same transaction as any app. A build that
// carries none leaves it to the user.
bool Service::home_app(const catalog::Entry &game, const std::string &location,
                       const install::Environment &environment, std::string &error)
{
    catalog::Entry app;
    std::string archive, current;
    if (!carried_app(bundle, game, app, archive))
    {
        hui::sys::log("[STORE] home app id=%s: none carried", game.id.c_str());
        return true;
    }
    // Installed: replaced when the carried one is newer, or the same version built anew
    // (its bytes differ from what the receipt says was installed: a rebuild with a fix).
    // One installed by hand has no receipt and is left alone.
    bool same_version = false;
    if (folder_version(location, game.id, current) &&
        !catalog::update_available(current, app.content_version))
    {
        std::string body, why;
        catalog::Receipt receipt;
        if (current != app.content_version ||
            !install::read_small(environment.root + "/receipts/" + game.id + ".json", 64 * 1024,
                                 body) ||
            !catalog::parse_receipt(body, receipt, why) || receipt.id != game.id ||
            receipt.digest == app.digest)
            return true;
        same_version = true;
    }
    install::Request request{app, location, {}, false, archive};
    request.same_version = same_version;
    const auto result = install::apply(environment, request, job_control_, progress_);
    hui::sys::log("[STORE] home app id=%s operation=%s ok=%d version=%s error=%s %s",
                  game.id.c_str(), result.operation.c_str(), result.ok, result.version.c_str(),
                  result.error.c_str(), result.note.c_str());
    if (!result.ok)
        error = "The game is installed, but its Home app isn't: " + result.error;
    return result.ok;
}

// The game's Home app taken off the console, when the manager installed it. A game's own app
// (the launcher, running now) is that app: only the game goes; the app is the console's to
// delete.
bool Service::remove_home_app(const std::string &id, const std::string &location,
                              const install::Environment &environment, std::string &error)
{
    if (id == environment.self || location.empty() || access((environment.root + "/receipts/" + id + ".json").c_str(), F_OK) != 0)
        return true;
    const auto result = install::uninstall(environment, id, location, progress_);
    if (!result.ok)
    {
        error = "Its Home app can't be removed: " + result.error;
        return false;
    }
    const int unregistered = system::unregister_title(id);
    hui::sys::log("[STORE] home app id=%s removed, unregister rc=0x%08x", id.c_str(),
                  static_cast<unsigned>(unregistered));
    return true;
}

// The installer's worker: a collection game installed (or repaired) from its plan with its
// Home app, or both removed, with the job's progress.
bool Service::game_job(const catalog::Entry &entry, bool remove, const std::string &location,
                       const std::vector<std::string> &chosen, library::Overlay overlay,
                       library::Cursor cursor, const install::Environment &environment,
                       std::string &error)
{
    library::InstallPlan plan;
    const library::Manifest *manifest = nullptr;
    library::InstallOptions options;
    if (!game_plan(entry, plan, manifest, options, error))
        return false;
    options.chosen = chosen;
    options.overlay = overlay;
    options.cursor = cursor;
    const library::InstallPaths paths{root_, win_root};
    bases_stale_ = true;
    if (remove)
    {
        // The app first: while it can't go (running), the game stays playable.
        if (!remove_home_app(entry.id, location, environment, error))
            return false;
        progress_.phase = static_cast<int>(install::Phase::removing);
        return library::uninstall_game(plan, paths, options, error);
    }
    // What mods-cache lacks: from what the manager carries, else downloaded. The page lists
    // the stages (kGameStages) with what each is doing.
    const auto chosen_plan = library::effective_plan(plan, options);
    const auto sources = library::needed_sources(chosen_plan);
    const auto needed = sources.size();
    // The whole install as one fraction, for the page's ring: each part weighed by the bytes
    // it moves (the downloads, the bare prefix, the mods checked and then added, the base
    // copy copied); the last steps a small share. The downloads' bytes are known once the
    // cache has been looked through.
    enum Part { import_part, downloads, prefix_part, check_mods, copy_base, add_mods, last, parts };
    std::array<double, parts> weight{};
    for (const auto *source : sources)
        weight[add_mods] += static_cast<double>(source->size);
    weight[check_mods] = weight[add_mods];
    // The player's copy: in base-games, else on a USB drive, copied to base-games first as part
    // of this install (it is kept there for repairs; the USB copy is left as it is).
    const auto usable = [](const library::BaseGame &base)
    { return base.state == library::State::ready || base.state == library::State::needs_patch; };
    auto base = library::locate(root_, {}, *manifest);
    const bool from_usb = !usable(base);
    if (from_usb)
    {
        base = library::locate(root_, library::usb_roots(), *manifest);
        if (!base.on_usb || !usable(base))
        {
            error = base.state == library::State::absent ? "There's no copy of the game to install from"
                                                         : "Your copy can't be used: " + base.detail;
            return false;
        }
    }
    {
        std::vector<std::pair<std::string, std::uint64_t>> files;
        std::vector<std::string> dirs;
        std::string unused;
        if (library::walk(base.path, files, dirs, unused))
            for (const auto &file : files)
                weight[copy_base] += static_cast<double>(file.second);
        weight[import_part] = from_usb ? weight[copy_base] : 0.0;
    }
    int part = import_part;
    const auto overall = [&](int at, double fraction)
    {
        if (at < part)
            return;
        part = at;
        weight[last] = 0.0;
        double sum = 0.0, before = 0.0;
        for (int i = 0; i < parts; ++i)
            sum += weight[static_cast<std::size_t>(i)];
        weight[last] = sum * 0.03;
        sum += weight[last];
        for (int i = 0; i < part; ++i)
            before += weight[static_cast<std::size_t>(i)];
        const double now = weight[static_cast<std::size_t>(part)] * std::clamp(fraction, 0.0, 1.0);
        overall_ = sum > 0.0 ? static_cast<float>((before + now) / sum) : 0.0f;
    };
    overall(import_part, 0.0);
    Timer whole, part_time;
    if (from_usb)
    {
        stage(0, "Copying it from the USB drive to the console");
        progress_.phase = static_cast<int>(install::Phase::copying);
        const bool imported = library::import_base(
            base.path, root_, *manifest,
            [&](const library::ImportProgress &step)
            {
                progress_.total = step.total;
                progress_.done = step.done;
                overall(import_part, step.total ? static_cast<double>(step.done) /
                                                      static_cast<double>(step.total)
                                                : 0.0);
            },
            job_control_.cancelled, error);
        bases_stale_ = true;
        part_time.lap(std::string("importing the base copy from USB") + (imported ? "" : " (failed)"),
                      progress_.total.load());
        if (!imported)
            return false;
        stage(0, "Copied to the console; the USB drive can go");
    }
    else
        stage(0, "On the console");
    overall(downloads, 0.0);
    progress_.done = 0;
    progress_.total = 0;
    stage(1, "Looking for the " + std::to_string(needed) + " files it needs");
    progress_.phase = static_cast<int>(install::Phase::downloading);
    // fetch_sources runs downloads on threads of its own: each worker keeps its connection
    // for its pieces here, released when this job is done. The job's cancel reaches them
    // through the sink, which fetch_sources stops on cancel.
    std::array<net::Control, library::kParallelDownloads> controls;
    const library::Download download = [&controls](std::size_t worker, const std::string &url,
                                                   std::uint64_t offset, std::uint64_t length,
                                                   bool ranged,
                                                   const std::function<bool(std::string_view)> &sink,
                                                   std::string &why)
    {
        auto &control = controls[worker];
        control.range = ranged ? std::to_string(offset) + "-" + std::to_string(offset + length - 1)
                               : std::string();
        const auto response = net::get(url, net::Purpose::source, length, sink, control);
        control.range.clear();
        if (response.ok() && response.status == (ranged ? 206 : 200))
            return true;
        why = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error;
        return false;
    };
    std::size_t started = 0;
    std::vector<std::string> shown;
    // Only the chosen options' sources: an install without them downloads none of theirs.
    if (!library::fetch_sources(
            chosen_plan, root_ + "/mods-cache", bundle, download,
            [&](const library::FetchStatus &status)
            {
                progress_.total = status.total;
                progress_.done = status.done;
                weight[downloads] = static_cast<double>(status.all_total);
                overall(downloads, status.all_total ? static_cast<double>(status.all_done) /
                                                          static_cast<double>(status.all_total)
                                                    : 1.0);
                if (started == 0 && status.index > 0)
                    part_time.lap("looking through mods-cache");
                started = status.index;
                if (!status.piece.empty())
                    hui::sys::log("[STORE] piece %s", status.piece.c_str());
                if (!status.finished.empty())
                    hui::sys::log("[STORE] time fetching %s: %.1f s, %.1f MB, %.1f MB/s",
                                  status.finished.c_str(), status.finished_seconds,
                                  static_cast<double>(status.finished_bytes) / 1e6,
                                  status.finished_seconds > 0.0
                                      ? static_cast<double>(status.finished_bytes) / 1e6 /
                                            status.finished_seconds
                                      : 0.0);
                if (status.names != shown && !status.names.empty())
                {
                    // "Getting Xbox 360 Stuff Pack 4.2 (3 of 14)", or with several at once
                    // "Getting 3 files at once: Xbox 360 Stuff Pack 4.2, ... (12 of 14)".
                    shown = status.names;
                    std::string what = shown.front();
                    if (shown.size() > 1)
                    {
                        what = std::to_string(shown.size()) + " files at once: ";
                        for (std::size_t i = 0; i < shown.size(); ++i)
                            what += (i ? ", " : "") + shown[i];
                    }
                    stage(1, "Getting " + what + " (" + std::to_string(status.index) + " of " +
                                 std::to_string(status.count) + ")");
                }
            },
            job_control_.cancelled, error))
        return false;
    part_time.lap(started == 0 ? "looking through mods-cache (nothing to fetch)"
                               : "all the files fetched",
                  static_cast<std::uint64_t>(weight[downloads]));
    // Every one of them was hashed whole just now (looking through the cache, or as it
    // arrived): install_game needn't again.
    for (const auto *source : sources)
        options.verified.insert(source->sha256);
    stage(1, "All " + std::to_string(needed) + " files ready");
    // The prefix, made from the bare prefix the manager carries when it isn't there yet.
    const auto prefix = chosen_plan.prefix;
    struct stat bare{};
    if (!library::prefix_ready(win_root, prefix) &&
        stat((bundle + "/bare-prefix-nfs-blackbox.zip").c_str(), &bare) == 0)
        weight[prefix_part] = static_cast<double>(bare.st_size);
    overall(prefix_part, 0.0);
    if (library::prefix_ready(win_root, prefix))
        stage(2, "Using the prefix " + prefix);
    else
    {
        stage(2, "Making the prefix " + prefix);
        progress_.phase = static_cast<int>(install::Phase::unpacking);
        if (!library::ensure_prefix(
                win_root, prefix, bundle + "/bare-prefix-nfs-blackbox.zip",
                [&](std::uint64_t done, std::uint64_t total)
                {
                    progress_.total = total;
                    progress_.done = done;
                    overall(prefix_part, total ? static_cast<double>(done) / static_cast<double>(total)
                                               : 0.0);
                },
                job_control_.cancelled, error))
            return false;
        part_time.lap("making the prefix " + prefix,
                      static_cast<std::uint64_t>(weight[prefix_part]));
        stage(2, "Made the prefix " + prefix);
    }
    std::string timed_step;
    std::uint64_t timed_bytes = 0;
    const auto step_done = [&]
    {
        if (!timed_step.empty())
            part_time.lap(timed_step, timed_bytes);
    };
    const bool installed = library::install_game(
            plan, *manifest, paths, options,
            [&](const library::InstallStatus &status)
            {
                // The steps counted in bytes are timed with them; the others are counts.
                if (status.step != timed_step)
                {
                    step_done();
                    timed_step = status.step;
                }
                timed_bytes = status.step == "Checking the mod files" ||
                                      status.step == "Copying the base copy" ||
                                      status.step == "Adding the mods"
                                  ? status.total
                                  : 0;
                using install::Phase;
                // The page shows phases: the steps that move bytes show how far they are.
                const auto phase = status.step.starts_with("Checking") ? Phase::verifying
                                   : status.step == "Copying the base copy" ||
                                           status.step == "Adding the mods"
                                       ? Phase::copying
                                       : Phase::activating;
                progress_.phase = static_cast<int>(phase);
                progress_.total = status.total;
                progress_.done = status.done;
                const double fraction = status.total ? static_cast<double>(status.done) /
                                                           static_cast<double>(status.total)
                                                     : 0.0;
                overall(status.step == "Checking the base copy"   ? check_mods
                        : status.step == "Checking the mod files" ? check_mods
                        : status.step == "Copying the base copy"  ? copy_base
                        : status.step.starts_with("Upgrading") ||
                                status.step == "Adding the mods"
                            ? add_mods
                            : last,
                        status.step == "Checking the base copy" || status.step.starts_with("Upgrading")
                            ? 0.0
                            : fraction);
                // Its steps, in the page's words, under the stage each belongs to; a stage
                // left behind says what it did.
                const auto &step = status.step;
                const int at = step.starts_with("Checking the") && step != "Checking the result" ? 3
                               : step == "Setting up Wine" || step == "Installed"              ? 5
                                                                                               : 4;
                const char *done[] = {"", "", "", "Your copy and the mod files check out",
                                      "The game's files are in place"};
                for (int before = 3; before < at; ++before)
                    stage(before, done[before]);
                stage(at, step == "Checking the base copy"   ? "Checking your copy of the game"
                          : step == "Copying the base copy"  ? "Copying your game into the prefix"
                          : step == "Settings"               ? "Writing the mods' settings"
                          : step == "Checking the result"    ? "Checking every file it changed"
                          : step == "Setting up Wine"        ? "Wine's DLLs, registry and profile"
                          : step == "Installed"              ? "Registry and profile set"
                                                             : step);
            },
            job_control_.cancelled, error);
    step_done();
    if (!installed)
        return false;
    if (entry.id == environment.self)
    {
        whole.lap("the whole install");
        overall_ = 1.0f;
        stage(6, "Ready to play"); // a game's own app: it is the Home app
        return true;
    }
    stage(6, "Putting its app on the Home screen");
    if (!home_app(entry, location, environment, error))
        return false;
    part_time.lap("its Home app");
    whole.lap("the whole install");
    overall_ = 1.0f;
    stage(6, "On the Home screen");
    return true;
}

// Where each game's base copy is and what it is: published when it changes (a copy put
// in place over FTP, a USB drive plugged in or out).
void Service::check_bases()
{
    if (root_.empty())
        return;
    const auto roots = library::usb_roots();
    for (const auto &game : catalog::blackbox_games())
    {
        const auto *manifest = library::manifest_for(game.folder);
        if (!manifest || !game.available)
            continue;
        auto found = library::locate(root_, roots, *manifest);
        bool installed = false;
        std::vector<library::OptionView> views;
        {
            library::InstallPlan plan;
            const library::Manifest *unused = nullptr;
            library::InstallOptions options;
            std::string error;
            catalog::Entry entry;
            entry.id = game.id;
            installed = game_plan(entry, plan, unused, options, error) &&
                        access(library::record_path(library::effective_plan(plan, options),
                                                    {root_, win_root})
                                   .c_str(),
                               F_OK) == 0;
            if (!plan.game.empty())
                views = library::option_views(plan, {root_, win_root}, options);
        }
        std::vector<std::string> on;
        for (const auto &view : views)
            if (view.on)
                on.push_back(view.id);
        const auto known = bases_.find(game.id);
        if (known != bases_.end() && known->second.state == found.state &&
            known->second.path == found.path && known->second.detail == found.detail &&
            installed_[game.id] == installed && options_on_[game.id] == on)
            continue;
        installed_[game.id] = installed;
        options_on_[game.id] = on;
        hui::sys::log("[STORE] base %s state=%d usb=%d path=%s version=%s", game.id,
                      static_cast<int>(found.state), found.on_usb, found.path.c_str(),
                      found.version.c_str());
        bases_[game.id] = found;
        Update update;
        update.kind = Update::Kind::base;
        update.entry.id = game.id;
        update.base = std::move(found);
        update.game_installed = installed;
        update.game_options = std::move(views);
        publish(std::move(update));
    }
}

// The installer's worker: the USB copy into base-games, with the job's progress.
bool Service::import_job(const catalog::Entry &entry, std::string &error)
{
    const library::Manifest *manifest = nullptr;
    for (const auto &game : catalog::blackbox_games())
        if (entry.id == game.id)
            manifest = library::manifest_for(game.folder);
    if (!manifest || root_.empty())
    {
        error = "This game has no base copy to import";
        return false;
    }
    const auto found = library::locate(root_, library::usb_roots(), *manifest);
    if (!found.on_usb ||
        (found.state != library::State::ready && found.state != library::State::needs_patch))
    {
        error = found.on_usb ? found.detail : "No copy on a USB drive to import";
        return false;
    }
    progress_.phase = static_cast<int>(install::Phase::copying);
    Timer timer;
    const bool ok = library::import_base(
        found.path, root_, *manifest,
        [&](const library::ImportProgress &step)
        {
            progress_.total = step.total;
            progress_.done = step.done;
        },
        job_control_.cancelled, error);
    timer.lap(std::string("importing the base copy from USB") + (ok ? "" : " (failed)"),
              progress_.total.load());
    bases_stale_ = true;
    return ok;
}

bool Service::enqueue(Job job)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !installer_started_ || jobs_.size() >= 16)
        return false;
    const auto same = [&](const Job &other) { return other.entry.id == job.entry.id; };
    if (job_id_ != job.entry.id && std::none_of(jobs_.begin(), jobs_.end(), same))
        jobs_.push_back(std::move(job));
    return true;
}

std::string Service::settings_path() const
{
    if (!settings_file.empty())
        return settings_file;
    return root_.empty() ? std::string() : root_ + "/settings.txt";
}

bool Service::save_settings(std::string text)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    settings_ = std::move(text);
    settings_pending_ = true;
    return true;
}

bool Service::request_uninstall(const std::string &id, const std::string &location)
{
    catalog::Entry entry;
    entry.id = id;
    std::string name;
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return false;
        for (const auto &listed : entries_)
            if (listed.id == id)
                entry.name = listed.name;
    }
    return enqueue({Job::Kind::uninstall, std::move(entry), location, {}, library::Overlay::off});
}

bool Service::cancel_job(const std::string &id)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    std::erase_if(jobs_, [&](const Job &job) { return job.entry.id == id; });
    if (job_id_ == id)
        job_control_.cancelled = true; // The transfer and the unpacking both watch it.
    return true;
}

void Service::stop_at(std::string step)
{
    std::lock_guard lock(stop_guard_);
    stop_step_ = std::move(step);
}

void Service::stage(int index, std::string line)
{
    std::lock_guard lock(stage_mutex_);
    stages_.resize(std::size(library::kGameStages));
    stage_ = index;
    stages_[static_cast<std::size_t>(index)] = std::move(line);
}

void Service::clear_stages()
{
    std::lock_guard lock(stage_mutex_);
    stage_ = -1;
    stages_.clear();
}

bool Service::job(JobView &view)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    {
        std::lock_guard stages(stage_mutex_);
        view.stage = stage_;
        view.stages = stages_;
    }
    view.id = job_id_;
    view.phase = static_cast<install::Phase>(progress_.phase.load());
    view.done = progress_.done.load();
    view.total = progress_.total.load();
    view.overall = overall_.load();
    view.waiting.clear();
    for (const auto &job : jobs_)
        view.waiting.push_back(job.entry.id);
    return true;
}

bool Service::running(std::vector<std::string> &ids, bool &known)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    ids = running_;
    known = running_known_;
    return true;
}

void *Service::install_entry(void *self)
{
    static_cast<Service *>(self)->run_installer();
    return nullptr;
}

void Service::run_installer()
{
    install::Environment environment;
    if (installer_environment)
        environment = *installer_environment;
    else
    {
        environment.root = root_;
        environment.self = system::own_title();
        environment.self_version = version_;
        environment.fetch = [](const std::string &url, std::uint64_t limit, const net::Sink &sink,
                               net::Control &control)
        {
#ifdef STORE_DEVELOPMENT
            // A scripted self-update reads its archive from the console, since
            // no newer store is published while it is being tested.
            if (url.ends_with("/dev/PPSA99400.zip"))
            {
                // In pieces, as a download arrives: never the whole archive in memory.
                net::Response response;
                const int file = open("/data/nfs-blackbox/dev/self.zip", O_RDONLY);
                std::vector<char> piece(256 * 1024);
                std::uint64_t total = 0;
                bool ok = file >= 0;
                while (ok)
                {
                    const auto count = read(file, piece.data(), piece.size());
                    if (count <= 0)
                    {
                        ok = count == 0;
                        break;
                    }
                    total += static_cast<std::uint64_t>(count);
                    ok = total <= limit && !control.cancelled.load() &&
                         sink(std::string_view(piece.data(), static_cast<std::size_t>(count)));
                }
                if (file >= 0)
                    close(file);
                if (ok)
                    response.status = 200;
                else
                    response.error = "The development archive could not be read";
                return response;
            }
#endif
            return net::get(url, net::Purpose::artifact, limit, sink, control);
        };
        environment.running = [](const std::string &id) { return system::title_running(id); };
#ifdef STORE_FILE_WORKER
        environment.unpack = [](const std::string &archive, const std::string &title,
                                const std::string &destination, const std::atomic<bool> &cancelled,
                                std::atomic<std::uint64_t> &written, std::string &error,
                                install::ExtractTimes &times)
        {
            return install::worker_extract(system::launch_worker, archive, title, destination,
                                           cancelled, written, error, times);
        };
        environment.remove = [](const std::string &path)
        { return install::worker_remove(system::launch_worker, path); };
        environment.save = [](const std::string &path, install::Writer &writer)
        { return install::worker_store(system::launch_worker, path, writer); };
#endif
#ifdef STORE_DEVELOPMENT
        environment.interrupt = [this](const char *step)
        {
            std::lock_guard lock(stop_guard_);
            if (stop_step_ != step)
                return false;
            hui::sys::log("[STORE] stopping dead at step %s", step);
            std::fflush(stdout);
            _exit(0);
        };
#endif
    }
    std::string broken;
    if (environment.root.empty() || !hui::save::ensure_directory(environment.root) ||
        (!installer_environment && !load_policy(environment.policy)))
        broken = "Installing is unavailable: ShadowMountPlus's settings couldn't be read. "
                 "About says what to check.";
    const auto rescan = [&]
    {
        Update installed;
        installed.kind = Update::Kind::inventory;
        installed.installed = system::scan_installed(
            environment.policy, environment.root + "/receipts", control_.cancelled);
        publish(std::move(installed));
    };
    if (broken.empty())
    {
        // Where apps can go: the folders ShadowMountPlus scans that exist on a
        // drive the store can work on, with the room each has.
        Update places;
        places.kind = Update::Kind::locations;
        for (const auto &path : environment.policy.roots)
        {
            std::uint64_t available = 0;
            if (path.starts_with("/mnt/shadowmnt") ||
                (!installer_environment && system::drive_root(path).empty()) ||
                install::kind(path) != install::Kind::directory ||
                !system::available_space(path, available) || available < (256ull << 20))
                continue;
            places.locations.emplace_back(path, available);
        }
        publish(std::move(places));
        // Before anything else: finish or undo what was interrupted last time.
        const auto recovered = install::recover(environment);
        hui::sys::log("[STORE] recovery ok=%d operation=%s error=%s", recovered.ok,
                      recovered.operation.c_str(), recovered.error.c_str());
        if (!recovered.ok)
            broken = recovered.error;
        else if (recovered.restart)
        {
            // The console started the old store once more after its update.
            broken = "Prospero BlackBox is finishing its own update. Close it and open it again.";
            Update notice;
            notice.kind = Update::Kind::job;
            notice.entry.id = environment.self;
            notice.ok = true;
            notice.restart = true;
            notice.message = "Prospero BlackBox was updated";
            notice.detail = "The console is still running the previous version. Close "
                            "Prospero BlackBox, restart the console and open it again.";
            publish(std::move(notice));
        }
        else if (!recovered.operation.empty())
        {
            Update notice;
            notice.kind = Update::Kind::notice;
            notice.message = kOwnGame ? "Recovered" : "Prospero BlackBox recovered";
            notice.detail = "An interrupted " + recovered.operation + " was cleaned up.";
            publish(std::move(notice));
            rescan();
        }
    }
    unsigned idle = 0;
    const auto entries_snapshot = [&]
    {
        std::vector<std::string> ids;
        std::lock_guard lock(mutex_);
        for (const auto &entry : entries_)
            ids.push_back(entry.id);
        return ids;
    };
    while (!control_.cancelled.load())
    {
        Job job;
        std::string minimum;
        {
            std::lock_guard lock(mutex_);
            if (!jobs_.empty())
            {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                job_id_ = job.entry.id;
                const auto listed = versions_.find(job_id_);
                minimum = listed == versions_.end() ? std::string{} : listed->second;
                // A cancel belongs to the job it was asked for, never to the next one.
                job_control_.cancelled = control_.cancelled.load();
                progress_.phase = static_cast<int>(install::Phase::idle);
                progress_.done = 0;
                progress_.total = 0;
                overall_ = -1.0f;
                clear_stages();
            }
        }
        if (job.entry.id.empty())
        {
            // Idle: keep the list of running titles fresh for the screens.
            if (++idle % 40 == 1)
            {
                std::vector<std::string> ids;
                const bool known = installer_environment ? true : system::running_titles(ids);
                if (installer_environment && installer_environment->running)
                    for (const auto &entry : entries_snapshot())
                        if (installer_environment->running(entry) == 1)
                            ids.push_back(entry);
                std::lock_guard lock(mutex_);
#ifdef STORE_DEVELOPMENT
                if (ids != running_ || known != running_known_)
                {
                    std::string list;
                    for (const auto &id : ids)
                        list += (list.empty() ? "" : ",") + id;
                    hui::sys::log("[STORE] running known=%d titles=%s", known, list.c_str());
                }
#endif
                running_ = std::move(ids);
                running_known_ = known;
            }
            hui::sys::sleep_us(50000);
            continue;
        }
        install::Result result;
        if (!broken.empty())
            result.error = broken;
        else if (job.kind == Job::Kind::uninstall)
            result = install::uninstall(environment, job.entry.id, job.location, progress_);
        else if (job.kind == Job::Kind::adopt)
            result = install::adopt(environment, job.entry.id, job.location);
        else if (job.kind == Job::Kind::import_base)
        {
            result.operation = "import";
            result.ok = import_job(job.entry, result.error);
        }
        else if (job.kind == Job::Kind::install_game || job.kind == Job::Kind::uninstall_game)
        {
            const bool remove = job.kind == Job::Kind::uninstall_game;
            result.operation = remove ? "game-uninstall" : "game-install";
            result.ok = game_job(job.entry, remove, job.location, job.options, job.overlay,
                                 job.cursor, environment, result.error);
        }
        else if (job.kind == Job::Kind::game_settings)
        {
            result.operation = "game-settings";
            result.ok = settings_job(job.entry, job.overlay, job.cursor, result.error);
        }
#ifdef STORE_FILE_WORKER
        else if (job.entry.id == environment.self)
        {
            // The store itself: the kit's helper swaps the files of its folder
            // once it has closed, so the next start runs the new version.
            result.operation = "self-update";
            result.ok = system::update_self(job.entry, environment.self_version, progress_,
                                            job_control_, result.error);
            result.version = job.entry.content_version;
        }
#endif
        else
            result = install::apply(
                environment, {job.entry, job.location, minimum, job.entry.id == environment.self, {}},
                job_control_, progress_);
        hui::sys::log("[STORE] job id=%s operation=%s ok=%d version=%s error=%s %s",
                      job.entry.id.c_str(), result.operation.c_str(), result.ok,
                      result.version.c_str(), result.error.c_str(), result.note.c_str());
        // An uninstalled app leaves the home screen too. Its own files under
        // /data are not the console's to remove and stay where they are.
        int unregistered = 1;
        if (result.ok && result.operation == "uninstall")
        {
            unregistered = system::unregister_title(job.entry.id);
            hui::sys::log("[STORE] unregister id=%s rc=0x%08x", job.entry.id.c_str(),
                          static_cast<unsigned>(unregistered));
        }
        Update done;
        done.kind = Update::Kind::job;
        done.entry.id = job.entry.id;
        done.ok = result.ok;
        const auto &name = job.entry.name.empty() ? job.entry.id : job.entry.name;
        if (result.ok && result.operation == "self-update")
        {
            done.close = true;
            done.message = "Updating Prospero BlackBox";
            done.detail = "It closes now. A notification says when the new version is in "
                          "place; then open it again.";
        }
        else if (result.ok)
        {
            done.restart = result.restart;
            done.message = name + (result.operation == "uninstall" ? " uninstalled"
                                   : result.operation == "import"  ? " copied to the console"
                                   : result.operation == "game-uninstall" ? " uninstalled"
                                   : result.operation == "game-settings" ? ": settings saved"
                                   : result.operation == "adopt"   ? " is now managed"
                                   : result.operation == "update"  ? " updated"
                                                                   : " installed");
            done.detail =
                result.restart ? "Close Prospero BlackBox and open it again to finish. If the "
                                 "old version opens, restart the console first."
                : result.operation == "uninstall"
                    ? (unregistered == 0 ? "It is off the home screen. Its saved data stays."
                                         : "Its saved data stays. Its home-screen tile goes "
                                           "after a restart.")
                : result.operation == "import" ? "Kept in base-games for installs. The USB drive can go."
                : result.operation == "game-install" && job.entry.id == system::own_title()
                    ? "Press Play. Its settings are in the game: Options, PS5 Options."
                : result.operation == "game-install"
                    ? "Start it from its own app on the Home screen, or from prospero-win."
                : result.operation == "game-settings" ? "They apply when the game starts."
                : result.operation == "game-uninstall"
                    ? (kOwnGame ? "Your saves and your copy of the game stay. Install puts it back."
                                : "Its saves stay in the prefix, and so does the base copy.")
                : result.operation == "adopt" ? "Prospero BlackBox will offer its updates from now on."
                : result.operation == "update"
                    ? "The new version is in place. Give the console a few minutes "
                      "before starting it."
                    : "ShadowMountPlus will add it to your home screen in a moment.";
            if (result.operation == "update" && !result.kept_at.empty())
                done.detail = "Previous folder kept in " + result.kept_at + ".";
        }
        else
        {
            done.message = name + (result.error == "Cancelled" ? ": cancelled" : ": not changed");
            done.detail = result.error == "Cancelled" ? "Nothing was installed." : result.error;
        }
        if (broken.empty())
            rescan();
        {
            std::lock_guard lock(mutex_);
            job_id_.clear();
            clear_stages();
            overall_ = -1.0f;
            progress_.phase = static_cast<int>(install::Phase::idle);
        }
        publish(std::move(done));
    }
}
void Service::run()
{
    if (!root_.empty() && !hui::save::ensure_directory(root_))
    {
        Update failure;
        failure.message = kOwnGame ? "Storage is unavailable: " + root_ + " can't be made."
                                   : "Storage is unavailable. The catalog could not be loaded.";
        publish(std::move(failure));
        return;
    }
    system::ScanPolicy scan_policy;
    std::vector<std::string> setup_notes;
    const bool can_scan = load_policy(scan_policy, &setup_notes);
    if (!setup_notes.empty())
    {
        Update setup;
        setup.kind = Update::Kind::setup;
        for (const auto &line : setup_notes)
            setup.detail += line + "\n";
        publish(std::move(setup));
    }
#ifdef STORE_DEVELOPMENT
    for (const auto &path : scan_policy.roots)
    {
        if (!can_scan || control_.cancelled.load())
            break;
        const auto drive = system::drive_root(path);
        if (drive.empty())
            continue;
        const auto probe = system::probe_storage(path);
        const bool work_safe =
            system::work_path_unscanned(scan_policy, drive + "/nfs-blackbox/staging/PPSA99400");
        hui::sys::log(
            "[STORE] storage path=%s fs=%s available=%llu rename=%d work-safe=%d error=%s",
            path.c_str(), probe.filesystem.c_str(),
            static_cast<unsigned long long>(probe.available), probe.renamed, work_safe,
            probe.error.c_str());
    }
#endif
    // The catalog is built in: the Black Box games, nothing downloaded or verified.
    if (!control_.cancelled.load())
    {
        Update result;
        result.kind = Update::Kind::catalog;
        result.snapshot = catalog::blackbox_snapshot(system::app_folder());
        result.message = "Black Box collection";
        hui::sys::log("[STORE] catalog built-in games=%zu", result.snapshot.entries.size());
        publish(std::move(result));
    }
    if (!control_.cancelled.load())
    {
        Update installed;
        installed.kind = Update::Kind::inventory;
        if (can_scan)
            installed.installed =
                system::scan_installed(scan_policy, root_ + "/receipts", control_.cancelled);
        else
        {
            installed.installed.complete = false;
            installed.installed.errors.push_back("Installed apps could not be inspected with the "
                                                 "current permissions or scan configuration.");
        }
        hui::sys::log("[STORE] inventory complete=%d apps=%zu errors=%zu",
                      installed.installed.complete, installed.installed.apps.size(),
                      installed.installed.errors.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(3, installed.installed.errors.size());
             ++i)
            hui::sys::log("[STORE] inventory error=%s", installed.installed.errors[i].c_str());
        publish(std::move(installed));
    }
    auto bases_checked = std::chrono::steady_clock::time_point{};
    while (!control_.cancelled.load())
    {
        std::string id;
        std::string frame_report;
        {
            std::lock_guard lock(mutex_);
            id.swap(detail_);
            frame_report.swap(frame_report_);
        }
        if (!frame_report.empty())
            hui::sys::log("[STORE] %s", frame_report.c_str());
        std::string settings;
        bool save = false;
        {
            std::lock_guard lock(mutex_);
            if (settings_pending_)
            {
                settings.swap(settings_);
                settings_pending_ = false;
                save = true;
            }
        }
        if (save)
        {
            const auto error =
                settings_path().empty()
                    ? "This console gave the app nowhere to keep them. They last until you "
                      "close it."
                    : hui::save::write_atomic(settings_path(), settings);
            if (!error.empty())
            {
                Update notice;
                notice.kind = Update::Kind::notice;
                notice.message = "Settings could not be saved";
                notice.detail = error;
                publish(std::move(notice));
            }
        }
        if (!id.empty())
        {
            // The built-in catalog already holds everything a game's page shows.
            Update result;
            result.entry.id = id;
            result.message = "Not part of the collection";
            for (auto &entry : catalog::blackbox_snapshot(system::app_folder()).entries)
                if (entry.id == id)
                {
                    result.entry = std::move(entry);
                    result.kind = Update::Kind::detail;
                    break;
                }
            publish(std::move(result));
        }
        if (const auto now = std::chrono::steady_clock::now();
            bases_stale_.exchange(false) || now - bases_checked >= std::chrono::seconds(5))
        {
            bases_checked = now;
            check_bases();
        }
        hui::sys::sleep_us(100000);
    }
}

void *Service::icon_entry(void *self)
{
    static_cast<Service *>(self)->load_icons();
    return nullptr;
}

void Service::load_icons()
{
    catalog::Icons artwork(root_.empty() ? "" : root_ + "/cache/icons");
    std::set<std::string> failed_icons;
    std::uint64_t failure_generation = 0;
    // A picture just delivered is asked for again until the frame loop has
    // uploaded it: it is not read and decoded a second time for that.
    std::map<std::string, std::int64_t> delivered;
    while (!icon_control_.cancelled.load())
    {
        catalog::Entry entry;
        std::uint64_t generation = 0;
        bool online = false;
        {
            std::lock_guard lock(mutex_);
            generation = generation_;
            if (!icons_.empty())
            {
                const auto found =
                    std::find_if(entries_.begin(), entries_.end(),
                                 [&](const auto &item) { return item.id == icons_.front(); });
                if (found != entries_.end())
                    entry = *found;
                icons_.erase(icons_.begin());
                online = online_;
            }
        }
        if (generation != failure_generation)
        {
            failed_icons.clear();
            failure_generation = generation;
        }
        if (catalog::local_icon(entry.icon))
        {
            const auto moment = hui::sys::monotonic_us();
            const auto stamp = entry.id + "#" + std::to_string(generation);
            if (const auto last = delivered.find(stamp);
                last != delivered.end() && moment - last->second < 1500000)
                continue;
            Update result;
            result.kind = Update::Kind::icon;
            result.entry.id = entry.id;
            result.generation = generation;
            std::string encoded;
            if (!failed_icons.contains(entry.id) &&
                hui::save::read_file(entry.icon, &encoded, 2u << 20) &&
                hui::decode_png(encoded, result.image))
            {
                std::uint32_t vivid = 0;
                result.ambient = make_ambient(result.image, vivid);
                result.accent = vivid ? vivid : average_colour(result.image);
                delivered[stamp] = moment;
                publish(std::move(result));
            }
            else
                failed_icons.insert(entry.id);
            continue;
        }
        const auto key = catalog::Icons::key(entry);
        const auto moment = hui::sys::monotonic_us();
        const auto stamp = key + "#" + std::to_string(generation);
        if (const auto last = delivered.find(stamp);
            last != delivered.end() && moment - last->second < 1500000)
            continue;
        if (delivered.size() > 4096)
            delivered.clear();
        if (!key.empty() && !failed_icons.contains(key))
        {
            Update result;
            result.kind = Update::Kind::icon;
            result.entry.id = entry.id;
            result.generation = generation;
#ifdef STORE_DEVELOPMENT
            const auto started = hui::sys::monotonic_us();
#endif
            bool loaded = artwork.cached(entry, result.image);
#ifdef STORE_DEVELOPMENT
            const bool cached = loaded;
#endif
            if (!loaded && online)
            {
                std::string encoded;
                const auto response =
                    net::fetch(entry.icon, net::Purpose::catalog, 2u << 20, encoded, icon_control_);
                loaded = response.ok() && artwork.store(entry, encoded, result.image);
            }
#ifdef STORE_DEVELOPMENT
            hui::sys::log(
                "[STORE] icon id=%s cached=%d loaded=%d elapsed_ms=%llu", entry.id.c_str(), cached,
                loaded,
                static_cast<unsigned long long>((hui::sys::monotonic_us() - started) / 1000));
#endif
            if (loaded)
            {
                // The app's picture and colour come from its icon, here on the worker.
                std::uint32_t vivid = 0;
                result.ambient = make_ambient(result.image, vivid);
                result.accent = vivid ? vivid : average_colour(result.image);
                delivered[stamp] = moment;
                publish(std::move(result));
            }
            else if (online)
                failed_icons.insert(key);
        }
        if (entry.id.empty())
            hui::sys::sleep_us(10000);
    }
}
} // namespace store
