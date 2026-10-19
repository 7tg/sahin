// Prospero BlackBox - A game's install plan: what goes into the prefix, read from data.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/install_plan.hpp"
#include "core/save_file.hpp"
#include "third_party/yyjson/yyjson.h"
#include <memory>

namespace store::library
{
namespace
{
struct Reader
{
    std::string &error;
    bool failed = false;

    bool fail(std::string why)
    {
        if (!failed)
            error = std::move(why);
        failed = true;
        return false;
    }
    std::string text(yyjson_val *object, const char *key, bool required, const std::string &where)
    {
        auto *value = yyjson_obj_get(object, key);
        if (!value)
        {
            if (required)
                fail(where + ": " + key + " is missing");
            return {};
        }
        if (!yyjson_is_str(value))
        {
            fail(where + ": " + key + " must be text");
            return {};
        }
        return {yyjson_get_str(value), yyjson_get_len(value)};
    }
    std::uint64_t number(yyjson_val *object, const char *key, bool required,
                         const std::string &where)
    {
        auto *value = yyjson_obj_get(object, key);
        if (!value)
        {
            if (required)
                fail(where + ": " + key + " is missing");
            return 0;
        }
        if (!yyjson_is_uint(value))
        {
            fail(where + ": " + key + " must be a whole number");
            return 0;
        }
        return yyjson_get_uint(value);
    }
    bool flag(yyjson_val *object, const char *key, const std::string &where)
    {
        auto *value = yyjson_obj_get(object, key);
        if (value && !yyjson_is_bool(value))
            fail(where + ": " + key + " must be true or false");
        return value && yyjson_get_bool(value);
    }
};

bool unhex(const std::string &text, std::string &out)
{
    if (text.empty() || text.size() % 2)
        return false;
    out.clear();
    for (std::size_t i = 0; i < text.size(); i += 2)
    {
        int value = 0;
        for (const char c : {text[i], text[i + 1]})
        {
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0)
                return false;
            value = value * 16 + digit;
        }
        out += static_cast<char>(value);
    }
    return true;
}

bool hex64(const std::string &text)
{
    if (text.size() != 64)
        return false;
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

yyjson_val *array(Reader &read, yyjson_val *root, const char *key)
{
    auto *value = yyjson_obj_get(root, key);
    if (value && !yyjson_is_arr(value))
    {
        read.fail(std::string(key) + " must be a list");
        return nullptr;
    }
    return value;
}

void placements(Reader &read, yyjson_val *root, const char *key, std::vector<Placement> &out,
                const std::string &label = {})
{
    auto *list = array(read, root, key);
    std::size_t index = 0, count = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(list, index, count, item)
    {
        const auto where = label + key + "[" + std::to_string(index) + "]";
        if (!yyjson_is_obj(item))
        {
            read.fail(where + " must be an object");
            return;
        }
        Placement place;
        place.source = read.text(item, "source", true, where);
        place.member = read.text(item, "member", false, where);
        place.to = read.text(item, "to", true, where);
        place.keep_builtin = read.flag(item, "keep_builtin", where);
        const bool folder = place.member.ends_with('/');
        if (!place.member.empty() && !safe_relative(folder ? place.member.substr(0, place.member.size() - 1)
                                                            : place.member))
            read.fail(where + ": member is not a plain relative path");
        if (folder ? !(place.to.empty() || (place.to.ends_with('/') &&
                                            safe_relative(place.to.substr(0, place.to.size() - 1))))
                   : !safe_relative(place.to))
            read.fail(where + ": to is not a plain relative path" +
                      (folder ? " ending in / (or empty)" : ""));
        out.push_back(std::move(place));
    }
}

void registry(Reader &read, yyjson_val *root, const char *key, std::vector<RegistryValue> &out)
{
    auto *list = array(read, root, key);
    std::size_t index = 0, count = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(list, index, count, item)
    {
        const auto where = std::string(key) + "[" + std::to_string(index) + "]";
        RegistryValue value;
        value.key = read.text(item, "key", true, where);
        value.name = read.text(item, "name", false, where);
        if (yyjson_obj_get(item, "dword"))
        {
            const auto number = read.number(item, "dword", true, where);
            if (number > 0xffffffffu)
                read.fail(where + ": dword is more than 32 bits");
            value.dword = true;
            value.number = static_cast<std::uint32_t>(number);
        }
        else
            value.text = read.text(item, "string", true, where);
        out.push_back(std::move(value));
    }
}

// An XML name as the user files use them: letters, digits, '_', '-', '.', not a digit first.
bool xml_name(const std::string &name)
{
    if (name.empty() || (name[0] >= '0' && name[0] <= '9') || name[0] == '-' || name[0] == '.')
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.'))
            return false;
    return true;
}

// An attribute's value: printable, nothing that ends or escapes it, and {width} and {height}
// the only placeholders.
bool attribute_value(const std::string &value)
{
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' || c == '}')
            return false;
        if (c == '{')
        {
            const auto end = value.find('}', i);
            if (end == std::string::npos)
                return false;
            const auto name = value.substr(i, end - i + 1);
            if (name != "{width}" && name != "{height}")
                return false;
            i = end;
        }
    }
    return true;
}

