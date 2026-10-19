// Prospero BlackBox - The store's menu clicks load under their cue names.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/cues.hpp"

#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    const std::string root = argc > 1 ? argv[1] : "assets/audio/sfx";
    hui::audio::SoundBank bank;
    const auto stats = bank.load(root);
    if (stats.rejected != 0 || stats.files != 20)
    {
        std::cerr << "menu sfx: files=" << stats.files << " rejected=" << stats.rejected << "\n";
        for (const auto &error : stats.errors)
            std::cerr << error << "\n";
        return 1;
    }
    const hui::audio::Cue cues[] = {
        hui::audio::Cue::focus,     hui::audio::Cue::select,      hui::audio::Cue::back,
        hui::audio::Cue::tab,       hui::audio::Cue::open,        hui::audio::Cue::modal_open,
        hui::audio::Cue::modal_close, hui::audio::Cue::toggle,    hui::audio::Cue::slider,
        hui::audio::Cue::error,     hui::audio::Cue::notify,
    };
    for (const auto cue : cues)
    {
        if (!bank.has_recording(cue, hui::audio::SoundSet::menu))
            return 2;
        if (bank.has_recording(cue, hui::audio::SoundSet::glass))
            return 3;
    }
    std::cout << "Menu sound checks passed\n";
    return 0;
}
