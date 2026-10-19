// Prospero BlackBox - Host renderer for inspecting the same screens as the console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/ambient.hpp"
#include "app/keyboard_map.hpp"
#include "app/store.hpp"
#include "catalog/blackbox.hpp"
#include "catalog/icons.hpp"
#include "core/image.hpp"
#include "core/qr.hpp"
#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <cassert>
#include <set>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "../third_party/stb/stb_image_write.h"
#pragma clang diagnostic pop

static void check_artwork_requests()
{
    store::Screen screen;
    assert(screen.artwork().empty());
    std::vector<store::App> apps;
    for (unsigned i = 0; i < 100; ++i)
    {
        store::App app;
        app.title_id = "PPSA" + std::to_string(99000 + i);
        char name[16];
        std::snprintf(name, sizeof(name), "App %03u", i);
        app.name = name;
        app.kind = "app";
        apps.push_back(app);
    }
    screen.set_catalog(std::move(apps), "test");
    hui::ui::Feedback feedback;
    // Discover: down from "New and updated" to the Apps shelf, then along it.
    hui::InputFrame down, right;
    down.nav = hui::Direction::down;
    right.nav = hui::Direction::right;
    screen.update(down, 1.0f / 60.0f, feedback);
    for (unsigned i = 0; i < 20; ++i)
    {
        const auto wanted = screen.artwork();
        assert(!wanted.empty() && wanted.size() <= 16);
        assert(std::set<std::string>(wanted.begin(), wanted.end()).size() == wanted.size());
        screen.set_icon(wanted.front(), 1);
        screen.set_ambient(wanted.front(), 2);
        assert(screen.artwork() == wanted); // Uploading artwork must never reset focus.
        screen.update(right, 1.0f / 60.0f, feedback);
    }
    const auto focused = screen.artwork().front();
    assert(focused == "PPSA99020");
    hui::InputFrame confirm;
    confirm.pressed = hui::action_bit(hui::Action::confirm);
    screen.update(confirm, 1.0f / 60.0f, feedback);
    assert(screen.artwork() == std::vector<std::string>{focused});
    screen.set_catalog({}, "empty");
    assert(screen.artwork().empty());
}

static void check_search_and_sort()
{
    store::Screen screen;
    store::App a, b, c;
    a.title_id = "PPSA99001";
    a.name = "Zulu";
    a.author = "Bear Studio";
    a.released = "2026-01-01";
    a.updated = "2026-10-01";
    b.title_id = "PPSA99002";
    b.name = "alpha";
    b.author = "Another Developer";
    b.released = "2026-09-01";
    b.updated = "2026-09-01";
    c.title_id = "PPSA99003";
    c.name = "Coming Soon";
    a.kind = b.kind = c.kind = "game";
    screen.set_catalog({a, b, c}, "test");
    {
        // Sorting and search work on a section's grid: go to Games.
        hui::InputFrame next;
        next.pressed = hui::action_bit(hui::Action::page_next);
        hui::ui::Feedback quiet;
        screen.update(next, 1.0f / 60.0f, quiet);
    }
    assert(screen.artwork().front() == b.title_id);
    screen.set_query("STUDIO");
    assert(screen.artwork() == std::vector<std::string>{a.title_id});
    screen.set_query("ALpHa");
    assert(screen.artwork() == std::vector<std::string>{b.title_id});
    screen.set_query("no such app");
    assert(screen.artwork().empty());
    screen.set_query("");
    hui::InputFrame sort;
    sort.pressed = hui::action_bit(hui::Action::r3);
    hui::ui::Feedback feedback;
    screen.update(sort, 1.0f / 60.0f, feedback);
    assert(screen.artwork().front() == b.title_id); // Newest release.
    screen.update(sort, 1.0f / 60.0f, feedback);
    assert(screen.artwork().front() == a.title_id); // Recently updated.
    screen.set_catalog({c, b, a}, "refreshed");
    assert(screen.artwork().front() == a.title_id);
    screen.update(sort, 1.0f / 60.0f, feedback);
    assert(screen.artwork().front() == b.title_id);
    hui::InputFrame search;
    search.pressed = hui::action_bit(hui::Action::north);
    screen.update(search, 1.0f / 60.0f, feedback);
    assert(screen.pending_search);
    screen.pending_search = false;
    hui::InputFrame open;
    open.pressed = hui::action_bit(hui::Action::confirm);
    screen.update(open, 1.0f / 60.0f, feedback);
    assert(screen.pending_detail == b.title_id);
    screen.set_detail_error(b.title_id, "offline");
    screen.pending_detail.clear();
    screen.update(search, 1.0f / 60.0f, feedback);
    assert(screen.pending_detail == b.title_id && !screen.pending_search);
    store::catalog::Entry detail;
    detail.id = b.title_id;
    detail.description = "Verified description";
    screen.set_detail(detail);
    // A developer's page: that developer's apps alone, with the link their details give.
    screen.set_catalog({a, b, c}, "again");
    detail = {};
    detail.id = a.title_id;
    detail.source = "https://github.com/bear-studio/zulu";
    screen.set_detail(detail);
    assert(screen.open_developer("Bear Studio") && screen.developer() == "Bear Studio");
    assert(screen.artwork() == std::vector<std::string>{a.title_id});
    assert(screen.pending_developer_link == "https://github.com/bear-studio");
    assert(!screen.open_developer(""));
    hui::InputFrame leave;
    leave.pressed = hui::action_bit(hui::Action::back);
    screen.update(leave, 1.0f / 60.0f, feedback);
    assert(screen.developer().empty() && screen.artwork().size() == 3);
    {
        bool support = false;
        store::catalog::Entry who;
        assert(store::catalog::developer_link(who, support).empty() && !support);
        who.source = "https://example.com/someone/app";
        assert(store::catalog::developer_link(who, support).empty());
        who.source = "https://codeberg.org/someone/app";
        assert(store::catalog::developer_link(who, support) == "https://codeberg.org/someone");
        who.author_url = "https://someone.example/";
        assert(store::catalog::developer_link(who, support) == "https://someone.example/" &&
               !support);
        who.donate_url = "https://ko-fi.com/someone";
        assert(store::catalog::developer_link(who, support) == "https://ko-fi.com/someone" &&
               support);
        who.donate_url = "http://ko-fi.com/someone"; // not HTTPS: passed over
        assert(store::catalog::developer_link(who, support) == "https://someone.example/");
    }
    screen.set_catalog({}, "removed");
    assert(screen.artwork().empty());
    screen.update(search, 1.0f / 60.0f, feedback);
    assert(screen.pending_search);
}

