// Sahin - The table of sentences behind i18n::tr (core/i18n_strings.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/i18n.hpp"

#include <cstddef>
#include <string_view>

namespace store::i18n
{

// One sentence in every language, in the order of Language. The English is the key; an empty
// column has no translation yet and the English is shown instead.
struct Row
{
    std::string_view text[kLanguageCount];
};

extern const Row kStrings[];
extern const std::size_t kStringCount;

} // namespace store::i18n
