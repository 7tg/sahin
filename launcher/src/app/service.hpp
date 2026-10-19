// Prospero BlackBox - Background catalog work and nonblocking frame delivery.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalog/catalog.hpp"
#include "library/base_game.hpp"
#include "library/game_install.hpp"
#include "core/image.hpp"
#include "install/transaction.hpp"
#include "system/inventory.hpp"
#include <deque>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <vector>

namespace store
{
struct Update
{
    enum class Kind
    {
        catalog,
        detail,
        icon,
        qr,
        inventory,
        notice,       // message is its title, detail its body
        setup,        // what to check in ShadowMountPlus's settings: detail, one per line
        news,         // the catalog's news feed, newest first
        job,          // a finished install, update or uninstall: entry.id, ok, message, detail
        locations,    // where apps can be installed on this console
        base,         // a game's base copy: entry.id, base
        error
    } kind = Kind::error;
    std::string detail;
    catalog::Snapshot snapshot;
    std::vector<catalog::News> news;
    catalog::Entry entry;
    hui::Image image;
    hui::Image ambient;       // icon: the app's picture, made from it (make_ambient)
    std::uint32_t accent = 0; // icon: the colour the screen leans toward, 0xRRGGBB
    system::Inventory installed;
    std::uint64_t generation = 0;
    std::string message;
    bool ok = false;
    bool restart = false; // the store updated itself: restart to finish
    bool close = false;   // the update helper has the go-ahead: the store must close now
    std::vector<std::pair<std::string, std::uint64_t>> locations; // path, free bytes
    library::BaseGame base;
    bool game_installed = false; // base: the game's install record is there
    std::vector<library::OptionView> game_options; // base: what Install can add, each on or off
    library::Resolution game_resolution;           // base: the plan's picture size (0: none)
};
// What the installer is doing, for the frame that draws it.
struct JobView
{
    std::string id; // the title being changed; empty when idle
    install::Phase phase = install::Phase::idle;
    std::uint64_t done = 0, total = 0;
    std::vector<std::string> waiting; // titles queued behind it
    // A game install's stages (library::kGameStages): the one under way, and for each the line it
    // shows (what it is doing, or once done what it did). -1 and empty for other jobs.
    int stage = -1;
    std::vector<std::string> stages;
    float overall = -1.0f; // a game install: how far the whole of it is, 0..1
};

class Service
{
  public:
    // version is the running store's contentVersion; empty skips its update check.
    explicit Service(std::string root, std::string version = {})
        : root_(std::move(root)), version_(std::move(version))
    {
    }
    ~Service();
    // Set before start() to run the installer: one transaction at a time, on
    // its own worker, after recovering whatever the journal says was interrupted.
    bool installer = false;
    // Set before start(): whether to ask the catalog for a newer store.
    bool check_updates = true;
    // Captured at start; changes in Settings apply on the next launch.
    std::string catalog_url = catalog::kDefaultApi;
    bool verify_signatures = true;
    // With a custom catalog set: keep the official one and add the custom catalog's apps.
    bool with_official = false;
    // Tests only: replaces the environment built from the console's configuration.
    std::optional<install::Environment> installer_environment;
    bool start();
    void stop();
    bool take(std::vector<Update> &updates);
    bool request_detail(const std::string &id);
    bool request_icons(const std::vector<std::string> &ids);
    void report_frames(std::string report);
    // Never block: false means "ask again next frame" (or the queue is full).
    // location is the scanned folder that holds, or will hold, the app's folder.
    bool request_install(const catalog::Entry &entry, const std::string &location);
    bool request_uninstall(const std::string &id, const std::string &location);
    // Takes over an app installed by hand (a receipt is written, nothing else).
    bool request_adopt(const catalog::Entry &entry, const std::string &location);
    // Copies a game's base copy from a USB drive into base-games (entry.id names the game).
    bool request_import(const catalog::Entry &entry);
    // Installs (or repairs) a collection game into its prefix from its plan, with its Home
    // app in location (the scanned folder that holds, or will hold, it), or removes both.
    // options: the ids of the plan's options to install with (install only).
    bool request_game(const catalog::Entry &entry, bool remove, const std::string &location,
                      std::vector<std::string> options = {},
                      library::Overlay overlay = library::Overlay::off,
                      library::Cursor cursor = library::Cursor::touchpad,
                      library::Resolution resolution = {});
    // An installed game's settings (its performance overlay, cursor and picture size) written into its
    // profile and ini files now; nothing to do for a game that isn't installed (its next
    // install takes them).
    bool request_game_settings(const catalog::Entry &entry, library::Overlay overlay,
                               library::Cursor cursor = library::Cursor::touchpad,
                               library::Resolution resolution = {});
    // Set before start(): where the games' install plans are (<plans>/<folder>.json),
    // prospero-win's data, and the prefix games go into ("" for each plan's own; a test
    // prefix keeps a test install apart from the live one, its profile named after it).
    std::string plans;
    // Files shipped with the manager (<bundle>/<sha256> plan sources, <bundle>/
    // bare-prefix-sahin.zip): taken before downloading; empty for none.
    std::string bundle;
    std::string win_root = "/data/prospero-win";
    std::string game_prefix;
    // The settings file, written by a worker: never on the frame.
    bool save_settings(std::string text);
    // Where settings are kept; empty means the store folder's settings.txt. Set before
    // start() when settings can be kept although the store has no folder of its own.
    std::string settings_file;
    bool cancel_job(const std::string &id);
    bool job(JobView &view);
    // The titles running now, refreshed every two seconds by the installer's
    // worker. known is false while the console's sandbox folder can't be listed.
    bool running(std::vector<std::string> &ids, bool &known);
    // Development builds: the store's process ends on the spot when a
    // transaction reaches this named step, as it would in a power cut.
    void stop_at(std::string step);