static void check_wrapping_and_closing()
{
    store::Screen screen;
    store::App app;
    app.title_id = "PPSA99001";
    app.name = "One";
    app.kind = "app";
    screen.set_catalog({app}, "test");
    hui::ui::Feedback quiet;
    // L1 on the first section goes round to the last one (Updates: nothing listed), and R1 back.
    hui::InputFrame previous, next;
    previous.pressed = hui::action_bit(hui::Action::page_prev);
    next.pressed = hui::action_bit(hui::Action::page_next);
    screen.update(previous, 1.0f / 60.0f, quiet);
    assert(screen.artwork().empty());
    screen.update(next, 1.0f / 60.0f, quiet);
    assert(screen.artwork() == std::vector<std::string>{app.title_id});
    // Circle: a tap does not close, a hold does, and a hold that began on a page does not.
    hui::InputFrame press, hold;
    press.pressed = press.held = hui::action_bit(hui::Action::back);
    hold.held = hui::action_bit(hui::Action::back);
    screen.update(press, 1.0f / 60.0f, quiet);
    screen.update({}, 1.0f / 60.0f, quiet);
    assert(!screen.wants_quit());
    assert(screen.open_app(app.title_id));
    screen.pending_detail.clear();
    screen.update(press, 1.0f / 60.0f, quiet); // leaves the page
    for (int frame = 0; frame < 200; ++frame)
        screen.update(hold, 1.0f / 60.0f, quiet);
    assert(!screen.page_open() && !screen.wants_quit());
    screen.update({}, 1.0f / 60.0f, quiet);
    screen.update(press, 1.0f / 60.0f, quiet);
    for (int frame = 0; frame < 60; ++frame)
        screen.update(hold, 1.0f / 60.0f, quiet);
    assert(!screen.wants_quit()); // a second in: not yet
    for (int frame = 0; frame < 60; ++frame)
        screen.update(hold, 1.0f / 60.0f, quiet);
    assert(screen.wants_quit());
}

static void check_installed_sections()
{
    store::Screen screen;
    store::App app;
    app.title_id = "PPSA99010";
    app.name = "Example";
    app.available_version = "02.000.000";
    screen.set_catalog({app}, "verified", true);
    store::system::Inventory inventory;
    store::system::InstalledApp installed;
    installed.id = app.title_id;
    installed.name = app.name;
    installed.version = "01.000.000";
    installed.path = "/data/homebrew/" + app.title_id;
    installed.managed = true;
    inventory.apps.push_back(installed);
    screen.set_inventory(inventory);
    hui::InputFrame section;
    section.pressed = hui::action_bit(hui::Action::page_next);
    hui::ui::Feedback feedback;
    // Installed is the fourth section, Updates the fifth.
    for (int index = 0; index < 3; ++index)
        screen.update(section, 1.0f / 60.0f, feedback);
    assert(screen.artwork() == std::vector<std::string>{app.title_id});
    screen.update(section, 1.0f / 60.0f, feedback);
    assert(screen.artwork() == std::vector<std::string>{app.title_id});
    // Update all: Square asks for the details, then orders the update.
    screen.set_installer(true, true, "", "/data/homebrew");
    screen.set_running({}, true);
    hui::InputFrame square;
    square.pressed = hui::action_bit(hui::Action::west);
    screen.update(square, 1.0f / 60.0f, feedback);
    screen.update({}, 1.0f / 60.0f, feedback);
    assert(screen.pending_detail == app.title_id);
    assert(screen.pending_order.kind == store::Order::Kind::none);
    store::catalog::Entry listed;
    listed.id = app.title_id;
    listed.format = "zip";
    listed.digest = std::string(64, 'a');
    screen.set_detail(listed);
    screen.update({}, 1.0f / 60.0f, feedback);
    assert(screen.pending_order.kind == store::Order::Kind::install);
    assert(screen.pending_order.entry.digest == listed.digest);
    assert(screen.pending_order.location == "/data/homebrew");
    screen.pending_order = {};
    screen.pending_detail.clear();
    // A running app is passed over.
    screen.set_running({app.title_id}, true);
    screen.update(square, 1.0f / 60.0f, feedback);
    screen.update({}, 1.0f / 60.0f, feedback);
    assert(screen.pending_order.kind == store::Order::Kind::none);
    screen.set_running({}, true);
    inventory.apps.front().managed = false;
    screen.set_inventory(inventory);
    assert(screen.artwork().empty());
}

