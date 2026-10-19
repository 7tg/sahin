// ps5-homebrew-ui - IME ABI used by ProsperoRadio's native keyboard.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace hui::ps5::detail
{
struct ImeParam
{
    std::int32_t user_id, type;
    std::uint64_t supported_languages;
    std::int32_t enter_label, input_method;
    void *filter;
    std::uint32_t option, max_text_length;
    std::uint16_t *input_text_buffer;
    float pos_x, pos_y;
    std::int32_t horizontal_alignment, vertical_alignment;
    const std::uint16_t *placeholder, *title;
    std::int8_t reserved[16];
};
struct ImeResult
{
    std::int32_t outcome;
    std::int8_t reserved[12];
};
static_assert(sizeof(ImeParam) == 96);
static_assert(offsetof(ImeParam, input_text_buffer) == 40);
static_assert(offsetof(ImeParam, title) == 72);
static_assert(sizeof(ImeResult) == 16);
} // namespace hui::ps5::detail

extern "C"
{
    int sceCommonDialogInitialize();
    int sceImeDialogAbort();
    int sceImeDialogGetResult(hui::ps5::detail::ImeResult *result);
    int sceImeDialogGetStatus();
    int sceImeDialogInit(const hui::ps5::detail::ImeParam *param, const void *extended);
    int sceImeDialogTerm();
    int sceSysmoduleLoadModule(std::uint16_t module_id);
    int sceSysmoduleUnloadModule(std::uint16_t module_id);
    int sceUserServiceGetForegroundUser(std::int32_t *user_id);
}