  private:
    std::mutex stop_guard_;
    std::string stop_step_;
    struct Job
    {
        enum class Kind
        {
            install,
            uninstall,
            adopt,
            import_base,
            install_game,
            uninstall_game,
            game_settings
        } kind = Kind::install;
        catalog::Entry entry;
        std::string location;
        std::vector<std::string> options; // install_game: the plan's options chosen
        library::Overlay overlay = library::Overlay::off; // install_game, game_settings
        library::Cursor cursor = library::Cursor::touchpad;    // install_game, game_settings
        library::Resolution resolution = {};                   // install_game, game_settings
    };
    static void *entry(void *self);
    static void *icon_entry(void *self);
    static void *install_entry(void *self);
    void load_icons();
    void run();
    void run_installer();
    void publish(Update update);
    bool enqueue(Job job);
    // notes: what the user should check in ShadowMountPlus's settings, in plain words.
    bool load_policy(system::ScanPolicy &policy, std::vector<std::string> *notes = nullptr) const;
    std::string settings_path() const;
    std::string root_, version_;
    net::Control control_;
    net::Control icon_control_;
    net::Control job_control_;
    pthread_t thread_{};
    pthread_t icon_thread_{};
    pthread_t install_thread_{};
    bool started_ = false;
    bool icons_started_ = false;
    bool installer_started_ = false;
    std::mutex mutex_;
    std::vector<Update> updates_;
    std::string detail_;
    std::vector<std::string> icons_;
    std::vector<catalog::Entry> entries_;
    std::map<std::string, std::string> versions_;
    std::deque<Job> jobs_;
    std::string job_id_;
    std::vector<std::string> running_;
    bool running_known_ = false;
    install::Progress progress_;
    std::atomic<float> overall_{-1.0f}; // JobView::overall
    std::uint64_t generation_ = 0;
    bool online_ = false;
    std::string frame_report_;
    std::string settings_;
    bool settings_pending_ = false;
    // The base copies last published, by title ID; checked again every few seconds, and at
    // once after an import.
    std::map<std::string, library::BaseGame> bases_;
    std::atomic<bool> bases_stale_{true};
    void check_bases();
    bool import_job(const catalog::Entry &entry, std::string &error);
    bool game_job(const catalog::Entry &entry, bool remove, const std::string &location,
                  const std::vector<std::string> &chosen, library::Overlay overlay,
                  library::Cursor cursor, library::Resolution resolution,
                  const install::Environment &environment, std::string &error);
    bool settings_job(const catalog::Entry &entry, library::Overlay overlay, library::Cursor cursor,
                      library::Resolution resolution, std::string &error);
    bool home_app(const catalog::Entry &game, const std::string &location,
                  const install::Environment &environment, std::string &error);
    bool remove_home_app(const std::string &id, const std::string &location,
                         const install::Environment &environment, std::string &error);
    // The game's plan and the options this manager installs it with; false without one.
    bool game_plan(const catalog::Entry &entry, library::InstallPlan &plan,
                   const library::Manifest *&manifest, library::InstallOptions &options,
                   std::string &error) const;
    std::map<std::string, bool> installed_;
    std::map<std::string, std::vector<std::string>> options_on_; // as last published
    // The running game install's stage and lines (JobView), set by the worker.
    std::mutex stage_mutex_;
    int stage_ = -1;
    std::vector<std::string> stages_;
    void stage(int index, std::string line);
    void clear_stages();
};
} // namespace store