static void check_keyboard_keys()
{
    using namespace hui::pad_bits;
    const auto press = [](std::initializer_list<std::uint16_t> keys, std::uint32_t modifiers = 0)
    {
        const std::vector<std::uint16_t> held(keys);
        return store::keyboard_buttons(held, modifiers);
    };
    assert(press({store::hid::kUp}) == kUp && press({store::hid::kLeft}) == kLeft);
    assert(press({store::hid::kEnter}) == kCross && press({store::hid::kSpace}) == kCross);
    assert(press({store::hid::kEscape}) == kCircle && press({store::hid::kBackspace}) == kCircle);
    assert(press({store::hid::kTab}) == kR1 && press({store::hid::kTab}, 0x02) == kL1);
    assert(press({store::hid::kPageDown}) == kR1 && press({store::hid::kPageUp}) == kL1);
    assert(press({store::hid::kSlash}) == kTriangle && press({store::hid::kDelete}) == kSquare);
    assert(press({store::hid::kF10}) == kOptions && press({store::hid::kF5}) == kR3);
    assert(press({store::hid::kDown, store::hid::kEnter}) == (kDown | kCross));
    assert(press({0x04 /* A */}) == 0 && press({}) == 0);
}

static void check_hold_to_uninstall()
{
    store::Screen screen;
    store::App app;
    app.title_id = "PPSA99010";
    app.name = "Example";
    app.kind = "app";
    app.available_version = "01.000.000";
    screen.set_catalog({app}, "verified", true);
    store::system::Inventory inventory;
    store::system::InstalledApp installed;
    installed.id = app.title_id;
    installed.name = app.name;
    installed.version = "01.000.000";
    installed.path = "/data/homebrew/" + app.title_id;
    installed.managed = true;
    inventory.apps.push_back(installed);
    screen.set_inventory(inventory);
    screen.set_installer(true, true, "", "/data/homebrew");
    screen.set_running({}, true);
    assert(screen.open_app(app.title_id));
    store::catalog::Entry listed;
    listed.id = app.title_id;
    listed.version = "01.000.000";
    listed.content_version = "01.000.000";
    listed.format = "zip";
    listed.digest = std::string(64, 'a');
    screen.set_detail(listed);
    hui::ui::Feedback feedback;
    for (int frame = 0; frame < 30; ++frame)
        screen.update({}, 1.0f / 60.0f, feedback);
    // A tap does nothing but say how.
    hui::InputFrame tap;
    tap.pressed = hui::action_bit(hui::Action::confirm);
    tap.held = tap.pressed;
    screen.update(tap, 1.0f / 60.0f, feedback);
    hui::InputFrame up;
    up.released = hui::action_bit(hui::Action::confirm);
    screen.update(up, 1.0f / 60.0f, feedback);
    assert(screen.pending_order.kind == store::Order::Kind::none);
    // Letting go halfway does nothing either.
    hui::InputFrame hold;
    hold.held = hui::action_bit(hui::Action::confirm);
    screen.update(tap, 1.0f / 60.0f, feedback);
    for (int frame = 0; frame < 30; ++frame)
        screen.update(hold, 1.0f / 60.0f, feedback);
    screen.update(up, 1.0f / 60.0f, feedback);
    assert(screen.pending_order.kind == store::Order::Kind::none);
    for (int frame = 0; frame < 120; ++frame)
        screen.update({}, 1.0f / 60.0f, feedback);
    // A full hold orders the uninstall.
    screen.update(tap, 1.0f / 60.0f, feedback);
    for (int frame = 0; frame < 90 && screen.pending_order.kind == store::Order::Kind::none;
         ++frame)
        screen.update(hold, 1.0f / 60.0f, feedback);
    assert(screen.pending_order.kind == store::Order::Kind::uninstall);

    store::Settings settings;
    settings.reduce_motion = true;
    assert(store::parse_settings(store::format_settings(settings)).reduce_motion);
    assert(!store::parse_settings("location=/data/homebrew\n").reduce_motion);
}