void user_files(Reader &read, yyjson_val *root, std::vector<UserFile> &out)
{
    auto *list = array(read, root, "user_files");
    std::size_t index = 0, count = 0;
    yyjson_val *item = nullptr;
    yyjson_arr_foreach(list, index, count, item)
    {
        const auto where = "user_files[" + std::to_string(index) + "]";
        if (!yyjson_is_obj(item))
        {
            read.fail(where + " must be an object");
            return;
        }
        UserFile file;
        file.file = read.text(item, "file", true, where);
        file.element = read.text(item, "element", true, where);
        if (!safe_relative(file.file))
            read.fail(where + ": file is not a plain relative path");
        if (!xml_name(file.element))
            read.fail(where + ": element is not a plain XML name");
        auto *attributes = yyjson_obj_get(item, "attributes");
        if (!yyjson_is_obj(attributes) || yyjson_obj_size(attributes) == 0)
            read.fail(where + ": attributes must be an object with at least one");
        std::size_t inner = 0, inner_count = 0;
        yyjson_val *name = nullptr, *value = nullptr;
        if (yyjson_is_obj(attributes))
        yyjson_obj_foreach(attributes, inner, inner_count, name, value)
        {
            std::string key(yyjson_get_str(name), yyjson_get_len(name));
            if (!xml_name(key))
                read.fail(where + ": " + key + " is not a plain XML name");
            else if (!yyjson_is_str(value) ||
                     !attribute_value({yyjson_get_str(value), yyjson_get_len(value)}))
                read.fail(where + ": " + key + " must be plain text ({width} and {height} allowed)");
            else
                file.attributes.emplace_back(std::move(key),
                                             std::string(yyjson_get_str(value), yyjson_get_len(value)));
        }
        out.push_back(std::move(file));
    }
}

// What a plan and each of its options add to the game: placements, written files, ini
// edits and the hashes the finished folder must have. label says where (an option's).
void files(Reader &read, yyjson_val *root, std::vector<Placement> &game_files,
           std::vector<Placement> &system_files, std::vector<TextFile> &text_files,
           std::vector<IniEdit> &ini_edits, std::vector<std::pair<std::string, std::string>> &expected,
           const std::string &label = {})
{
    placements(read, root, "game_files", game_files, label);
    placements(read, root, "system_files", system_files, label);
    std::size_t index = 0, count = 0;
    yyjson_val *key = nullptr, *item = nullptr;
    auto *texts = array(read, root, "texts");
    yyjson_arr_foreach(texts, index, count, item)
    {
        const auto where = label + "texts[" + std::to_string(index) + "]";
        TextFile file{read.text(item, "to", true, where), read.text(item, "body", true, where),
                      read.flag(item, "append", where)};
        if (!safe_relative(file.to))
            read.fail(where + ": to is not a plain relative path");
        text_files.push_back(std::move(file));
    }
    auto *ini = array(read, root, "ini");
    yyjson_arr_foreach(ini, index, count, item)
    {
        const auto where = label + "ini[" + std::to_string(index) + "]";
        IniEdit edit;
        edit.file = read.text(item, "file", true, where);
        edit.lf = read.flag(item, "lf", where);
        if (!safe_relative(edit.file))
            read.fail(where + ": file is not a plain relative path");
        auto *values = yyjson_obj_get(item, "values");
        if (!values || !yyjson_is_obj(values))
            read.fail(where + ": values must be an object");
        std::size_t inner = 0, inner_count = 0;
        yyjson_val *name = nullptr, *value = nullptr;
        if (yyjson_is_obj(values)) // a missing one is already reported
        yyjson_obj_foreach(values, inner, inner_count, name, value)
        {
            if (!yyjson_is_str(value))
                read.fail(where + ": every value must be text");
            else
                edit.values.emplace_back(std::string(yyjson_get_str(name), yyjson_get_len(name)),
                                         std::string(yyjson_get_str(value), yyjson_get_len(value)));
        }
        ini_edits.push_back(std::move(edit));
    }
    auto *expect = yyjson_obj_get(root, "expect");
    if (expect && !yyjson_is_obj(expect))
        read.fail(label + "expect must be an object");
    if (yyjson_is_obj(expect))
    yyjson_obj_foreach(expect, index, count, key, item)
    {
        std::string path(yyjson_get_str(key), yyjson_get_len(key));
        std::string hash = yyjson_is_str(item) ? yyjson_get_str(item) : "";
        if (!safe_relative(path) || !hex64(hash))
            read.fail(label + "expect." + path + " must be a plain path and a SHA-256");
        expected.emplace_back(std::move(path), std::move(hash));
    }
}
} // namespace

