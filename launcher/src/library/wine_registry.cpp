// Prospero BlackBox - A Wine prefix's registry files (system.reg, user.reg), edited as text.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/wine_registry.hpp"
#include "core/save_file.hpp"
#include <cstdio>

namespace store::library
{
namespace
{
char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

bool printable(std::string_view text)
{
    for (const char c : text)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}

void put_utf8(std::string &out, unsigned code)
{
    if (code < 0x80)
        out += static_cast<char>(code);
    else if (code < 0x800)
    {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
    else
    {
        out += static_cast<char>(0xe0 | ((code >> 12) & 0x0f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Reads Wine's escaped text from at up to the unescaped end character; at is left after
// it. False when the line ends first.
bool unescape(std::string_view line, std::size_t &at, char end, std::string &out)
{
    while (at < line.size())
    {
        const char c = line[at++];
        if (c == end)
            return true;
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (at >= line.size())
            return false;
        const char e = line[at++];
        if (e == 'n')
            out += '\n';
        else if (e == 'r')
            out += '\r';
        else if (e == 't')
            out += '\t';
        else if (e == 'x')
        {
            unsigned code = 0;
            int digits = 0;
            while (digits < 4 && at < line.size() && hex_digit(line[at]) >= 0)
                code = code * 16 + static_cast<unsigned>(hex_digit(line[at++])), ++digits;
            if (digits == 0)
                return false;
            put_utf8(out, code);
        }
        else if (e >= '0' && e <= '7')
        {
            unsigned code = static_cast<unsigned>(e - '0');
            for (int digits = 1; digits < 3 && at < line.size() && line[at] >= '0' && line[at] <= '7';
                 ++digits)
                code = code * 8 + static_cast<unsigned>(line[at++] - '0');
            put_utf8(out, code);
        }
        else
            out += e; // \\, \", \[, \]
    }
    return false;
}

// Escaped as Wine writes it: the backslash itself, and the characters in specials.
std::string escape(std::string_view text, std::string_view specials)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\\' || specials.find(c) != std::string_view::npos)
            out += '\\';
        out += c;
    }
    return out;
}

std::string value_line(const std::string &name, const std::string &data)
{
    return (name.empty() ? std::string("@") : "\"" + escape(name, "\"") + "\"") + "=" + data + "\n";
}
} // namespace

bool WineRegistry::parse(std::string_view text, std::string &error)
{
    WineRegistry read;
    if (!read.read(text, error))
        return false;
    *this = std::move(read);
    return true;
}

bool WineRegistry::read(std::string_view text, std::string &error)
{
    if (!text.starts_with("WINE REGISTRY Version 2"))
    {
        error = "Not a Wine registry file";
        return false;
    }
    std::size_t at = 0, number = 0;
    while (at < text.size())
    {
        const auto newline = text.find('\n', at);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        const std::string_view raw = text.substr(at, end - at);
        std::string_view line = raw;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.remove_suffix(1);
        at = end;
        ++number;
        const auto fail = [&](const char *why)
        {
            error = "Line " + std::to_string(number) + ": " + why;
            return false;
        };
        if (line.starts_with('['))
        {
            Key key;
            std::size_t cursor = 1;
            if (!unescape(line, cursor, ']', key.name))
                return fail("a key without its closing ]");
            key.line = raw;
            keys_.push_back(std::move(key));
            continue;
        }
        if (keys_.empty())
        {
            preamble_ += raw;
            continue;
        }
        auto &items = keys_.back().items;
        // A value ending in a backslash goes on to the next line (long hex data).
        if (!items.empty() && items.back().value)
        {
            std::string_view last = items.back().lines;
            while (!last.empty() && (last.back() == '\n' || last.back() == '\r'))
                last.remove_suffix(1);
            if (last.ends_with('\\'))
            {
                items.back().lines += raw;
                continue;
            }
        }
        Item item;
        item.lines = raw;
        if (line.starts_with('"') || line.starts_with('@'))
        {
            std::size_t cursor = 1;
            if (line[0] == '"' && !unescape(line, cursor, '"', item.name))
                return fail("a value name without its closing quote");
            if (cursor >= line.size() || line[cursor] != '=')
                return fail("a value without =");
            item.value = true;
        }
        items.push_back(std::move(item));
    }
    return true;
}

std::string WineRegistry::text() const
{
    std::string out = preamble_;
    for (const auto &key : keys_)
    {
        out += key.line;
        for (const auto &item : key.items)
            out += item.lines;
    }
    return out;
}

WineRegistry::Key *WineRegistry::find(const std::string &key)
{
    for (auto &known : keys_)
        if (same(known.name, key))
            return &known;
    return nullptr;
}

const WineRegistry::Key *WineRegistry::find(const std::string &key) const
{
    return const_cast<WineRegistry *>(this)->find(key);
}

bool WineRegistry::has_key(const std::string &key) const
{
    return find(key) != nullptr;
}

const WineRegistry::Item *WineRegistry::find_value(const std::string &key,
                                                   const std::string &name) const
{
    if (const auto *known = find(key))
        for (const auto &item : known->items)
            if (item.value && same(item.name, name))
                return &item;
    return nullptr;
}

bool WineRegistry::set_raw(const std::string &key, const std::string &name,
                           const std::string &data, std::time_t now)
{
    if (key.empty() || !printable(key) || !printable(name))
        return false;
    Item item{true, name, value_line(name, data)};
    if (auto *known = find(key))
    {
        for (auto &existing : known->items)
            if (existing.value && same(existing.name, name))
            {
                existing.lines = item.lines;
                return true;
            }
        // After the last line that isn't blank: the blank line between keys stays last.
        auto place = known->items.end();
        while (place != known->items.begin() && (place - 1)->lines.find_first_not_of("\r\n") ==
                                                    std::string::npos)
            --place;
        known->items.insert(place, std::move(item));
        return true;
    }
    // A new key at the end, set off by a blank line as Wine writes them.
    std::string *last = keys_.empty() ? &preamble_
                        : keys_.back().items.empty() ? &keys_.back().line
                                                     : &keys_.back().items.back().lines;
    if (!last->empty() && last->back() != '\n')
        *last += '\n';
    if (!keys_.empty() && (keys_.back().items.empty() ||
                           keys_.back().items.back().lines.find_first_not_of("\r\n") !=
                               std::string::npos))
        keys_.back().items.push_back({false, {}, "\n"});
    // #time is a FILETIME: 100 ns steps since 1601.
    const auto filetime =
        (static_cast<unsigned long long>(now) + 11644473600ull) * 10000000ull;
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "#time=%llx\n", filetime);
    Key added;
    added.name = key;
    added.line = "[" + escape(key, "[]") + "] " + std::to_string(static_cast<long long>(now)) + "\n";
    added.items.push_back({false, {}, stamp});
    added.items.push_back(std::move(item));
    added.items.push_back({false, {}, "\n"});
    keys_.push_back(std::move(added));
    return true;
}

bool WineRegistry::set_string(const std::string &key, const std::string &name,
                              const std::string &value, std::time_t now)
{
    return printable(value) && set_raw(key, name, "\"" + escape(value, "\"") + "\"", now);
}

bool WineRegistry::set_dword(const std::string &key, const std::string &name, std::uint32_t value,
                             std::time_t now)
{
    char data[16];
    std::snprintf(data, sizeof data, "dword:%08x", value);
    return set_raw(key, name, data, now);
}

std::optional<std::string> WineRegistry::string_value(const std::string &key,
                                                      const std::string &name) const
{
    const auto *item = find_value(key, name);
    if (!item)
        return std::nullopt;
    std::string_view line = item->lines;
    std::size_t at = line.find('=');
    if (at == std::string_view::npos || at + 1 >= line.size() || line[at + 1] != '"')
        return std::nullopt;
    at += 2;
    std::string out;
    if (!unescape(line, at, '"', out))
        return std::nullopt;
    return out;
}

std::optional<std::uint32_t> WineRegistry::dword(const std::string &key,
                                                 const std::string &name) const
{
    const auto *item = find_value(key, name);
    if (!item)
        return std::nullopt;
    const std::string_view line = item->lines;
    const auto at = line.find("=dword:");
    if (at == std::string_view::npos)
        return std::nullopt;
    std::uint32_t value = 0;
    int digits = 0;
    for (std::size_t i = at + 7; i < line.size() && hex_digit(line[i]) >= 0 && digits < 8; ++i, ++digits)
        value = value * 16 + static_cast<std::uint32_t>(hex_digit(line[i]));
    if (digits != 8)
        return std::nullopt;
    return value;
}

bool load_registry(const std::string &path, WineRegistry &registry, std::string &error)
{
    // A prefix's system.reg is a few megabytes.
    std::string text;
    if (!hui::save::read_file(path, &text, 64u << 20))
    {
        error = "Can't read " + path;
        return false;
    }
    if (!registry.parse(text, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool save_registry(const std::string &path, const WineRegistry &registry, std::string &error)
{
    if (const auto failed = hui::save::write_atomic(path, registry.text()); !failed.empty())
    {
        error = "Can't write " + path + ": " + failed;
        return false;
    }
    return true;
}
} // namespace store::library
