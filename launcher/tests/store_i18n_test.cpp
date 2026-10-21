// Sahin - The words in each language: lookup, fallback, placeholders, capitals and the table's
// shape.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/early_exit.hpp"
#include "core/i18n.hpp"
#include "core/i18n_strings.hpp"
#include <cassert>
#include <set>
#include <string>

namespace
{
// The {0}, {1}... of a sentence, in the order they stand.
std::string placeholders(std::string_view text)
{
    std::string found;
    for (std::size_t i = 0; i + 2 < text.size(); ++i)
        if (text[i] == '{' && text[i + 1] >= '0' && text[i + 1] <= '9' && text[i + 2] == '}')
            found += text[i + 1];
    std::set<char> unique(found.begin(), found.end());
    return std::string(unique.begin(), unique.end());
}
} // namespace

int main()
{
    using namespace store::i18n;

    // The console's languages: Turkish is 19; both Englishes and everything else are English.
    assert(from_console(19) == Language::turkish);
    assert(from_console(1) == Language::english && from_console(18) == Language::english);
    assert(from_console(0) == Language::english && from_console(5) == Language::english);
    assert(from_console(-1) == Language::english);
    assert(language() == Language::english); // before anything is set

    // The same sentence, in each language; a sentence that is not in the table stays as it is.
    set_language(Language::english);
    assert(tr("Install") == "Install");
    set_language(Language::turkish);
    assert(language() == Language::turkish);
    assert(tr("Install") == "Yükle");
    assert(trs("Repair") == "Onar");
    assert(std::string(trc("Play")) == "Oyna");
    assert(tr("This sentence is not in the table") == "This sentence is not in the table");
    // The notice main.cpp sets through trs() is a named constant, which tools/check-i18n.py
    // can't see: its row is checked here.
    assert(tr(store::kEarlyExitHint) != store::kEarlyExitHint);

    // {0} and the arguments.
    assert(trf("Version {0}.", {"1.0"}) == "Sürüm 1.0.");
    assert(trf("{0} of {1}", {"1 MB", "4 MB"}) == "1 MB / 4 MB");
    set_language(Language::english);
    assert(trf("Version {0}.", {"1.0"}) == "Version 1.0.");
    assert(trf("{0} of {1}", {"1 MB", "4 MB"}) == "1 MB of 4 MB");
    // A {0} the call has no argument for is dropped, not read out of bounds.
    assert(trf("{0} of {1}", {"1 MB"}) == "1 MB of ");
    assert(trf("No placeholder", {}) == "No placeholder");

    // Capitals for the small tracked labels: accented letters too, and the sharp s as two.
    assert(upper("ready to install") == "READY TO INSTALL");
    assert(upper("Français à ñ ü ç œ") == "FRANÇAIS À Ñ Ü Ç Œ");
    assert(upper("Straße") == "STRASSE");
    assert(upper("a÷b") == "A÷B"); // the division sign is not a letter
    // Turkish: the dotted i is İ and the dotless ı is I, but only while the page speaks Turkish.
    set_language(Language::turkish);
    assert(upper("yüklemeye hazır") == "YÜKLEMEYE HAZIR");
    assert(upper("işığı ğ ç ö ü") == "İŞIĞI Ğ Ç Ö Ü");
    set_language(Language::english);
    assert(upper("işığı") == "IŞIĞI");

    // The table: a sentence once, in every language, with the same {0}s in all of them.
    std::set<std::string> seen;
    for (std::size_t i = 0; i < kStringCount; ++i)
    {
        const Row &row = kStrings[i];
        assert(!row.text[0].empty());
        assert(seen.insert(std::string(row.text[0])).second); // no sentence twice
        for (int column = 1; column < kLanguageCount; ++column)
        {
            assert(!row.text[column].empty());
            assert(placeholders(row.text[column]) == placeholders(row.text[0]));
        }
    }
    return 0;
}