bool safe_relative(std::string_view path)
{
    if (path.empty())
        return false;
    std::size_t start = 0;
    while (start <= path.size())
    {
        auto end = path.find('/', start);
        end = end == std::string_view::npos ? path.size() : end;
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (const char c : part)
            if (c < 0x20 || c > 0x7e || c == '\\' || c == ':')
                return false;
        start = end + 1;
    }
    return true;
}

const InstallPlan::Option *InstallPlan::option(const std::string &id) const
{
    for (const auto &known : options)
        if (known.id == id)
            return &known;
    return nullptr;
}

const Source *InstallPlan::source(const std::string &id) const
{
    for (const auto &known : sources)
        if (known.id == id)
            return &known;
    return nullptr;
}

bool parse_plan(std::string_view text, InstallPlan &out, std::string &error)
{
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc{
        yyjson_read(text.data(), text.size(), 0), yyjson_doc_free};
    auto *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    if (!root || !yyjson_is_obj(root))
    {
        error = "The install plan is not a JSON object";
        return false;
    }
    Reader read{error};
    if (read.number(root, "schema", true, "plan") != 1)
        return read.fail("The install plan's schema is not 1");
    InstallPlan plan;
    plan.game = read.text(root, "game", true, "plan");
    plan.title = read.text(root, "title", true, "plan");
    plan.release = read.text(root, "release", true, "plan");
    plan.prefix = read.text(root, "prefix", true, "plan");
    if (!safe_relative(plan.game) || plan.game.find('/') != std::string::npos ||
        !safe_relative(plan.prefix) || plan.prefix.find('/') != std::string::npos)
        read.fail("plan: game and prefix must be plain folder names");

    auto *sources = yyjson_obj_get(root, "sources");
    if (sources && !yyjson_is_obj(sources))
        read.fail("sources must be an object");
    std::size_t index = 0, count = 0;
    yyjson_val *key = nullptr, *item = nullptr;
    if (yyjson_is_obj(sources)) // yyjson's object loop can't take null
    yyjson_obj_foreach(sources, index, count, key, item)
    {
        Source source;
        source.id.assign(yyjson_get_str(key), yyjson_get_len(key));
        const auto where = "sources." + source.id;
        source.name = read.text(item, "name", true, where);
        source.sha256 = read.text(item, "sha256", true, where);
        source.size = read.number(item, "size", true, where);
        source.url = read.text(item, "url", false, where);
        source.drive = read.text(item, "drive", false, where);
        if (!hex64(source.sha256))
            read.fail(where + ": sha256 must be 64 lower-case hex digits");
        if (!source.url.empty() && !source.url.starts_with("https://"))
            read.fail(where + ": url must be https");
        if (plan.source(source.id))
            read.fail(where + " is named twice");
        plan.sources.push_back(std::move(source));
    }
    files(read, root, plan.game_files, plan.system_files, plan.texts, plan.ini, plan.expect);
    for (const auto *list : {&plan.game_files, &plan.system_files})
        for (const auto &place : *list)
            if (!plan.source(place.source))
                read.fail("A placement names " + place.source + ", which isn't in sources");
    auto *patches = array(read, root, "patches");
    yyjson_arr_foreach(patches, index, count, item)
    {
        const auto where = "patches[" + std::to_string(index) + "]";
        BytePatch patch;
        patch.file = read.text(item, "file", true, where);
        patch.offset = read.number(item, "offset", true, where);
        if (!safe_relative(patch.file))
            read.fail(where + ": file is not a plain relative path");
        if (!unhex(read.text(item, "from", true, where), patch.from) ||
            !unhex(read.text(item, "to", true, where), patch.to) ||
            patch.from.size() != patch.to.size())
            read.fail(where + ": from and to must be hex bytes of the same length");
        plan.patches.push_back(std::move(patch));
    }
    auto *reg = yyjson_obj_get(root, "registry");
    if (reg && !yyjson_is_obj(reg))
        read.fail("registry must be an object");
    registry(read, reg, "machine", plan.machine);
    registry(read, reg, "user", plan.user);
    user_files(read, root, plan.user_files);

    auto *profile = yyjson_obj_get(root, "profile");
    if (!profile || !yyjson_is_obj(profile))
        read.fail("profile is missing");
    else
    {
        auto &p = plan.profile;
        const std::string where = "profile";
        p.id = read.text(profile, "id", true, where);
        p.name = read.text(profile, "name", true, where);
        p.executable = read.text(profile, "executable", true, where);
        p.working_directory = read.text(profile, "working_directory", true, where);
        p.dll_overrides = read.text(profile, "dll_overrides", false, where);
        p.prefix = plan.prefix;
        p.width = static_cast<unsigned>(read.number(profile, "width", true, where));
        p.height = static_cast<unsigned>(read.number(profile, "height", true, where));
        if (auto *runtime = yyjson_obj_get(profile, "runtime"); yyjson_is_obj(runtime))
        {
            std::size_t inner = 0, inner_count = 0;
            yyjson_val *name = nullptr, *value = nullptr;
            yyjson_obj_foreach(runtime, inner, inner_count, name, value)
            {
                if (!yyjson_is_str(value))
                    read.fail("profile.runtime: every value must be text");
                else
                    p.switches.emplace_back(std::string(yyjson_get_str(name), yyjson_get_len(name)),
                                            std::string(yyjson_get_str(value), yyjson_get_len(value)));
            }
        }
        p.comment = {plan.title + ": written by the game's PS5 app. Reinstalling or repairing the game",
                     "writes this file again."};
        plan.profile_after = read.text(profile, "after", false, where);
        std::string unused;
        if (!read.failed && !profile_text(p, unused, error))
            read.failed = true;
    }
    auto *upgrades = array(read, root, "upgrades");
    yyjson_arr_foreach(upgrades, index, count, item)
    {
        const auto where = "upgrades[" + std::to_string(index) + "]";
        auto from = read.text(item, "from", true, where), source = read.text(item, "source", true, where);
        if (!plan.source(source))
            read.fail(where + " names " + source + ", which isn't in sources");
        if (from == plan.release)
            read.fail(where + " is from the release it installs");
        const auto kind = read.text(item, "kind", false, where);
        if (!kind.empty() && kind != "zip" && kind != "rtpatch")
            read.fail(where + " is of kind " + kind + ", which isn't zip or rtpatch");
        plan.upgrades.push_back({std::move(from), std::move(source), kind == "rtpatch"});
    }
    auto *pending = array(read, root, "pending");
    yyjson_arr_foreach(pending, index, count, item)
    {
        if (!yyjson_is_str(item))
            read.fail("pending: every item must be text");
        else
            plan.pending.emplace_back(yyjson_get_str(item), yyjson_get_len(item));
    }
    auto *strip = array(read, root, "strip");
    yyjson_arr_foreach(strip, index, count, item)
    {
        if (!yyjson_is_str(item))
            read.fail("strip: every item must be text");
        else if (std::string path(yyjson_get_str(item), yyjson_get_len(item)); !safe_relative(path))
            read.fail("strip: " + path + " isn't a path inside the game's folder");
        else
            plan.strip.push_back(std::move(path));
    }
    if (auto *touchpad = yyjson_obj_get(root, "touchpad"))
    {
        if (!yyjson_is_obj(touchpad))
            read.fail("touchpad must be an object");
        else
        {
            std::vector<Placement> no_files, no_system;
            std::vector<TextFile> no_texts;
            std::vector<std::pair<std::string, std::string>> no_expect;
            files(read, touchpad, no_files, no_system, no_texts, plan.touchpad_ini, no_expect,
                  "touchpad.");
            if (!no_files.empty() || !no_system.empty() || !no_texts.empty() || !no_expect.empty())
                read.fail("touchpad holds ini edits only");
        }
    }
    auto *options = array(read, root, "options");
    yyjson_arr_foreach(options, index, count, item)
    {
        const auto where = "options[" + std::to_string(index) + "]";
        if (!yyjson_is_obj(item))
        {
            read.fail(where + " must be an object");
            break;
        }
        InstallPlan::Option option;
        option.id = read.text(item, "id", true, where);
        option.title = read.text(item, "title", true, where);
        option.description = read.text(item, "description", false, where);
        option.on = read.flag(item, "default", where);
        option.dll_overrides = read.text(item, "dll_overrides", false, where);
        if (!safe_relative(option.id) || option.id.find('/') != std::string::npos)
            read.fail(where + ": id must be a plain name");
        if (plan.option(option.id))
            read.fail(where + ": " + option.id + " is named twice");
        files(read, item, option.game_files, option.system_files, option.texts, option.ini,
              option.expect, where + ".");
        for (const auto *list : {&option.game_files, &option.system_files})
            for (const auto &place : *list)
                if (!plan.source(place.source))
                    read.fail(where + ": a placement names " + place.source + ", which isn't in sources");
        plan.options.push_back(std::move(option));
    }
    if (read.failed)
        return false;
    out = std::move(plan);
    return true;
}

bool load_plan(const std::string &path, InstallPlan &plan, std::string &error)
{
    std::string text;
    if (!hui::save::read_file(path, &text, 4u << 20))
    {
        error = "Can't read " + path;
        return false;
    }
    if (!parse_plan(text, plan, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}
} // namespace store::library