int main(int argc, char **argv)
{
    check_artwork_requests();
    check_search_and_sort();
    check_wrapping_and_closing();
    check_installed_sections();
    check_hold_to_uninstall();
    check_keyboard_keys();
    if (argc < 3 || argc > 5)
        return 2;
    const auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                    : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
        return 3;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        return 4;
    {
        hui::gfx::set_glsl_prefix("#version 450 core\n");
        hui::gfx::Renderer renderer;
        store::Fonts fonts;
        hui::gfx::Canvas target;
        if (!renderer.init() || !fonts.load(renderer, argv[1]) || !target.create(1920, 1080, 1))
            return 5;
        store::Screen screen;
        // Settings previews do not need a live catalog or cached downloads.
        if (argc == 4 &&
            (std::string(argv[3]) == "settings" || std::string(argv[3]) == "catalog-settings"))
        {
            screen.open_panel(1);
            hui::ui::Feedback quiet;
            hui::InputFrame down;
            down.nav = hui::Direction::down;
            if (std::string(argv[3]) == "catalog-settings")
            {
                for (int i = 0; i < 7; ++i)
                    screen.update(down, 0.016f, quiet);
                hui::InputFrame confirm;
                confirm.pressed = hui::action_bit(hui::Action::confirm);
                screen.update(confirm, 0.016f, quiet);
            }
        }
        std::vector<GLuint> textures;
        {
            std::string encoded;
            hui::Image image;
            if (hui::save::read_file(std::string(argv[1]) + "/images/coming-soon.png", &encoded) &&
                hui::decode_png(encoded, image))
            {
                textures.push_back(
                    renderer.batch().create_texture(image.width, image.height, image.rgba.data()));
                screen.set_coming_soon_art(textures.back());
            }
        }
        if (argc >= 4 && std::string(argv[3]) != "settings" &&
            std::string(argv[3]) != "catalog-settings")
        {
            // argv[3]: a built app folder (dist/PPSA99400), with assets/games and sce_sys.
            const std::string app_root = argv[3];
            const auto snapshot = store::catalog::blackbox_snapshot(app_root);
            std::vector<store::App> apps;
            for (const auto &entry : snapshot.entries)
            {
                apps.push_back({entry.id, entry.name, entry.author, entry.description, entry.kind,
                                entry.version, entry.status == "coming_soon" ? "Coming soon" : "",
                                0, entry.released, entry.updated});
                apps.back().available_version = entry.content_version;
                apps.back().size = entry.size;
                apps.back().added = entry.added;
            }
            screen.set_catalog(std::move(apps), "Sahin");
            const auto picture = [&](const std::string &path, hui::Image &image)
            {
                std::string encoded;
                return hui::save::read_file(path, &encoded, 2u << 20) &&
                       hui::decode_png(encoded, image);
            };
            hui::Image logo;
            const bool cutout = picture(app_root + "/assets/logo.png", logo);
            if (cutout || picture(app_root + "/sce_sys/icon0.png", logo))
            {
                textures.push_back(
                    renderer.batch().create_texture(logo.width, logo.height, logo.rgba.data()));
                screen.set_logo(textures.back(), cutout);
            }
            for (const auto &entry : snapshot.entries)
            {
                hui::Image image;
                if (!picture(entry.icon, image))
                    continue;
                const auto texture =
                    renderer.batch().create_texture(image.width, image.height, image.rgba.data());
                textures.push_back(texture);
                screen.set_icon(entry.id, texture);
                // Its own picture, made from the icon, and the colour the screen leans toward.
                std::uint32_t vivid = 0;
                const auto ambient = store::make_ambient(image, vivid);
                if (!ambient.rgba.empty())
                {
                    const auto field = renderer.batch().create_texture(
                        ambient.width, ambient.height, ambient.rgba.data());
                    textures.push_back(field);
                    screen.set_ambient(entry.id, field);
                }
                screen.set_accent(
                    entry.id, hui::gfx::Color::rgb(vivid ? vivid : store::average_colour(image)));
            }
            // The built-in catalog holds every page's details.
            for (const auto &entry : snapshot.entries)
                screen.set_detail(entry);
        }
        hui::ui::Feedback feedback;
        if (argc == 5)
        {
            const std::string mode = argv[4];
            hui::InputFrame down, right, nav_up;
            down.nav = hui::Direction::down;
            right.nav = hui::Direction::right;
            nav_up.nav = hui::Direction::up;
            const auto settle = [&](int frames)
            {
                for (int frame = 0; frame < frames; ++frame)
                    screen.update({}, 1.0f / 60.0f, feedback);
            };
            if (mode.starts_with("mw-"))
            {
                // Blur's page in one of its install states: mw-ready, mw-old (a 1.2
                // base copy), mw-installed, mw-busy (Blur's Home app running).
                store::library::BaseGame base;
                base.state = mode == "mw-old"       ? store::library::State::needs_patch
                              : mode == "mw-solo-new" ? store::library::State::absent
                                                      : store::library::State::ready;
                base.version = mode == "mw-old" ? "1.2" : "1.3";
                base.path = "/data/sahin/base-games/BLUR";
                if (mode == "mw-solo-usb") // the copy on a USB drive: Install copies it first
                {
                    base.on_usb = true;
                    base.path = "/mnt/usb1/BLUR";
                }
                base.detail = mode == "mw-old" ? "Version 1.2: upgraded to 1.3 as it installs."
                                               : "Version " + base.version + ".";
                screen.set_installer(true, true, "", "/data/homebrew");
                screen.set_running(mode == "mw-busy" ? std::vector<std::string>{"PPSA99404"}
                                                     : std::vector<std::string>{},
                                   true);
                // The plan's options, as the Service publishes them.
                screen.set_base("PPSA99404", base,
                                mode == "mw-installed" || mode.starts_with("mw-solo-installed"),
                                {{"online", "Online through Amax",
                                  "Multiplayer on the Amax community servers (amax-emu.com).", true},
                                 {"amax-client", "Amax client extras",
                                  "Amax's client: profile pictures, its Lua plugins and a 60 fps "
                                  "race cap.",
                                  false}});
                if (mode.starts_with("mw-solo"))
                {
                    // The game's own app: its page only (installed, the player quit to it).
                    screen.set_single_game("PPSA99404");
                }
                assert(screen.open_app("PPSA99404"));
                settle(90);
                if (mode == "mw-ask")
                {
                    // Install pressed: the question about the option.
                    hui::InputFrame press;
                    press.pressed = hui::action_bit(hui::Action::confirm);
                    screen.update(press, 1.0f / 60.0f, feedback);
                    settle(40);
                }
                else if (mode == "mw-settings")
                {
                    // Triangle: the game's settings, Full picked.
                    hui::InputFrame press, down;
                    press.pressed = hui::action_bit(hui::Action::north);
                    screen.update(press, 1.0f / 60.0f, feedback);
                    settle(30);
                    down.nav = hui::Direction::down;
                    for (int i = 0; i < 2; ++i)
                    {
                        screen.update(down, 1.0f / 60.0f, feedback);
                        settle(8);
                    }
                    hui::InputFrame pick;
                    pick.pressed = hui::action_bit(hui::Action::confirm);
                    screen.update(pick, 1.0f / 60.0f, feedback);
                    settle(40);
                }
                else if (mode == "mw-solo-installed-options")
                {
                    // A game's own app: the Amax client switched on, so Cross repairs.
                    screen.update(down, 1.0f / 60.0f, feedback);
                    settle(8);
                    screen.update(right, 1.0f / 60.0f, feedback);
                    settle(30);
                }
                else if (mode == "mw-solo-installed-resolution")
                {
                    // A game's own app: 2560x1440 picked, kept at once; Cross still plays.
                    for (int i = 0; i < 2; ++i)
                    {
                        screen.update(down, 1.0f / 60.0f, feedback);
                        settle(8);
                    }
                    screen.update(right, 1.0f / 60.0f, feedback);
                    settle(30);
                    const auto kept = screen.settings().resolutions.find("PPSA99404");
                    assert(kept != screen.settings().resolutions.end() && kept->second.width == 2560 &&
                           screen.settings_changed &&
                           screen.pending_order.kind == store::Order::Kind::game_settings &&
                           screen.pending_order.resolution.height == 1440);
                    assert(store::parse_settings(store::format_settings(screen.settings()))
                               .resolutions.at("PPSA99404")
                               .width == 2560);
                }
                else if (mode == "mw-installing" || mode == "mw-solo-installing")
                {
                    // Downloading, the first stage, the box grown.
                    screen.set_activity({"PPSA99404", 1, 640u << 20, 1495u << 20, {}, 1,
                                         {"Copied to the console; the USB drive can go",
                                          "Getting Amax client 0.0.0.7 (2 of 3)"}, 0.31f});
                    settle(60);
                }
                else if (mode == "mw-installing-files")
                {
                    // Further on: the game's files, the stages before it done.
                    screen.set_activity({"PPSA99404", 6, 1200u << 20, 3000u << 20, {}, 4,
                                         {"On the console", "All 17 files ready", "Using the prefix pb-mw",
                                          "Your copy and the mod files check out",
                                          "Copying your game into the prefix"}, 0.72f});
                    settle(60);
                }
            }
            else if (mode.starts_with("theme-"))
            {
                // theme-<n>: n steps right (or left, negative) along the collection, then the
                // colours settle on that game's.
                hui::InputFrame left;
                left.nav = hui::Direction::left;
                const int steps = std::atoi(mode.c_str() + 6);
                for (int i = 0; i < std::abs(steps); ++i)
                {
                    screen.update(steps < 0 ? left : right, 1.0f / 60.0f, feedback);
                    settle(12);
                }
                settle(90);
            }
            else if (mode == "stage")
            {
                // The featured app on the stage, the focus on its button.
                screen.update(nav_up, 1.0f / 60.0f, feedback);
                settle(30);
            }
            else if (mode == "shelf" || mode == "shelf-2" || mode == "shelf-3")
            {
                const int downs = mode == "shelf" ? 0 : mode == "shelf-2" ? 1 : 2;
                for (int i = 0; i < downs; ++i)
                {
                    screen.update(down, 1.0f / 60.0f, feedback);
                    settle(20);
                }
                for (int i = 0; i < 2; ++i)
                {
                    screen.update(right, 1.0f / 60.0f, feedback);
                    settle(20);
                }
            }
            else if (mode == "games" || mode == "apps")
            {
                hui::InputFrame section;
                section.pressed = hui::action_bit(hui::Action::page_next);
                for (int index = 0; index < (mode == "apps" ? 1 : 2); ++index)
                    screen.update(section, 1.0f / 60.0f, feedback);
                settle(30);
                screen.update(right, 1.0f / 60.0f, feedback);
                settle(20);
            }
            else if (mode == "page-light" || mode == "page-installing" ||
                     mode == "page-unpacking" || mode == "page-installed" || mode == "page-eden")
            {
                screen.set_installer(true, true, "", "/data/homebrew");
                screen.set_running({}, true);
                screen.set_locations({{"/data/homebrew", 548ull << 30}});
                const std::string id = mode == "page-eden"        ? "PPSA99008"
                                       : mode == "page-installed" ? "PPSA99006"
                                       : mode == "page-light"     ? "PPSA99002"
                                                                  : "PPSA99169";
                if (mode == "page-installed")
                {
                    store::system::Inventory inventory;
                    inventory.apps = {{id, "ProsperoPuzzles", "01.000.010", "/data/homebrew/" + id,
                                       "", false, true, false}};
                    screen.set_inventory(std::move(inventory));
                }
                assert(screen.open_app(id));
                screen.pending_detail.clear();
                hui::Image qr;
                assert(hui::encode_qr("https://homebrew.page/app/" + id + "/", qr));
                const auto texture =
                    renderer.batch().create_texture(qr.width, qr.height, qr.rgba.data());
                textures.push_back(texture);
                screen.set_qr(id, texture, qr.width);
                if (mode == "page-installing" || mode == "page-unpacking")
                {
                    const std::uint64_t total =
                        mode == "page-installing" ? 197867360ull : 412000000ull;
                    const int phase = mode == "page-installing" ? 1 : 3;
                    screen.set_activity({id, phase, total / 5, total, {"PPSA99420"}, -1, {}});
                    settle(60);
                    screen.set_activity({id, phase, total * 43 / 100, total, {"PPSA99420"}, -1, {}});
                }
            }
            else if (mode == "reel-install")
            {
                screen.set_installer(true, true, "", "/data/homebrew");
                screen.set_running({}, true);
                screen.set_locations({{"/data/homebrew", 548ull << 30}});
                assert(screen.open_app("PPSA99169"));
                screen.pending_detail.clear();
                hui::Image qr;
                assert(hui::encode_qr("https://homebrew.page/app/PPSA99169/", qr));
                const auto texture =
                    renderer.batch().create_texture(qr.width, qr.height, qr.rgba.data());
                textures.push_back(texture);
                screen.set_qr("PPSA99169", texture, qr.width);
            }
            else if (mode == "reel-browse")
            {
            }
            else if (mode == "reel-intro")
                screen.play_intro();
            else if (mode == "intro")
            {
                // The opening a second in: the emblem landed, the name in.
                screen.play_intro();
                settle(60);
            }
            else if (mode == "busy-home")
            {
                screen.set_installer(true, true, "", "/data/homebrew");
                screen.set_activity(
                    {"PPSA99169", 1, 197867360ull / 5, 197867360ull, {"PPSA99420"}, -1, {}});
                settle(60);
                screen.set_activity(
                    {"PPSA99169", 1, 197867360ull * 43 / 100, 197867360ull, {"PPSA99420"}, -1, {}});
                for (int i = 0; i < 1; ++i)
                {
                    screen.update(right, 1.0f / 60.0f, feedback);
                    settle(20);
                }
            }
            else if (mode == "search")
                screen.set_query("radio");
            else if (mode == "closing")
            {
                // Circle is held after the screen has settled: see below.
            }
            else if (mode == "news")
            {
                screen.set_news(
                    {{"2026-10-11-store-060", "2026-10-11T09:00:00Z", "Sahin 1.000.060",
                      "Developer pages, a News room and a shelf for apps new to the catalog.\n\n"
                      "Open an app and press R3 to see everything else its developer made."},
                     {"2026-10-08-mirror", "2026-10-08T18:00:00Z", "The catalog has a mirror",
                      "When homebrew.page can't be reached, the store now reads the same signed "
                      "catalog from its mirror."}});
                screen.open_panel(3);
                settle(60);
            }
            else if (mode == "whats-new")
            {
                screen.update(down, 1.0f / 60.0f, feedback);
                settle(40);
            }
            else if (mode == "developer")
            {
                // The page of the developer of the first app whose details are here.
                screen.open_developer("BlackBearReloaded");
                const std::string link = screen.pending_developer_link.empty()
                                             ? "https://github.com/blackbearreloaded"
                                             : screen.pending_developer_link;
                hui::Image qr;
                assert(hui::encode_qr(link, qr));
                const auto texture =
                    renderer.batch().create_texture(qr.width, qr.height, qr.rgba.data());
                textures.push_back(texture);
                screen.pending_developer_link = link;
                screen.set_developer_qr(link, texture);
                settle(40);
            }
            else if (mode == "busy")
            {
                // An install in progress, seen from the grid and the top bar.
                screen.set_installer(true, true, "", "/data/homebrew");
                screen.set_activity({"PPSA99007", 1, 30u << 20, 58u << 20, {"PPSA99420"}, -1, {}});
            }
            else if (mode == "queue" || mode == "downloads" || mode == "settings" ||
                     mode == "about")
            {
                screen.set_installer(true, true, "", "/data/homebrew");
                screen.set_self("PPSA99400", "01.000.000");
                screen.set_locations(
                    {{"/data/homebrew", 548ull << 30}, {"/mnt/ext1/homebrew", 912ull << 30}});
                if (mode == "queue" || mode == "downloads")
                {
                    screen.set_activity(
                        {"PPSA99007", 3, 61u << 20, 94u << 20, {"PPSA99420", "PPSA99169"}, -1, {}});
                    screen.finish_job(
                        true, false, "ProsperoRadio installed",
                        "ShadowMountPlus will add it to your home screen in a moment.");
                    screen.finish_job(false, false, "PS5SX2: not changed", "Close the app first");
                }
                screen.open_panel(mode == "queue" || mode == "downloads" ? 0
                                  : mode == "settings"                   ? 1
                                                                         : 2);
                if (mode == "downloads")
                    settle(700);
                // Settings: down to the location, right to the next one, saved once.
                if (mode == "settings")
                {
                    hui::InputFrame right;
                    right.nav = hui::Direction::right;
                    screen.update(right, 1.0f / 60.0f, feedback);
                    assert(screen.settings().location == "/mnt/ext1/homebrew");
                    assert(screen.settings_changed);
                    assert(
                        store::parse_settings(store::format_settings(screen.settings())).location ==
                        "/mnt/ext1/homebrew");
                    screen.update(right, 1.0f / 60.0f, feedback);
                    assert(screen.settings().location == "/data/homebrew");
                    assert(!store::parse_settings("location=../x\nsounds=0\n").sounds);
                    assert(store::parse_settings("location=../x\n").location == "/data/homebrew");
                }
                if (mode == "queue")
                {
                    // Cross on the first row cancels the running job.
                    hui::InputFrame cross;
                    cross.pressed = hui::action_bit(hui::Action::confirm);
                    screen.update(cross, 1.0f / 60.0f, feedback);
                    assert(screen.pending_order.kind == store::Order::Kind::cancel);
                    assert(screen.pending_order.id == "PPSA99007");
                    screen.pending_order = {};
                }
            }
            else if (mode == "notice")
                screen.notify("Update available: 01.000.010",
                              "A newer Sahin is available.");
            else if (mode == "scrolled" || mode == "coming-soon")
            {
                hui::InputFrame move;
                move.nav = hui::Direction::down;
                move.pressed = mode == "scrolled" ? 0 : hui::action_bit(hui::Action::page_next);
                for (int index = 0; index < (mode == "scrolled" ? 2 : 4); ++index)
                    screen.update(move.pressed ? hui::InputFrame{.pressed = move.pressed} : move,
                                  1.0f / 60.0f, feedback);
            }
            else if (mode == "empty")
                screen.set_query("no matching application");
            else if (mode == "updated")
            {
                hui::InputFrame sort;
                sort.pressed = hui::action_bit(hui::Action::r3);
                screen.update(sort, 1.0f / 60.0f, feedback);
                screen.update(sort, 1.0f / 60.0f, feedback);
            }
            else if (mode == "installed" || mode == "updates" || mode == "local-detail")
            {
                store::system::Inventory inventory;
                inventory.apps = {
                    {"PPSA99001", "ProsperoRadio", "01.000.000", "/data/homebrew/PPSA99001", "",
                     false, true, false},
                    {"PPSA99002", "ProsperoLight", "01.000.000", "/data/homebrew/PPSA99002",
                     "Installed outside Sahin. Not managed by this app.", false, false,
                     false},
                    {"PPSA99980", "Example local app", "01.000.001", "/mnt/ext1/homebrew/PPSA99980",
                     "Installed outside Sahin. Not managed by this app.", false, false,
                     false},
                    {"", "Example.ffpkg", "", "/mnt/ext1/homebrew/Example.ffpkg",
                     "Image installed outside Sahin. Not managed by this app.", true, false,
                     false}};
                screen.set_inventory(std::move(inventory));
                screen.set_status("Installed library preview");
                hui::InputFrame section;
                section.pressed = hui::action_bit(hui::Action::page_next);
                for (int index = 0; index < (mode == "updates" ? 6 : 5); ++index)
                    screen.update(section, 1.0f / 60.0f, feedback);
                if (mode == "local-detail")
                {
                    screen.set_query("Example local");
                    hui::InputFrame open;
                    open.pressed = hui::action_bit(hui::Action::confirm);
                    screen.update(open, 1.0f / 60.0f, feedback);
                    assert(screen.pending_detail.empty());
                }
            }
            else if (mode == "detail" || mode == "detail-error" || mode == "detail-end" ||
                     mode == "detail-armed" || mode == "detail-progress" || mode == "confirm" ||
                     mode == "adopt")
            {
                if (mode == "detail-armed" || mode == "detail-progress" || mode == "confirm" ||
                    mode == "adopt")
                    screen.set_installer(true, true, "", "/data/homebrew");
                if (mode == "adopt")
                {
                    // Installed by hand, listed in the catalog: it can be handed over.
                    store::system::Inventory inventory;
                    inventory.apps = {{"PPSA99001", "ProsperoRadio", "01.000.005",
                                       "/data/homebrew/PPSA99001",
                                       "Installed outside Sahin. Not managed by this app.",
                                       false, false, false}};
                    screen.set_inventory(std::move(inventory));
                }
                if (mode == "confirm")
                {
                    store::system::Inventory inventory;
                    inventory.apps = {{"PPSA99001", "ProsperoRadio", "01.000.000",
                                       "/data/homebrew/PPSA99001", "", false, true, false}};
                    screen.set_inventory(std::move(inventory));
                }
                screen.set_query("radio");
                hui::InputFrame open;
                open.pressed = hui::action_bit(hui::Action::confirm);
                screen.update(open, 1.0f / 60.0f, feedback);
                assert(!screen.pending_detail.empty());
                if (mode == "detail-error")
                    screen.set_detail_error(screen.pending_detail, "The network is unavailable.");
                else
                {
                    store::catalog::Entry entry;
                    for (const auto &game : store::catalog::blackbox_snapshot(argv[3]).entries)
                        if (game.id == screen.pending_detail)
                            entry = game;
                    assert(!entry.id.empty());
                    screen.set_detail(entry);
                    if (mode == "detail-progress")
                    {
                        // Two samples a second apart give the page a speed, and so a time left.
                        screen.set_activity({entry.id, 1, entry.size / 5, entry.size, {}, -1, {}});
                        for (int frame = 0; frame < 60; ++frame)
                            screen.update({}, 1.0f / 60.0f, feedback);
                        screen.set_activity({entry.id, 1, entry.size * 2 / 5, entry.size, {}, -1, {}});
                    }
                    if (mode == "detail-armed")
                    {
                        // Cross asks for the install of exactly this verified record.
                        screen.update(open, 1.0f / 60.0f, feedback);
                        assert(screen.pending_order.kind == store::Order::Kind::install);
                        assert(screen.pending_order.entry.digest == entry.digest);
                        assert(screen.pending_order.location == "/data/homebrew");
                        screen.pending_order = {};
                    }
                    if (mode == "adopt")
                    {
                        // Cross asks first; the answer orders the hand-over, nothing else.
                        screen.update(open, 1.0f / 60.0f, feedback);
                        assert(screen.pending_order.kind == store::Order::Kind::none);
                    }
                    if (mode == "confirm")
                    {
                        // Square asks first; nothing is ordered until the answer.
                        hui::InputFrame square;
                        square.pressed = hui::action_bit(hui::Action::west);
                        screen.update(square, 1.0f / 60.0f, feedback);
                        assert(screen.pending_order.kind == store::Order::Kind::none);
                    }
                }
            }
            else
                return 9;
        }
        // Two seconds to settle, except for the opening, which would be over by then.
        const int warm_up = argc == 5 && std::string(argv[4]) == "intro" ? 0 : 120;
        for (int frame = 0; frame < warm_up; ++frame)
            screen.update({}, 1.0f / 60.0f, feedback);
        if (const char *reel = std::getenv("STORE_REEL"); reel && argc == 5)
        {
            // A clip: presses at given frames, every second frame written as a
            // 960x540 JPEG; the installer's progress is simulated for "reel-install".
            const std::string scene = argv[4];
            std::vector<std::pair<int, std::string>> steps;
            int frames = 0;
            if (scene == "reel-browse")
            {
                steps = {{20, "up"},     {150, "right"},   {230, "down"}, {300, "right"},
                         {360, "right"}, {420, "right"},   {490, "down"}, {550, "right"},
                         {610, "right"}, {680, "confirm"}, {860, "back"}, {960, "r1"},
                         {1030, "r1"},   {1100, "right"}};
                frames = 1190;
            }
            else if (scene == "reel-install")
            {
                steps = {{40, "confirm"}};
                frames = 760;
            }
            else if (scene == "reel-intro")
            {
                frames = 300;
                screen.play_intro(); // after the warm-up frames, so the clip starts at zero
            }
            const auto press = [](const std::string &name)
            {
                hui::InputFrame input;
                if (name == "up" || name == "down" || name == "left" || name == "right")
                    input.nav = name == "up"     ? hui::Direction::up
                                : name == "down" ? hui::Direction::down
                                : name == "left" ? hui::Direction::left
                                                 : hui::Direction::right;
                else if (name == "confirm")
                    input.pressed = hui::action_bit(hui::Action::confirm);
                else if (name == "back")
                    input.pressed = hui::action_bit(hui::Action::back);
                else if (name == "r1")
                    input.pressed = hui::action_bit(hui::Action::page_next);
                return input;
            };
            std::vector<unsigned char> pixels(1920 * 1080 * 4), half(960 * 540 * 3);
            stbi_flip_vertically_on_write(1);
            const std::string id = "PPSA99169";
            const std::uint64_t total = 197867360ull, unpacked = 412000000ull;
            for (int frame = 0; frame < frames; ++frame)
            {
                hui::InputFrame input;
                for (const auto &[at, name] : steps)
                    if (at == frame)
                        input = press(name);
                if (scene == "reel-install")
                {
                    // Download 4 s, verify 0.5 s, unpack 2.5 s, finish 0.4 s, then installed.
                    const int start = 60;
                    const int f = frame - start;
                    if (f >= 0 && f < 240)
                        screen.set_activity(
                            {id, 1, total * static_cast<std::uint64_t>(f) / 240, total, {}, -1, {}});
                    else if (f >= 240 && f < 270)
                        screen.set_activity({id, 2, 0, 0, {}, -1, {}});
                    else if (f >= 270 && f < 420)
                        screen.set_activity({id,
                                             3,
                                             unpacked * static_cast<std::uint64_t>(f - 270) / 150,
                                             unpacked,
                                             {}, -1, {}});
                    else if (f >= 420 && f < 444)
                        screen.set_activity({id, 4, 0, 0, {}, -1, {}});
                    else if (f == 444)
                    {
                        screen.set_activity({});
                        store::system::Inventory inventory;
                        inventory.apps = {{id, "RetroArch", "01.000.000", "/data/homebrew/" + id,
                                           "", false, true, false}};
                        screen.set_inventory(std::move(inventory));
                        screen.finish_job(
                            true, false, "RetroArch installed",
                            "ShadowMountPlus adds it to your home screen in a moment.");
                    }
                }
                screen.update(input, 1.0f / 60.0f, feedback);
                if (frame % 2)
                    continue;
                screen.draw(renderer, fonts.refs);
                renderer.present(target.framebuffer(), 1920, 1080);
                glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
                glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                for (int y = 0; y < 540; ++y)
                    for (int x = 0; x < 960; ++x)
                        for (int c = 0; c < 3; ++c)
                        {
                            const auto at = [&](int dx, int dy)
                            { return pixels[((2 * y + dy) * 1920 + (2 * x + dx)) * 4 + c]; };
                            half[(y * 960 + x) * 3 + c] = static_cast<unsigned char>(
                                (at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
                        }
                char name[512];
                std::snprintf(name, sizeof(name), "%s/%05d.jpg", reel, frame / 2);
                if (!stbi_write_jpg(name, 960, 540, 3, half.data(), 90))
                    return 6;
            }
            std::printf("Reel: %d frames in %s\n", frames / 2, reel);
            glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglTerminate(display);
            return 0;
        }
        if (argc == 5 && std::string(argv[4]) == "closing")
        {
            // Circle held for most of a second: the bar is part of the way.
            hui::InputFrame press, hold;
            press.pressed = press.held = hui::action_bit(hui::Action::back);
            hold.held = hui::action_bit(hui::Action::back);
            screen.update(press, 1.0f / 60.0f, feedback);
            for (int frame = 0; frame < 54; ++frame)
                screen.update(hold, 1.0f / 60.0f, feedback);
        }
        screen.draw(renderer, fonts.refs);
        if (argc == 5 && std::string(argv[4]) == "detail-end")
        {
            hui::InputFrame scroll;
            scroll.nav = hui::Direction::down;
            for (int frame = 0; frame < 120; ++frame)
                screen.update(scroll, 1.0f / 60.0f, feedback);
            screen.draw(renderer, fonts.refs);
        }
        renderer.present(target.framebuffer(), 1920, 1080);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
        std::vector<unsigned char> pixels(1920 * 1080 * 4);
        glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        stbi_flip_vertically_on_write(1);
        if (!stbi_write_png(argv[2], 1920, 1080, 4, pixels.data(), 1920 * 4))
            return 6;
        const auto error = glGetError();
        std::printf("Snapshot: %s GL=0x%x draws=%zu\n", argv[2], error, renderer.last_draw_calls());
        if (error != GL_NO_ERROR)
            return 7;
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
