// Sahin - The words the page shows, in the player's language.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/i18n.hpp"

#include "core/i18n_strings.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

namespace store::i18n
{
namespace
{
// Read by the service's thread as well as the screen's.
std::atomic<Language> current{Language::english};

// The rows by their English, for a binary search; made on first use.
const std::vector<const Row *> &index()
{
    static const std::vector<const Row *> rows = []
    {
        std::vector<const Row *> sorted;
        sorted.reserve(kStringCount);
        for (std::size_t i = 0; i < kStringCount; ++i)
            sorted.push_back(&kStrings[i]);
        std::sort(sorted.begin(), sorted.end(),
                  [](const Row *a, const Row *b) { return a->text[0] < b->text[0]; });
        return sorted;
    }();
    return rows;
}

const Row *find(std::string_view english)
{
    const auto &rows = index();
    const auto at =
        std::lower_bound(rows.begin(), rows.end(), english,
                         [](const Row *row, std::string_view key) { return row->text[0] < key; });
    return at != rows.end() && (*at)->text[0] == english ? *at : nullptr;
}
} // namespace

Language from_console(int console_language)
{
    // The console's numbers: 19 is Turkish. Anything else has no translation here: English.
    return console_language == 19 ? Language::turkish : Language::english;
}

void set_language(Language language)
{
    current.store(language);
}

Language language()
{
    return current.load();
}

std::string_view tr(std::string_view english)
{
    const Language now = current.load();
    if (now == Language::english)
        return english;
    const Row *row = find(english);
    if (!row)
        return english;
    const std::string_view words = row->text[static_cast<int>(now)];
    return words.empty() ? english : words;
}

std::string trs(std::string_view english)
{
    return std::string(tr(english));
}

std::string upper(std::string_view text)
{
    const bool turkish = current.load() == Language::turkish;
    std::string out;
    out.reserve(text.size() + 4);
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == 'i' && turkish)
        {
            out += "\xC4\xB0"; // İ
        }
        else if (byte >= 'a' && byte <= 'z')
        {
            out += static_cast<char>(byte - 'a' + 'A');
        }
        else if (byte == 0xC4 && i + 1 < text.size() &&
                 (static_cast<unsigned char>(text[i + 1]) == 0x9F ||
                  static_cast<unsigned char>(text[i + 1]) == 0xB1))
        {
            // Turkish: ğ to Ğ, and the dotless ı to a plain I.
            out += text[i + 1] == '\xB1' ? "I" : "\xC4\x9E";
            ++i;
        }
        else if (byte == 0xC5 && i + 1 < text.size() &&
                 static_cast<unsigned char>(text[i + 1]) == 0x9F)
        {
            out += "\xC5\x9E"; // ş to Ş
            ++i;
        }
        else if ((byte == 0xC3 || byte == 0xC5) && i + 1 < text.size())
        {
            // Latin-1 (U+00E0..U+00FE, but not the division sign) and the ligature oe.
            auto next = static_cast<unsigned char>(text[i + 1]);
            ++i;
            if (byte == 0xC3 && next == 0x9F)
                out += "SS"; // the sharp s has no capital in the page's fonts
            else if (byte == 0xC3 && next >= 0xA0 && next <= 0xBE && next != 0xB7)
            {
                out += static_cast<char>(byte);
                out += static_cast<char>(next - 0x20);
            }
            else if (byte == 0xC5 && next == 0x93)
            {
                out += static_cast<char>(byte);
                out += static_cast<char>(0x92);
            }
            else
            {
                out += static_cast<char>(byte);
                out += static_cast<char>(next);
            }
        }
        else
        {
            out += static_cast<char>(byte);
        }
    }
    return out;
}

const char *trc(const char *english)
{
    return tr(english).data();
}

std::string trf(std::string_view english, std::initializer_list<std::string_view> arguments)
{
    const std::string_view sentence = tr(english);
    std::string out;
    out.reserve(sentence.size() + 16);
    for (std::size_t i = 0; i < sentence.size(); ++i)
    {
        if (sentence[i] == '{' && i + 2 < sentence.size() && sentence[i + 1] >= '0' &&
            sentence[i + 1] <= '9' && sentence[i + 2] == '}')
        {
            const auto number = static_cast<std::size_t>(sentence[i + 1] - '0');
            if (number < arguments.size())
                out += *(arguments.begin() + static_cast<std::ptrdiff_t>(number));
            i += 2;
            continue;
        }
        out += sentence[i];
    }
    return out;
}

} // namespace store::i18n
