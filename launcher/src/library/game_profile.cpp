// Prospero BlackBox - prospero-win's game profiles: <id>.profile and its line in profiles.lst.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/game_profile.hpp"
#include "core/save_file.hpp"
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace store::library
{
namespace
{
// prospero-win's limits (pw_app_profile.h, pw_game_profile.h, pw_profile_catalog.h).
constexpr std::size_t kMaxBytes = 8192, kIdMax = 64, kNameMax = 96, kPathMax = 259,
                      kOverridesMax = 256, kListMax = 32, kListNameMax = 63;
constexpr unsigned kMinWidth = 320, kMinHeight = 200, kMaxWidth = 3840, kMaxHeight = 2160;

bool identifier(const std::string &text)
{
    if (text.empty() || text.size() > kIdMax ||
        !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9')))
        return false;
    for (const char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

// An absolute DOS path: a drive, backslashes, no empty or .. parts.
bool windows_path(const std::string &path)
{
    if (path.size() < 3 || path.size() > kPathMax ||
        !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) ||
        path[1] != ':' || path[2] != '\\')
        return false;
    std::size_t segment = 3;
    for (std::size_t i = 3; i <= path.size(); ++i)
    {
        const char c = i < path.size() ? path[i] : '\0';
        if (c == '/' || c == ':' || (c != '\0' && static_cast<unsigned char>(c) < 0x20))
            return false;
        if (c == '\\' || c == '\0')
        {
            const auto length = i - segment;
            if ((length == 0 && i != path.size()) ||
                (length == 2 && path[segment] == '.' && path[segment + 1] == '.'))
                return false;
            segment = i + 1;
        }
    }
    return true;
}

bool overrides(const std::string &text)
{
    if (text.size() > kOverridesMax)
        return false;
    for (const char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ',' || c == '=' || c == ';' || c == '.' || c == '_' || c == '-' || c == '*'))
            return false;
    return true;
}

bool one_line(const std::string &text)
{
    for (const char c : text)
        if (c == '\n' || c == '\r')
            return false;
    return true;
}

bool one_of(const std::string &value, std::initializer_list<const char *> choices)
{
    for (const auto *choice : choices)
        if (value == choice)
            return true;
    return false;
}

bool list_name(const std::string &file)
{
    return file.size() <= kListNameMax && file.ends_with(".profile") &&
           identifier(file.substr(0, file.size() - 8));
}

// A line of profiles.lst without its spaces, or "" for a blank or comment line.
std::string listed(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        line.remove_suffix(1);
    if (line.empty() || line.front() == '#' || line.front() == ';')
        return {};
    return std::string(line);
}

std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size())
    {
        auto end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end;
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return lines;
}

std::string joined(const std::vector<std::string> &lines)
{
    std::string text;
    for (const auto &line : lines)
        text += line + "\n";
    return text;
}
} // namespace

bool profile_text(const GameProfile &p, std::string &text, std::string &error)
{
    const auto fail = [&](std::string why)
    {
        error = "Profile " + p.id + ": " + why;
        return false;
    };
    if (!identifier(p.id) || p.id.size() + 8 > kListNameMax)
        return fail("the id must be lower-case letters, digits, - and _");
    if (p.name.empty() || p.name.size() > kNameMax || !one_line(p.name))
        return fail("the name must be one line of at most 96 characters");
    if (!windows_path(p.executable) || !p.executable.ends_with(".exe") ||
        !windows_path(p.working_directory))
        return fail("the executable and its folder must be C:\\ paths, the executable an .exe");
    if (!overrides(p.dll_overrides))
        return fail("dll_overrides isn't in WINEDLLOVERRIDES syntax");
    if (!identifier(p.prefix) || !identifier(p.runtime))
        return fail("the prefix and runtime must be identifiers");
    if (!one_of(p.architecture, {"pe32", "pe64"}) ||
        !one_of(p.graphics, {"auto", "gdi", "dxvk", "opengl"}) ||
        !one_of(p.scaling, {"fit", "integer", "stretch"}) ||
        !one_of(p.input_mode, {"xinput", "keyboard"}))
        return fail("architecture, graphics, scaling or input mode is not one prospero-win knows");
    if (p.width < kMinWidth || p.height < kMinHeight || p.width > kMaxWidth ||
        p.height > kMaxHeight)
        return fail("the desktop must be between 320x200 and 3840x2160");
    std::string out;
    for (const auto &line : p.comment)
    {
        if (!one_line(line))
            return fail("a comment line holds a line break");
        out += line.empty() ? ";\n" : "; " + line + "\n";
    }
    out += "[application]\nid = " + p.id + "\nname = " + p.name + "\nexecutable = " +
           p.executable + "\nworking_directory = " + p.working_directory + "\n";
    if (!p.dll_overrides.empty())
        out += "dll_overrides = " + p.dll_overrides + "\n";
    out += "prefix = " + p.prefix + "\nruntime = " + p.runtime + "\narchitecture = " +
           p.architecture + "\ngraphics = " + p.graphics + "\n\n[display]\ndesktop = " +
           std::to_string(p.width) + "x" + std::to_string(p.height) + "\nscaling = " +
           p.scaling + "\nshow_fps = " + (p.show_fps ? "true" : "false") + "\n\n[input]\nmode = " + p.input_mode + "\n" +
           (p.touchpad_mouse ? "mouse = touchpad\ntouchpad = mouse_left\n" : "");
    if (!p.switches.empty())
    {
        out += "\n[runtime]\n";
        std::vector<std::string> seen;
        for (const auto &[key, value] : p.switches)
        {
            const bool flag = one_of(key, {"thread_scheduling", "shared_input", "fast_clock"});
            if (!(flag && one_of(value, {"true", "false"})) &&
                !(key == "cpu" && one_of(value, {"native", "translator"})))
                return fail("[runtime] " + key + " = " + value + " is not one prospero-win knows");
            for (const auto &other : seen)
                if (other == key)
                    return fail("[runtime] " + key + " is set twice");
            seen.push_back(key);
            out += key + " = " + value + "\n";
        }
    }
    if (!p.debug_env.empty())
    {
        // pw_game_profile.c's debug_env_field, rule for rule.
        constexpr const char *kFamilies[] = {"PW_", "DXVK_", "MESA_", "GALLIUM_", "ZINK_", "RADV_", "VK_"};
        constexpr const char *kTitleSet[] = {"PW_VK_BATCH", "PW_INPUT_SHARED_FAST", "GALLIUM_DRIVER"};
        if (p.debug_env.size() > 5)
            return fail("[debug] takes at most 5 env lines");
        out += "\n[debug]\n";
        std::vector<std::string> seen;
        for (const auto &[name, value] : p.debug_env)
        {
            bool family = false, plain = !name.empty() && name.size() < 48 && !value.empty() && value.size() < 96;
            for (const char c : name)
                plain = plain && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
            for (const char c : value)
                plain = plain && (std::isalnum(static_cast<unsigned char>(c)) ||
                                  std::string_view("_+-,.=:/").find(c) != std::string_view::npos);
            for (const char *prefix : kFamilies)
                family = family || name.starts_with(prefix);
            for (const char *own : kTitleSet)
                family = family && name != own;
            if (!plain || !family || name.starts_with("PW_QPC_TSC_"))
                return fail("[debug] env " + name + " is not one prospero-win takes");
            if (p.show_fps && (name == "DXVK_HUD" || name == "GALLIUM_HUD"))
                return fail("[debug] env " + name + " with show_fps, which sets it itself");
            for (const auto &other : seen)
                if (other == name)
                    return fail("[debug] env " + name + " is set twice");
            seen.push_back(name);
            out += "env = " + name + "=" + value + "\n";
        }
    }
    if (out.size() > kMaxBytes)
        return fail("longer than prospero-win reads");
    text = std::move(out);
    return true;
}

bool list_add(std::string &list, const std::string &file, const std::string &after,
              std::string &error)
{
    if (!list_name(file))
    {
        error = file + " is not a profile name prospero-win lists";
        return false;
    }
    auto lines = lines_of(list);
    std::size_t count = 0, place = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const auto name = listed(lines[i]);
        if (name == file)
            return true;
        if (!name.empty())
            ++count;
        if (!after.empty() && name == after)
            place = i + 1;
    }
    if (count >= kListMax)
    {
        error = "prospero-win's launcher lists at most 32 games, and profiles.lst is full";
        return false;
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(place), file);
    list = joined(lines);
    return true;
}

