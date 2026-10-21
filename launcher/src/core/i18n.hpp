// Sahin - The words the page shows, in the player's language.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// Every sentence the page writes for a person is its English text, handed to tr(); the table in
// core/i18n_strings.cpp has the same sentence in Turkish. A sentence the table does not have, or
// has no Turkish of yet, stays English. The game's own words (its name, file names, paths,
// versions) are never passed through here. tools/check-i18n.py keeps the table and the code in
// step.
namespace store::i18n
{

// The order of the columns in the table.
enum class Language : std::uint8_t
{
    english,
    turkish,
};
constexpr int kLanguageCount = 2;

// What the console is set to (SCE_SYSTEM_SERVICE_PARAM_ID_LANG) as one of ours: 19 is Turkish,
// anything else English.
Language from_console(int console_language);

void set_language(Language language);
Language language();

// The sentence in the current language; the English itself when there is no translation.
std::string_view tr(std::string_view english);
// The same as a string, for what takes ownership of its text.
std::string trs(std::string_view english);
// Capitals for small tracked labels ("READY TO INSTALL"), accented letters and ß included. In
// Turkish the dotted i and the dotless ı keep their own capitals: İ and I.
std::string upper(std::string_view text);
// For an API that wants a C string. Every row and every English text is a literal, so the result
// is one too: it ends where the text does and lives as long as the program.
const char *trc(const char *english);
// The same, with {0}, {1}... replaced by the arguments.
std::string trf(std::string_view english, std::initializer_list<std::string_view> arguments);

} // namespace store::i18n
