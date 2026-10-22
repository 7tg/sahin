// Prospero BlackBox - A game installed into a prefix from its base copy, mods-cache and a plan.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/save_file.hpp"
#include "library/game_install.hpp"
#include "library/wine_registry.hpp"
#include "third_party/miniz/miniz.h"
#include "third_party/picosha2/picosha2.h"
#include <cassert>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace store::library;
using namespace std::string_literals;

namespace
{
void put(const fs::path &path, const std::string &body)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << body;
}

// Whether folder holds an entry spelled exactly name: on a file system that ignores case
// (macOS's), fs::exists would find "data" when asked for "DATA".
bool spelled(const fs::path &folder, const std::string &name)
{
    for (const auto &entry : fs::directory_iterator(folder))
        if (entry.path().filename() == name)
            return true;
    return false;
}

std::string read(const fs::path &path)
{
    std::string text;
    assert(hui::save::read_file(path.string(), &text, 64u << 20));
    return text;
}

std::uint32_t crc(const std::string &body)
{
    return static_cast<std::uint32_t>(
        mz_crc32(mz_crc32(0, nullptr, 0), reinterpret_cast<const unsigned char *>(body.data()),
                 body.size()));
}

std::string sha(const std::string &body)
{
    return picosha2::hash256_hex_string(body);
}

// A file put in mods-cache under its hash, as the owner (or a download) would.
void cache(const fs::path &data, const std::string &body, const std::string &ext = "")
{
    put(data / "mods-cache" / (sha(body) + ext), body);
}

std::string zip(const std::vector<std::pair<std::string, std::string>> &members)
{
    char path[] = "/tmp/prospero-zip-XXXXXX";
    const int fd = mkstemp(path);
    close(fd);
    unlink(path);
    for (const auto &[name, body] : members)
        assert(mz_zip_add_mem_to_archive_file_in_place(path, name.c_str(), body.data(), body.size(),
                                                       nullptr, 0, MZ_DEFAULT_COMPRESSION));
    auto bytes = read(path);
    unlink(path);
    return bytes;
}

const char kUserReg[] = "WINE REGISTRY Version 2\n"
                        ";; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n"
                        "#arch=win64\n\n"
                        "[Software\\\\Game Co\\\\Game] 1791414901\n"
                        "#time=1dd56b1adfaa5ca\n"
                        "@=\"KEEP-THIS-KEY\"\n"
                        "\"Resolution\"=dword:87800438\n\n";
const char kSystemReg[] = "WINE REGISTRY Version 2\n"
                          ";; All keys relative to REGISTRY\\\\Machine\n\n"
                          "#arch=win64\n\n";
} // namespace