bool list_remove(std::string &list, const std::string &file)
{
    auto lines = lines_of(list);
    const auto before = lines.size();
    std::erase_if(lines, [&](const std::string &line) { return listed(line) == file; });
    if (lines.size() == before)
        return false;
    list = joined(lines);
    return true;
}

bool install_profile(const std::string &profiles, const GameProfile &profile,
                     const std::string &after, std::string &error)
{
    std::string text;
    if (!profile_text(profile, text, error))
        return false;
    const auto file = profile.id + ".profile", list_path = profiles + "/profiles.lst";
    std::string list;
    if (!hui::save::read_file(list_path, &list, 1u << 20) && access(list_path.c_str(), F_OK) == 0)
    {
        error = "Can't read " + list_path;
        return false;
    }
    const auto original = list;
    if (!list_add(list, file, after, error))
        return false;
    // prospero-win makes this folder on its first run, which may not have happened yet.
    if (mkdir(profiles.c_str(), 0777) != 0 && errno != EEXIST)
    {
        error = "Can't make " + profiles + ": " + std::strerror(errno);
        return false;
    }
    // The profile before its line, so the launcher never lists a file that isn't there.
    if (auto failed = hui::save::write_atomic(profiles + "/" + file, text); !failed.empty())
    {
        error = "Can't write " + file + ": " + failed;
        return false;
    }
    if (list != original)
        if (auto failed = hui::save::write_atomic(list_path, list); !failed.empty())
        {
            error = "Can't write profiles.lst: " + failed;
            return false;
        }
    return true;
}

bool remove_profile(const std::string &profiles, const std::string &id, std::string &error)
{
    const auto file = id + ".profile", list_path = profiles + "/profiles.lst";
    std::string list;
    // Its line first, so the launcher never lists a file that's gone.
    if (hui::save::read_file(list_path, &list, 1u << 20) && list_remove(list, file))
        if (auto failed = hui::save::write_atomic(list_path, list); !failed.empty())
        {
            error = "Can't write profiles.lst: " + failed;
            return false;
        }
    if (unlink((profiles + "/" + file).c_str()) != 0 && errno != ENOENT)
    {
        error = "Can't remove " + file + ": " + std::strerror(errno);
        return false;
    }
    return true;
}
} // namespace store::library
