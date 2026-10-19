// Prospero BlackBox - prospero-win's game profiles: <id>.profile and its line in profiles.lst.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace store::library
{
// What prospero-win needs to start one game. Checked against prospero-win's own rules
// (pw_app_profile.c, pw_game_profile.c) before it is written, so the launcher never
// meets a profile it would refuse.
struct GameProfile
{
    std::vector<std::string> comment; // lines at the top, each written after "; "
    // [application]
    std::string id;                // [a-z0-9][a-z0-9_-]*: also names the file
    std::string name;              // shown in the launcher
    std::string executable;        // C:\Games\BLUR\Blur.exe
    std::string working_directory; // C:\Games\BLUR
    std::string dll_overrides;     // WINEDLLOVERRIDES syntax; empty for none
    std::string prefix;            // the prefix's folder under /data/prospero-win/prefixes
    std::string runtime = "wine-wow64";
    std::string architecture = "pe32"; // pe32, pe64
    std::string graphics = "dxvk";     // auto, gdi, dxvk, opengl
    // [display]
    unsigned width = 1920, height = 1080;
    std::string scaling = "fit"; // fit, integer, stretch
    bool show_fps = false;
    // [input]
    std::string input_mode = "xinput"; // xinput, keyboard
    bool touchpad_mouse = false; // the DualSense touchpad moves the pointer, its press clicks
    // [runtime]: thread_scheduling, shared_input, fast_clock = true/false; cpu = native/translator
    std::vector<std::pair<std::string, std::string>> switches;
    // [debug] env = NAME=VALUE: diagnostics variables (DXVK_HUD, ...), at most 5, as prospero-win
    // takes them (pw_game_profile.c): a known family, never one the title sets itself, and no
    // DXVK_HUD/GALLIUM_HUD with show_fps, which sets the HUD itself.
    std::vector<std::pair<std::string, std::string>> debug_env;
};

// The profile as prospero-win reads it; false with error set when a field breaks its rules.
bool profile_text(const GameProfile &profile, std::string &text, std::string &error);

// profiles.lst names the profiles the launcher shows, one file name a line. Adding one
// that's there already changes nothing (prospero-win refuses a list naming one twice);
// a new one goes after the line named after, or last. Other lines stay as they were.
bool list_add(std::string &list, const std::string &file, const std::string &after,
              std::string &error);
// Every line naming file removed; true when the list changed.
bool list_remove(std::string &list, const std::string &file);

// <profiles>/<id>.profile written (atomically) and listed in <profiles>/profiles.lst.
bool install_profile(const std::string &profiles, const GameProfile &profile,
                     const std::string &after, std::string &error);
// The file and its line removed; a profile that isn't there is no error.
bool remove_profile(const std::string &profiles, const std::string &id, std::string &error);
} // namespace store::library