int main()
{
    char temporary[] = "/tmp/prospero-install-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path root = temporary, data = root / "data", win = root / "win";
    const InstallPaths paths{data.string(), win.string()};
    const std::atomic<bool> go{false};
    std::string error;

    // Blur's own plan, as shipped, reads and checks out.
    InstallPlan blur;
    assert(load_plan(STORE_ASSETS "/plans/BLUR.json", blur, error));
    assert(blur.game == "BLUR" && blur.release == "1.2" && blur.prefix == "sahin" &&
           blur.profile.id == "pb-blur" && blur.profile.width == 3840 && blur.pending.empty() &&
           blur.game_files.size() == 4 && blur.game_files[0].to == "dsound.dll" &&
           blur.options.size() == 2 && blur.option("online") && blur.upgrades.empty());
    // The PlayStation button prompts: carried in the app's bundle, so their source has no url.
    assert(blur.game_files[1].source == "playstation-prompts" &&
           blur.game_files[1].to == "scripts/DS2TexInject.asi" && blur.game_files[3].to == "texmod/" &&
           blur.source("playstation-prompts") && blur.source("playstation-prompts")->url.empty());
    // What earlier mod installs left behind is stripped from the copy.
    assert(blur.strip.size() == 14 && blur.strip[0] == "dsound.dll" && blur.strip[3] == "scripts");
    // The intro movies go too, so Blur starts at its title screen.
    assert(blur.strip[11] == "UI/movies/atvi.bik" && blur.strip[13] == "UI/movies/BlurAttract.bik");
    // Blur keeps its picture size in its own settings.xml once the player saves the Graphics
    // page; the plan keeps it to the page's Resolution.
    assert(blur.user_files.size() == 1 &&
           blur.user_files[0].file == "AppData/Roaming/Bizarre Creations/Blur/settings.xml" &&
           blur.user_files[0].element == "shared_graphical_settings" &&
           blur.user_files[0].attributes ==
               (std::vector<std::pair<std::string, std::string>>{{"width", "{width}"}, {"height", "{height}"}}));
    // A save the player puts in saves/Blur is where Blur keeps its own.
    assert(blur.user_seeds.size() == 1 && blur.user_seeds[0].from == "blursave" &&
           blur.user_seeds[0].file == "AppData/Roaming/Bizarre Creations/Blur/save/blursave");
    std::string profile;
    assert(profile_text(blur.profile, profile, error));

    // A small game of the same shape: releases told by data.bin, 2.0 current.
    const std::string current(3000, 'c'), older(2000, 'o');
    const char *folders[] = {"DATA"};
    const char *files[] = {"game.exe", "DATA/data.bin"};
    const Release releases[] = {{"2.0", current.size(), crc(current), false},
                                {"1.0", older.size(), crc(older), true}};
    const char *mod_files[] = {"scripts"};
    Manifest game{"GAME", folders, files, "DATA/data.bin", releases, mod_files};
    game.mods_left_out = true; // a copy with mods in it is installed from, without them

    // Mod files: a fixed exe, a zip with a loader and a scripts folder, a system DLL.
    const std::string exe = "fixed exe", loader = "loader dll", asi = "plugin",
                      ini = "[MAIN]\r\nFixHUD = 1        ; Corrects the HUD.\r\nSimRate = 0\r\nOther = x\r\n",
                      dll = "native d3dx";
    const std::string mods = zip({{"dinput8.dll", loader},
                                  {"README.md", "readme"},
                                  {"scripts/Mod.asi", asi},
                                  {"scripts/Mod.ini", ini}});
    const auto source = [](const std::string &id, const std::string &body, const char *extra = "")
    {
        return "\"" + id + "\": {\"name\": \"" + id + "\", \"sha256\": \"" + sha(body) +
               "\", \"size\": " + std::to_string(body.size()) + extra + "}";
    };
    // Our upgrade from 1.0 to 2.0: data.bin rewritten, extra.bin built from its old bytes,
    // a file new in 2.0 in a folder 1.0 lacks, a file 2.0 no longer has.
    const auto u64 = [](std::uint64_t value)
    {
        std::string out;
        for (int i = 0; i < 8; ++i)
            out += static_cast<char>(value >> (8 * i));
        return out;
    };
    const std::string upgrade = zip(
        {{"upgrade.json",
          "{\"schema\": 1, \"game\": \"GAME\", \"from\": \"1.0\", \"to\": \"2.0\", \"files\": ["
          "{\"path\": \"DATA/data.bin\", \"from_sha256\": \"" + sha(older) + "\", \"sha256\": \"" +
              sha(current) + "\", \"ops\": \"ops/0\"},"
          "{\"path\": \"DATA/extra.bin\", \"from_sha256\": \"" + sha("abcdef") + "\", \"sha256\": \"" +
              sha("cdefXY") + "\", \"ops\": \"ops/1\"},"
          "{\"path\": \"Support/new/readme.txt\", \"sha256\": \"" + sha("new in 2.0") +
              "\", \"ops\": \"ops/2\"}], \"remove\": [\"DATA/gone.txt\"]}"},
         {"ops/0", "A" + u64(3000) + current},
         {"ops/1", "C" + u64(2) + u64(4) + "A" + u64(2) + "XY"},
         {"ops/2", "A" + u64(10) + "new in 2.0"}});
    const std::string plan_text =
        "{\"schema\": 1, \"game\": \"GAME\", \"title\": \"The Game\", \"release\": \"2.0\","
        " \"prefix\": \"shared\", \"sources\": {" +
        source("exe", exe) + ", " +
        source("mods", mods, ", \"url\": \"https://example.com/mods.zip\"") + ", " +
        source("dll", dll) + ", " + source("up", upgrade) +
        "}, \"game_files\": ["
        "{\"source\": \"exe\", \"to\": \"game.exe\"},"
        "{\"source\": \"mods\", \"member\": \"dinput8.dll\", \"to\": \"dinput8.dll\"},"
        "{\"source\": \"mods\", \"member\": \"scripts/\", \"to\": \"scripts/\"},"
        "{\"source\": \"exe\", \"to\": \"data/extra/fixed.bin\"}],"
        " \"system_files\": [{\"source\": \"dll\", \"to\": \"syswow64/d3dx9_43.dll\","
        " \"keep_builtin\": true}],"
        " \"texts\": [{\"to\": \"dxvk.conf\", \"body\": \"d3d9.cachedDynamicBuffers = True\\n\"},"
        " {\"to\": \"scripts/Mod.ini\", \"body\": \"[extra]\\nhrtf = false\\n\", \"append\": true}],"
        " \"patches\": [{\"file\": \"game.exe\", \"offset\": 6, \"from\": \"6578\", \"to\": \"4558\"}],"
        " \"expect\": {\"game.exe\": \"" + sha("fixed EXe") + "\"},"
        " \"ini\": [{\"file\": \"SCRIPTS/mod.ini\", \"values\": {\"FixHUD\": \"0\", \"SimRate\": \"-1\"},"
        " \"lf\": true}],"
        " \"registry\": {\"machine\": [{\"key\": \"Software\\\\Wow6432Node\\\\Game Co\\\\Game\","
        " \"name\": \"Install Dir\", \"string\": \"C:\\\\Games\\\\GAME\\\\\"}],"
        " \"user\": [{\"key\": \"Software\\\\Game Co\\\\Game\", \"name\": \"Resolution\","
        " \"dword\": 2399144048}]},"
        " \"profile\": {\"id\": \"the-game\", \"name\": \"The Game\","
        " \"executable\": \"C:\\\\Games\\\\GAME\\\\game.exe\", \"working_directory\": \"C:\\\\Games\\\\GAME\","
        " \"dll_overrides\": \"dinput8=n,b\", \"width\": 1920, \"height\": 1080,"
        " \"runtime\": {\"fast_clock\": \"true\"}, \"after\": \"other.profile\"},"
        " \"upgrades\": [{\"from\": \"1.0\", \"source\": \"up\"}],"
        " \"strip\": [\"dsound.dll\", \"scripts\", \"amax\"],"
        " \"pending\": [\"A mod with no source yet\"],"
        " \"user_files\": [{\"file\": \"AppData/Roaming/Game Co/settings.xml\","
        " \"element\": \"video\", \"attributes\": {\"width\": \"{width}\", \"height\": \"{height}\","
        " \"mode\": \"full\"}}],"
        " \"user_seeds\": [{\"from\": \"save.dat\", \"file\": \"Saved Games/Game Co/save.dat\"}]}";
    InstallPlan plan;
    assert(parse_plan(plan_text, plan, error));
    assert(plan.pending.size() == 1 && plan.source("mods")->url == "https://example.com/mods.zip");
    assert(plan.strip.size() == 3 && plan.strip[1] == "scripts");
    assert(plan.user_files.size() == 1 && plan.user_files[0].element == "video" &&
           plan.user_files[0].attributes.size() == 3 &&
           plan.user_files[0].attributes[0] == std::make_pair("width"s, "{width}"s));
    assert(plan.user_seeds.size() == 1 && plan.user_seeds[0].from == "save.dat" &&
           plan.user_seeds[0].file == "Saved Games/Game Co/save.dat");

    // Plans that would write outside their folders, or name what isn't there, are refused.
    const auto refused = [&](std::string from, std::string to)
    {
        auto text = plan_text;
        const auto at = text.find(from);
        assert(at != std::string::npos);
        text.replace(at, from.size(), to);
        InstallPlan bad;
        return !parse_plan(text, bad, error) && !error.empty();
    };
    assert(refused("\"to\": \"game.exe\"", "\"to\": \"../game.exe\""));
    assert(refused("\"to\": \"game.exe\"", "\"to\": \"/etc/passwd\""));
    assert(refused("\"to\": \"game.exe\"", "\"to\": \"a\\\\b.exe\""));
    assert(refused("\"member\": \"scripts/\"", "\"member\": \"../\""));
    assert(refused("{\"source\": \"exe\", \"to\": \"game.exe\"}", "{\"source\": \"nope\", \"to\": \"x\"}"));
    assert(refused("\"schema\": 1", "\"schema\": 2"));
    assert(refused("\"strip\": [\"dsound.dll\"", "\"strip\": [\"../x\"") && error.find("strip") != std::string::npos);
    assert(refused("\"strip\": [\"dsound.dll\"", "\"strip\": [3") && error.find("strip") != std::string::npos);
    assert(refused("\"source\": \"up\"}", "\"source\": \"up\", \"kind\": \"bsdiff\"}"));
    assert(!plan.upgrades[0].rtpatch);
    {
        auto text = plan_text;
        text.replace(text.find("\"source\": \"up\"}"), 15, "\"source\": \"up\", \"kind\": \"rtpatch\"}");
        InstallPlan with_patch;
        assert(parse_plan(text, with_patch, error) && with_patch.upgrades[0].rtpatch &&
               with_patch.upgrades[0].from == "1.0" && with_patch.upgrades[0].source == "up");
    }
    assert(refused("\"prefix\": \"shared\"", "\"prefix\": \"../shared\""));
    assert(refused("https://example.com", "http://example.com"));
    assert(refused("\"width\": 1920", "\"width\": 99"));
    assert(refused(sha(dll), "ABC"));
    assert(refused("\"to\": \"4558\"", "\"to\": \"45\""));          // not the same length
    assert(refused("\"from\": \"6578\"", "\"from\": \"zz\""));       // not hex
    assert(refused("\"expect\": {\"game.exe\": \"", "\"expect\": {\"../game.exe\": \""));
    // User files: plain paths, a plain element name, values that can't end the attribute, and
    // only the placeholders it knows.
    assert(refused("\"AppData/Roaming", "\"../AppData/Roaming") && error.find("user_files") != std::string::npos);
    assert(refused("\"AppData/Roaming", "\"/AppData/Roaming"));
    assert(refused("\"from\": \"save.dat\"", "\"from\": \"../save.dat\"") &&
           error.find("user_seeds") != std::string::npos);
    assert(refused("\"Saved Games/Game Co/save.dat\"", "\"Saved Games/../../save.dat\""));
    assert(refused("\"element\": \"video\"", "\"element\": \"vid eo\""));
    assert(refused("\"element\": \"video\"", "\"element\": \"\""));
    assert(refused("\"element\": \"video\", ", ""));
    assert(refused("\"mode\": \"full\"", "\"mode\": \"a\\\"b\""));
    assert(refused("\"mode\": \"full\"", "\"mode\": \"<b\""));
    assert(refused("\"mode\": \"full\"", "\"mode\": \"{depth}\""));
    assert(refused("\"mode\": \"full\"", "\"m o\": \"full\""));
    assert(refused("\"mode\": \"full\"", "\"mode\": 3"));
    assert(refused("\"attributes\": {", "\"attributes\": {}, \"x\": {"));
    assert(!parse_plan("[]", plan, error) && plan.game == "GAME"); // nothing changed

    // The base copy (its names in other cases), the prefix and its registry, a save.
    const fs::path base = data / "base-games" / "GAME";
    put(base / "Game.exe", "original exe");
    put(base / "data" / "data.bin", current);
    const fs::path prefix = win / "prefixes" / "shared";
    put(prefix / "system.reg", kSystemReg);
    put(prefix / "user.reg", kUserReg);
    put(prefix / "drive_c" / "windows" / "syswow64" / "d3dx9_43.dll", "wine builtin");
    put(prefix / "drive_c" / "users" / "me" / "Documents" / "save.dat", "my save");
    put(win / "profiles" / "profiles.lst", "other.profile\nlast.profile\n");

    InstallOptions options;
    options.now = 1791500000;
    options.app_version = "01.000.001";
    std::vector<std::string> steps;
    std::uint64_t adding_done = 0, adding_total = 0;
    const auto progress = [&](const InstallStatus &status)
    {
        assert(status.total == 0 || status.done <= status.total);
        if (steps.empty() || steps.back() != status.step)
        {
            steps.push_back(status.step);
            if (status.step == "Adding the mods")
                adding_done = 0;
        }
        if (status.step == "Adding the mods")
        {
            assert(status.done >= adding_done); // only rising within the step
            adding_done = status.done;
            adding_total = status.total;
        }
    };
    const auto installed = prefix / "drive_c" / "Games" / "GAME";

    // Pending pieces stop it first, then sources missing from mods-cache (listed).
    assert(!install_game(plan, game, paths, options, progress, go, error));
    assert(error.find("A mod with no source yet") != std::string::npos);
    options.without_pending = true;
    cache(data, exe);
    assert(!install_game(plan, game, paths, options, progress, go, error));
    assert(error.find("lacks 2 files") != std::string::npos &&
           error.find(sha(mods)) != std::string::npos && !fs::exists(installed));
    assert(missing_sources(plan, (data / "mods-cache").string(), go).size() == 2);
    // A cached file whose bytes don't match its name is not used.
    put(data / "mods-cache" / (sha(mods) + ".zip"), "corrupt");
    cache(data, dll, ".dll");
    assert(missing_sources(plan, (data / "mods-cache").string(), go).size() == 1);
    fs::remove(data / "mods-cache" / (sha(mods) + ".zip"));
    cache(data, mods, ".zip");
    assert(missing_sources(plan, (data / "mods-cache").string(), go).empty());

    // An older base copy: without an upgrade from it, refused; with one, its copy is
    // upgraded as it installs (the base copy itself stays 1.0).
    put(base / "data" / "data.bin", older);
    put(base / "data" / "extra.bin", "abcdef");
    put(base / "data" / "gone.txt", "1.0 only");
    {
        auto none = plan;
        none.upgrades.clear();
        assert(!install_game(none, game, paths, options, progress, go, error));
        assert(error.find("version 1.0, and there's no upgrade") != std::string::npos);
    }
    assert(!install_game(plan, game, paths, options, progress, go, error)); // not in mods-cache
    assert(error.find("lacks 1 file") != std::string::npos && error.find(sha(upgrade)) != std::string::npos);
    cache(data, upgrade, ".zip");
    steps.clear();
    assert(install_game(plan, game, paths, options, progress, go, error));
    assert(std::find(steps.begin(), steps.end(), "Upgrading to 2.0") != steps.end());
    // Adding the mods counts the bytes it writes (the files and the zips' members), to the end.
    assert(adding_total > 0 && adding_done == adding_total);
    assert(read(installed / "data" / "data.bin") == current && read(installed / "data" / "extra.bin") == "cdefXY" &&
           read(installed / "Support" / "new" / "readme.txt") == "new in 2.0" &&
           !fs::exists(installed / "data" / "gone.txt") && !spelled(installed, "DATA"));
    assert(read(data / "installs" / "shared" / "GAME.json").find("\"base_release\": \"1.0\"") !=
           std::string::npos);
    assert(read(base / "data" / "data.bin") == older); // the base copy is never changed
    // An upgrade for other bytes is refused, and the install before it stays.
    put(base / "data" / "extra.bin", "abcdeZ");
    assert(!install_game(plan, game, paths, options, progress, go, error));
    assert(error.find("isn't the 1.0 file") != std::string::npos &&
           read(installed / "data" / "extra.bin") == "cdefXY");
    // From here on, a 2.0 copy.
    put(base / "data" / "data.bin", current);
    fs::remove(base / "data" / "extra.bin");
    fs::remove(base / "data" / "gone.txt");

    // Mods an earlier install left in the user's copy are left out of the install (the plan's
    // strip list, in any case), and the copy itself stays as it is.
    put(base / "Dsound.dll", "old mod loader");
    put(base / "scripts" / "a.asi", "old plugin");
    put(base / "AMAX" / "x" / "y.dll", "old client"); // a folder, in other case, with a subfolder
    assert(install_game(plan, game, paths, options, progress, go, error));
    assert(!fs::exists(installed / "dsound.dll") && !fs::exists(installed / "Dsound.dll"));
    assert(!fs::exists(installed / "scripts" / "a.asi") &&
           read(installed / "scripts" / "Mod.asi") == asi); // the plan's own scripts are there
    assert(read(base / "scripts" / "a.asi") == "old plugin" && read(base / "Dsound.dll") == "old mod loader");
    assert(!spelled(installed, "AMAX") && !fs::exists(installed / "amax") && read(base / "AMAX" / "x" / "y.dll") == "old client");
    fs::remove_all(base / "AMAX");
    fs::remove(base / "Dsound.dll");
    fs::remove_all(base / "scripts");

    // The install.
    steps.clear();
    assert(install_game(plan, game, paths, options, progress, go, error));
    assert(steps.front() == "Checking the base copy" && steps.back() == "Installed");
    assert(read(installed / "Game.exe") == "fixed EXe"); // patched; the base copy's spelling kept
    assert(!fs::exists(installed / "game.exe") || fs::equivalent(installed / "game.exe", installed / "Game.exe"));
    assert(read(installed / "dinput8.dll") == loader && read(installed / "scripts" / "Mod.asi") == asi);
    assert(!fs::exists(installed / "README.md"));
    assert(read(installed / "data" / "extra" / "fixed.bin") == exe); // into "data", not a new "DATA"
    // Edited, its line ends made \n, then the appended block.
    assert(read(installed / "scripts" / "Mod.ini") ==
           "[MAIN]\nFixHUD = 0        ; Corrects the HUD.\nSimRate = -1\nOther = x\n[extra]\nhrtf = false\n");
    assert(read(installed / "dxvk.conf") == "d3d9.cachedDynamicBuffers = True\n");
    const auto syswow64 = prefix / "drive_c" / "windows" / "syswow64";
    assert(read(syswow64 / "d3dx9_43.dll") == dll && read(syswow64 / "d3dx9_43.dll.builtin") == "wine builtin");
    assert(!fs::exists(prefix / ".blackbox-staging"));
    WineRegistry registry;
    assert(load_registry((prefix / "user.reg").string(), registry, error));
    assert(registry.dword("Software\\Game Co\\Game", "Resolution") == 0x8f000870u &&
           registry.string_value("Software\\Game Co\\Game", "") == "KEEP-THIS-KEY");
    assert(load_registry((prefix / "system.reg").string(), registry, error));
    assert(registry.string_value("Software\\Wow6432Node\\Game Co\\Game", "Install Dir") ==
           "C:\\Games\\GAME\\");
    assert(read(win / "profiles" / "profiles.lst") == "other.profile\nthe-game.profile\nlast.profile\n");
    assert(read(win / "profiles" / "the-game.profile").find("prefix = shared\n") != std::string::npos);
    const auto record = read(data / "installs" / "shared" / "GAME.json");
    assert(record.find("\"release\": \"2.0\"") != std::string::npos &&
           record.find("\"without\": [\"A mod with no source yet\"]") != std::string::npos &&
           record.find(sha(mods)) != std::string::npos);
    assert(read(base / "Game.exe") == "original exe"); // the base copy is never changed

    // Again over it (a repair): the game's folder is rebuilt, Wine's own DLL stays kept
    // once, the list names the game once, the save is untouched.
    put(installed / "scripts" / "stray.asi", "left by hand");
    assert(install_game(plan, game, paths, options, progress, go, error));
    assert(!fs::exists(installed / "scripts" / "stray.asi"));
    assert(read(syswow64 / "d3dx9_43.dll.builtin") == "wine builtin");
    assert(read(win / "profiles" / "profiles.lst") == "other.profile\nthe-game.profile\nlast.profile\n");
    assert(read(prefix / "drive_c" / "users" / "me" / "Documents" / "save.dat") == "my save");
    assert(!fs::exists(installed.string() + ".installing") && !fs::exists(installed.string() + ".old"));

    // A patch meant for other bytes, or a result that isn't what the plan expects, stops
    // the install before the new folder takes the old one's place.
    {
        InstallPlan other = plan;
        other.patches[0].from = "zz";
        assert(!install_game(other, game, paths, options, progress, go, error));
        assert(error.find("not the file its patch is for") != std::string::npos);
        other = plan;
        other.expect[0].second = sha("something else");
        assert(!install_game(other, game, paths, options, progress, go, error));
        assert(error.find("didn't come out as it should") != std::string::npos);
        assert(read(installed / "Game.exe") == "fixed EXe" && !fs::exists(installed.string() + ".installing"));
    }

    // A cancelled install leaves the one before it.
    const std::atomic<bool> stop{true};
    assert(!install_game(plan, game, paths, options, progress, stop, error));
    assert(error == "Cancelled" && read(installed / "dinput8.dll") == loader &&
           !fs::exists(installed.string() + ".installing"));

    // A plan option (MW's Xbox 360 look): chosen, its files, edits, expected hash and DLL
    // overrides are added; not chosen, none of it, and its source isn't needed.
    const std::string extra = "extra plugin";
    auto option_text = plan_text;
    option_text.insert(option_text.find("\"up\": {"), source("opt", extra) + ", ");
    option_text.insert(option_text.size() - 1,
                       ", \"options\": [{\"id\": \"extra\", \"title\": \"Extra look\","
                       " \"description\": \"More.\", \"game_files\": [{\"source\": \"opt\","
                       " \"to\": \"scripts/Extra.asi\"}], \"texts\": [{\"to\": \"extra.cfg\","
                       " \"body\": \"on\\n\"}], \"ini\": [{\"file\": \"scripts/Mod.ini\","
                       " \"values\": {\"Other\": \"y\"}}], \"expect\": {\"extra.cfg\": \"" +
                           sha("on\n") + "\"}, \"dll_overrides\": \";d3dx9_26=n\"}]");
    InstallPlan optional;
    assert(parse_plan(option_text, optional, error));
    assert(optional.options.size() == 1 && optional.option("extra") && !optional.options[0].on &&
           optional.options[0].game_files.size() == 1 && optional.options[0].expect.size() == 1);
    {
        auto bad = option_text;
        bad.replace(bad.find("{\"source\": \"opt\""), 16, "{\"source\": \"nope\"");
        InstallPlan refused_plan;
        assert(!parse_plan(bad, refused_plan, error) && error.find("nope") != std::string::npos);
        bad = option_text;
        bad.insert(bad.size() - 2, ", {\"id\": \"extra\", \"title\": \"Again\"}");
        assert(!parse_plan(bad, refused_plan, error) && error.find("named twice") != std::string::npos);
    }
    auto views = option_views(optional, paths, options);
    assert(views.size() == 1 && views[0].id == "extra" && views[0].title == "Extra look" &&
           !views[0].on); // the record before options: off
    InstallOptions with = options;
    with.chosen = {"extra", "unknown"};
    const auto chosen_plan = effective_plan(optional, with);
    assert(chosen_plan.options.empty() && chosen_plan.game_files.size() == plan.game_files.size() + 1 &&
           chosen_plan.profile.dll_overrides == "dinput8=n,b;d3dx9_26=n" &&
           chosen_plan.expect.size() == plan.expect.size() + 1);
    assert(missing_sources(chosen_plan, (data / "mods-cache").string(), go).size() == 1);
    assert(missing_sources(effective_plan(optional, options), (data / "mods-cache").string(), go).empty());
    cache(data, extra, ".asi");
    assert(install_game(optional, game, paths, with, progress, go, error));
    assert(read(installed / "scripts" / "Extra.asi") == extra && read(installed / "extra.cfg") == "on\n" &&
           read(installed / "scripts" / "Mod.ini").find("Other = y") != std::string::npos);
    assert(read(win / "profiles" / "the-game.profile").find("d3dx9_26=n") != std::string::npos);
    assert(read(data / "installs" / "shared" / "GAME.json").find("\"options\": [\"extra\"]") !=
           std::string::npos);
    views = option_views(optional, paths, options);
    assert(views.size() == 1 && views[0].on);
    // Repaired without it: its files go, the record says so.
    assert(install_game(optional, game, paths, options, progress, go, error));
    assert(!fs::exists(installed / "scripts" / "Extra.asi") && !fs::exists(installed / "extra.cfg"));
    assert(read(win / "profiles" / "the-game.profile").find("d3dx9_26=n") == std::string::npos);
    assert(read(data / "installs" / "shared" / "GAME.json").find("\"options\": []") != std::string::npos);
    assert(!option_views(optional, paths, options)[0].on);

    // The performance overlay goes into the profile: the counter as show_fps, the full one
    // as DXVK_HUD (prospero-win refuses both together).
    {
        InstallOptions fps = options, full = options;
        fps.overlay = Overlay::fps;
        full.overlay = Overlay::full;
        assert(effective_plan(plan, fps).profile.show_fps && effective_plan(plan, fps).profile.debug_env.empty());
        const auto hud = effective_plan(plan, full).profile;
        assert(!hud.show_fps && hud.debug_env.size() == 1 && hud.debug_env[0].first == "DXVK_HUD");
        assert(overlay_named(overlay_name(Overlay::full)) == Overlay::full && overlay_named("x") == Overlay::off);
        assert(install_game(plan, game, paths, full, progress, go, error));
        assert(read(win / "profiles" / "the-game.profile").find("env = DXVK_HUD=fps,frametimes") != std::string::npos);
    }

    // The picture size (the page's Resolution) goes into the profile; unset, the plan's.
    {
        InstallOptions sized = options;
        sized.width = 1920;
        sized.height = 1080;
        auto big = plan;
        big.profile.width = 3840;
        big.profile.height = 2160;
        assert(effective_plan(big, options).profile.width == 3840 &&
               effective_plan(big, options).profile.height == 2160);
        const auto chosen = effective_plan(big, sized).profile;
        assert(chosen.width == 1920 && chosen.height == 1080);
        assert(install_game(big, game, paths, sized, progress, go, error));
        assert(read(win / "profiles" / "the-game.profile").find("desktop = 1920x1080") != std::string::npos);
        sized.width = 2560;
        sized.height = 1440;
        assert(install_game(big, game, paths, sized, progress, go, error));
        assert(read(win / "profiles" / "the-game.profile").find("desktop = 2560x1440") != std::string::npos);
        assert(install_game(big, game, paths, options, progress, go, error));
        assert(read(win / "profiles" / "the-game.profile").find("desktop = 3840x2160") != std::string::npos);
        assert(resolution_named(resolution_name({2560, 1440})).width == 2560 &&
               resolution_named("2560x1440").height == 1440 && resolution_named("800x600").width == 0 &&
               resolution_named("").width == 0);
    }

    // A settings file the game keeps in the Windows user's folder: once it's there, the
    // install and a settings change set the element's attributes to the profile's size; only
    // those values change. Where it isn't, nothing is written; Public and folders reached
    // through a link are left alone.
    {
        const auto users = prefix / "drive_c" / "users";
        const auto settings = users / "me" / "AppData" / "Roaming" / "Game Co" / "settings.xml";
        const std::string before =
            "<?xml version=\"1.0\"?>\r\n<application_settings version=\"1\"/>\r\n"
            "<!-- <video width=\"1\"/> -->\r\n<videos width=\"7\"/>\r\n"
            "<video  height = '1440' x=\"0\" width=\"2560\" windowed=\"0\"\tvsync=\"1\"/>\r\n";
        put(settings, before);
        put(users / "Public" / "AppData" / "Roaming" / "Game Co" / "settings.xml", "<video width=\"5\"/>");
        const auto outside = root / "outside";
        put(outside / "AppData" / "Roaming" / "Game Co" / "settings.xml", "<video width=\"5\"/>");
        fs::create_directory_symlink(outside, users / "linked");
        InstallOptions sized = options;
        sized.width = 3840;
        sized.height = 2160;
        assert(install_game(plan, game, paths, sized, progress, go, error));
        const std::string after =
            "<?xml version=\"1.0\"?>\r\n<application_settings version=\"1\"/>\r\n"
            "<!-- <video width=\"1\"/> -->\r\n<videos width=\"7\"/>\r\n"
            "<video  height = '2160' x=\"0\" width=\"3840\" windowed=\"0\"\tvsync=\"1\" mode=\"full\"/>\r\n";
        assert(read(settings) == after);
        assert(read(users / "Public" / "AppData" / "Roaming" / "Game Co" / "settings.xml") == "<video width=\"5\"/>");
        assert(read(outside / "AppData" / "Roaming" / "Game Co" / "settings.xml") == "<video width=\"5\"/>");
        // Applied again on its own (a settings change), with the plan's size.
        auto chosen = effective_plan(plan, options);
        assert(apply_user_files(chosen, paths, error));
        assert(read(settings) == "<?xml version=\"1.0\"?>\r\n<application_settings version=\"1\"/>\r\n"
                                 "<!-- <video width=\"1\"/> -->\r\n<videos width=\"7\"/>\r\n"
                                 "<video  height = '1080' x=\"0\" width=\"1920\" windowed=\"0\"\tvsync=\"1\" mode=\"full\"/>\r\n");
        // A file without the element is left as it is.
        put(settings, "<other/>\n");
        assert(apply_user_files(chosen, paths, error) && read(settings) == "<other/>\n");
        // No file: none is made.
        fs::remove(settings);
        assert(apply_user_files(chosen, paths, error) && !fs::exists(settings));
        assert(install_game(plan, game, paths, sized, progress, go, error) && !fs::exists(settings));
        // An element that ends in a closing tag rather than "/>" takes the attributes too.
        put(settings, "<video width=\"1\">\n</video>\n");
        assert(apply_user_files(chosen, paths, error));
        assert(read(settings) == "<video width=\"1920\" height=\"1080\" mode=\"full\">\n</video>\n");
        fs::remove(users / "linked");
        fs::remove_all(outside);
        fs::remove_all(users / "Public");
        fs::remove_all(users / "me" / "AppData");
    }

    // A save the player keeps in saves/<game> (any case): copied into each Windows user's folder
    // on install, folders made on the way, only where that user has none yet; never into Public.
    {
        const auto users = prefix / "drive_c" / "users";
        const auto save = users / "me" / "Saved Games" / "Game Co" / "save.dat";
        fs::create_directories(users / "me");
        assert(install_game(plan, game, paths, options, progress, go, error) && !fs::exists(save));
        put(data / "saves" / "game" / "save.dat", "finished");
        put(users / "Public" / "keep", "");
        assert(install_game(plan, game, paths, options, progress, go, error));
        assert(read(save) == "finished" && !fs::exists(users / "Public" / "Saved Games"));
        put(save, "played on");
        assert(install_game(plan, game, paths, options, progress, go, error) && read(save) == "played on");
        fs::remove_all(users / "me" / "Saved Games");
        fs::remove_all(users / "Public");
        fs::remove_all(data / "saves");
    }

    // The touchpad as the mouse (the default now): the plan's touchpad ini edits after its
    // own, and the profile's input; off, neither.
    {
        auto touch = plan;
        touch.touchpad_ini = {{"scripts/Input.ini", {{"Cursor", "0"}}, false}};
        InstallOptions pad = options, none = options;
        none.cursor = Cursor::off;
        assert(InstallOptions{}.cursor == Cursor::touchpad);
        const auto on = effective_plan(touch, pad), off = effective_plan(touch, none);
        assert(on.ini.size() == plan.ini.size() + 1 && on.ini.back().file == "scripts/Input.ini" &&
               on.profile.touchpad_mouse && on.touchpad_ini.empty());
        assert(off.ini.size() == plan.ini.size() && !off.profile.touchpad_mouse);
        assert(cursor_named(cursor_name(Cursor::touchpad)) == Cursor::touchpad && cursor_named("x") == Cursor::off);
        InstallPlan elsewhere = plan;
        elsewhere.prefix = "nowhere";
        assert(!apply_ini(elsewhere, paths, error) && !error.empty());
        error.clear();
    }

    // A test install beside it: its own prefix, profile and record.
    put(win / "prefixes" / "pb-test" / "system.reg", kSystemReg);
    put(win / "prefixes" / "pb-test" / "user.reg", kUserReg);
    fs::create_directories(win / "prefixes" / "pb-test" / "drive_c" / "windows");
    InstallOptions test = options;
    test.prefix = "pb-test";
    test.profile_id = "pb-test";
    test.profile_name = "The Game (test)";
    assert(install_game(plan, game, paths, test, progress, go, error));
    assert(fs::exists(win / "prefixes" / "pb-test" / "drive_c" / "Games" / "GAME" / "dinput8.dll"));
    assert(read(win / "profiles" / "pb-test.profile").find("prefix = pb-test\n") != std::string::npos);
    assert(fs::exists(data / "installs" / "pb-test" / "GAME.json"));
    test.profile_id = "Bad Id";
    assert(!install_game(plan, game, paths, test, progress, go, error));

    // No prefix: nothing to install into.
    InstallOptions elsewhere = options;
    elsewhere.prefix = "missing";
    assert(!install_game(plan, game, paths, elsewhere, progress, go, error));
    assert(error.find("isn't there") != std::string::npos);

    // Uninstalled: the folder, profile and record go; the prefix's files and the save stay.
    assert(uninstall_game(plan, paths, options, error));
    assert(!fs::exists(installed) && !fs::exists(win / "profiles" / "the-game.profile") &&
           !fs::exists(data / "installs" / "shared" / "GAME.json"));
    assert(read(win / "profiles" / "profiles.lst") == "other.profile\npb-test.profile\nlast.profile\n");
    assert(read(syswow64 / "d3dx9_43.dll") == dll);
    assert(read(prefix / "drive_c" / "users" / "me" / "Documents" / "save.dat") == "my save");
    assert(fs::exists(win / "prefixes" / "pb-test" / "drive_c" / "Games" / "GAME"));
    assert(uninstall_game(plan, paths, options, error)); // again: nothing left to do

    // The PS5's language: the marker renamed when the copy's marker offers it, else left alone.
    {
        assert(marker_language(2) == "french" && marker_language(22) == "french" &&
               marker_language(18) == "english" && marker_language(19).empty()); // Turkish: none
        const std::string note = "To change language, rename this file to either:\r\n\r\n"
                                 "english.txt\r\nfrench.txt\r\nitalian.txt\r\n";
        assert((marker_languages(note) == std::vector<std::string>{"english", "french", "italian"}));
        assert(marker_languages("readme.txt notes.txt").empty());
        InstallPlan lang = plan;
        lang.prefix = "pb-test";
        const fs::path folder = win / "prefixes" / "pb-test" / "drive_c" / "Games" / "GAME";
        std::string done;
        assert(apply_system_language(lang, paths, 2, done, error) && done == "no language file");
        put(folder / "English.txt", note);
        assert(apply_system_language(lang, paths, 2, done, error) && done == "english -> french");
        assert(spelled(folder, "french.txt") && !spelled(folder, "English.txt") &&
               read(folder / "french.txt") == note);
        assert(apply_system_language(lang, paths, 22, done, error) && done == "french already");
        assert(apply_system_language(lang, paths, 4, done, error) && spelled(folder, "french.txt") &&
               done.find("isn't one this copy has") != std::string::npos);    // German: not here
        assert(apply_system_language(lang, paths, 19, done, error) && spelled(folder, "french.txt"));
        assert(apply_system_language(lang, paths, 1, done, error) && spelled(folder, "english.txt"));
        put(folder / "italian.txt", note);   // two markers: neither is touched
        assert(apply_system_language(lang, paths, 2, done, error) && spelled(folder, "english.txt") &&
               spelled(folder, "italian.txt") && done.find("more than one") != std::string::npos);
        InstallPlan nowhere = lang;
        nowhere.prefix = "nowhere";
        assert(!apply_system_language(nowhere, paths, 2, done, error) && !error.empty());
        error.clear();
    }

    fs::remove_all(root);
    std::cout << "Game install checks passed\n";
}
