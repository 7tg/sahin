// Prospero BlackBox - Game profiles written as prospero-win reads them, listed once.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/save_file.hpp"
#include "library/game_profile.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace store::library;

namespace
{
GameProfile most_wanted()
{
    GameProfile p;
    p.comment = {"NFS Most Wanted: written by Prospero BlackBox."};
    p.id = "nfs-most-wanted-2005";
    p.name = "NFS Most Wanted";
    p.executable = "C:\\Games\\NFSMW\\speed.exe";
    p.working_directory = "C:\\Games\\NFSMW";
    p.dll_overrides = "d3d8,d3d9,d3d10core,d3d11,dxgi=n;dinput8,dsound=n,b;d3dx9_26,d3dx9_43=n";
    p.prefix = "nfs-blackbox";
    p.width = 3840;
    p.height = 2160;
    p.switches = {{"fast_clock", "true"}, {"shared_input", "true"}, {"thread_scheduling", "true"}};
    return p;
}

std::string read(const fs::path &path)
{
    std::string text;
    assert(hui::save::read_file(path.string(), &text));
    return text;
}
} // namespace

int main()
{
    // Today's console profile, without its [debug] lines.
    std::string text, error;
    assert(profile_text(most_wanted(), text, error));
    assert(text == "; NFS Most Wanted: written by Prospero BlackBox.\n"
                   "[application]\n"
                   "id = nfs-most-wanted-2005\n"
                   "name = NFS Most Wanted\n"
                   "executable = C:\\Games\\NFSMW\\speed.exe\n"
                   "working_directory = C:\\Games\\NFSMW\n"
                   "dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi=n;dinput8,dsound=n,b;"
                   "d3dx9_26,d3dx9_43=n\n"
                   "prefix = nfs-blackbox\n"
                   "runtime = wine-wow64\n"
                   "architecture = pe32\n"
                   "graphics = dxvk\n"
                   "\n[display]\ndesktop = 3840x2160\nscaling = fit\nshow_fps = false\n"
                   "\n[input]\nmode = xinput\n"
                   "\n[runtime]\nfast_clock = true\nshared_input = true\nthread_scheduling = true\n");

    // What prospero-win would refuse is never written.
    const auto refused = [&](auto change)
    {
        auto p = most_wanted();
        change(p);
        std::string out = "unchanged";
        const bool ok = profile_text(p, out, error);
        return !ok && out == "unchanged" && !error.empty();
    };
    assert(refused([](GameProfile &p) { p.id = "NFS-MW"; }));
    assert(refused([](GameProfile &p) { p.id = std::string(60, 'a'); })); // too long a file name
    assert(refused([](GameProfile &p) { p.name = "two\nlines"; }));
    assert(refused([](GameProfile &p) { p.executable = "C:/Games/NFSMW/speed.exe"; }));
    assert(refused([](GameProfile &p) { p.executable = "C:\\Games\\..\\speed.exe"; }));
    assert(refused([](GameProfile &p) { p.executable = "C:\\Games\\NFSMW\\speed.bat"; }));
    assert(refused([](GameProfile &p) { p.working_directory = "Games\\NFSMW"; }));
    assert(refused([](GameProfile &p) { p.dll_overrides = "d3d9 = n"; }));
    assert(refused([](GameProfile &p) { p.prefix = "../other"; }));
    assert(refused([](GameProfile &p) { p.graphics = "vulkan"; }));
    assert(refused([](GameProfile &p) { p.input_mode = "mouse"; }));
    assert(refused([](GameProfile &p) { p.width = 7680; }));
    assert(refused([](GameProfile &p) { p.switches.push_back({"fast_clock", "false"}); }));
    assert(refused([](GameProfile &p) { p.switches = {{"turbo", "true"}}; }));
    assert(refused([](GameProfile &p) { p.switches = {{"cpu", "fast"}}; }));
    auto plain = most_wanted();
    plain.dll_overrides.clear();
    plain.switches.clear();
    plain.comment.clear();
    assert(profile_text(plain, text, error) && text.starts_with("[application]\n") &&
           text.find("dll_overrides") == std::string::npos &&
           text.find("[runtime]") == std::string::npos && text.find("mouse =") == std::string::npos);
    plain.touchpad_mouse = true;
    assert(profile_text(plain, text, error) &&
           text.find("[input]\nmode = xinput\nmouse = touchpad\ntouchpad = mouse_left\n") != std::string::npos);

    // profiles.lst: added once, after the line asked for, other lines kept.
    std::string list = "minesweeper.profile\n# games\nnfs-most-wanted-2005.profile\npinball.profile";
    assert(list_add(list, "pb-mw.profile", "nfs-most-wanted-2005.profile", error));
    assert(list == "minesweeper.profile\n# games\nnfs-most-wanted-2005.profile\npb-mw.profile\n"
                   "pinball.profile\n");
    const auto before = list;
    assert(list_add(list, "pb-mw.profile", "", error) && list == before);
    assert(list_add(list, "  carbon.profile", "", error) == false); // not a name prospero-win lists
    assert(list_add(list, "nfs-carbon.profile", "missing.profile", error) &&
           list.ends_with("pinball.profile\nnfs-carbon.profile\n"));
    assert(list_remove(list, "pb-mw.profile") && list.find("pb-mw") == std::string::npos);
    assert(!list_remove(list, "pb-mw.profile"));
    std::string full;
    for (int i = 0; i < 32; ++i)
        full += "game" + std::to_string(i) + ".profile\n";
    assert(!list_add(full, "one-more.profile", "", error) &&
           error.find("32") != std::string::npos);
    assert(list_add(full, "game7.profile", "", error)); // already listed: nothing to add

    // On disk: the file, then its line; removed the other way round.
    char temporary[] = "/tmp/prospero-profiles-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path profiles = temporary;
    assert(install_profile(profiles.string(), most_wanted(), "minesweeper.profile", error));
    assert(read(profiles / "profiles.lst") == "nfs-most-wanted-2005.profile\n");
    assert(profile_text(most_wanted(), text, error) &&
           read(profiles / "nfs-most-wanted-2005.profile") == text);
    assert(hui::save::write_atomic((profiles / "profiles.lst").string(),
                                          "minesweeper.profile\npinball.profile\n")
               .empty());
    auto test = most_wanted();
    test.id = "pb-mw";
    test.prefix = "pb-mw";
    assert(install_profile(profiles.string(), test, "minesweeper.profile", error));
    assert(install_profile(profiles.string(), test, "minesweeper.profile", error));
    assert(read(profiles / "profiles.lst") == "minesweeper.profile\npb-mw.profile\npinball.profile\n");
    test.width = 99;
    assert(!install_profile(profiles.string(), test, "", error));
    assert(remove_profile(profiles.string(), "pb-mw", error));
    assert(!fs::exists(profiles / "pb-mw.profile") &&
           read(profiles / "profiles.lst") == "minesweeper.profile\npinball.profile\n");
    assert(remove_profile(profiles.string(), "pb-mw", error));
    fs::remove_all(profiles);
    // [debug] env: the performance overlay, as prospero-win takes it; never with show_fps.
    {
        auto p = most_wanted();
        p.debug_env = {{"DXVK_HUD", "fps,frametimes,gpuload,cs,drawcalls,submissions,scale=2.5"}};
        assert(profile_text(p, text, error) &&
               text.ends_with("\n[debug]\nenv = DXVK_HUD=fps,frametimes,gpuload,cs,drawcalls,submissions,scale=2.5\n"));
        p.show_fps = true;
        assert(!profile_text(p, text, error) && error.find("show_fps") != std::string::npos);
        p.show_fps = false;
        for (const auto &bad : std::vector<std::pair<std::string, std::string>>{
                 {"WINEDEBUG", "+all"},        // not a family prospero-win takes
                 {"PW_VK_BATCH", "0"},          // one the title sets itself
                 {"DXVK_HUD", "fps;rm"},        // a character it refuses
                 {"dxvk_hud", "fps"}})          // a lower-case name
        {
            auto q = most_wanted();
            q.debug_env = {bad};
            assert(!profile_text(q, text, error));
        }
        p.debug_env.assign(6, {"DXVK_LOG_LEVEL", "none"});
        assert(!profile_text(p, text, error)); // more than 5
    }

    std::cout << "Game profile checks passed\n";
}
