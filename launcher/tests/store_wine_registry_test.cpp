// Prospero BlackBox - Wine registry files: read, edited in place, written back as they were.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/save_file.hpp"
#include "library/wine_registry.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace store::library;

namespace
{
const char kUser[] = "WINE REGISTRY Version 2\n"
                     ";; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n"
                     "\n"
                     "#arch=win64\n"
                     "\n"
                     "[Control Panel\\\\Desktop] 1791414507\n"
                     "#time=1dd56b0dc933808\n"
                     "\"Wallpaper\"=\"\"\n"
                     "\"Big\"=hex:00,01,02,03,04,05,06,07,08,09,0a,0b,0c,0d,0e,0f,10,11,12,13,\\\n"
                     "  14,15,16,17\n"
                     "\n"
                     "[Software\\\\EA Games\\\\Need for Speed Most Wanted] 1791414901\n"
                     "#time=1dd56b1adfaa5ca\n"
                     "@=\"INSERTYOURCDKEYHERE\"\n"
                     "\"CD Drive\"=\"D:\\\\\"\n"
                     "\"g_MotionBlurEnable\"=dword:00000001\n"
                     "\"g_RacingResolution\"=dword:87800438\n"
                     "\n"
                     "[Software\\\\Wine\\\\DllOverrides] 1791414550\n"
                     "#time=1dd56b0dc933808\n"
                     "\"d3d9\"=\"native\"\n";
const std::string kMw = "Software\\EA Games\\Need for Speed Most Wanted";
} // namespace

int main(int argc, char **argv)
{
    std::string error;
    WineRegistry registry;

    // Read and written back byte for byte, continuation lines included.
    assert(registry.parse(kUser, error));
    assert(registry.text() == kUser);
    assert(!registry.parse("REGEDIT4\n", error) && !error.empty());

    // Keys and names match without regard to case; @ is the empty name.
    assert(registry.has_key("software\\ea games\\NEED FOR SPEED MOST WANTED"));
    assert(registry.dword(kMw, "G_RACINGRESOLUTION") == 0x87800438u);
    assert(registry.string_value(kMw, "") == "INSERTYOURCDKEYHERE");
    assert(registry.string_value(kMw, "CD Drive") == "D:\\");
    assert(!registry.dword(kMw, "CD Drive") && !registry.string_value(kMw, "missing"));
    assert(registry.dword("Control Panel\\Desktop", "Big") == std::nullopt);

    // A value set changes its own line and nothing else; a new one goes before the
    // blank line that ends the key.
    assert(packed_resolution(1920, 1080) == 0x87800438u &&
           packed_resolution(3840, 2160) == 0x8f000870u &&
           packed_resolution(2560, 1440) == 0x8a0005a0u);
    assert(registry.set_dword(kMw, "g_RacingResolution", packed_resolution(3840, 2160), 0));
    assert(registry.set_dword(kMw, "g_MotionBlurEnable", 0, 0));
    assert(registry.set_dword(kMw, "g_VSyncOn", 1, 0));
    std::string expected = kUser;
    expected.replace(expected.find("dword:87800438"), 14, "dword:8f000870");
    expected.replace(expected.find("\"g_MotionBlurEnable\"=dword:00000001"), 35,
                     "\"g_MotionBlurEnable\"=dword:00000000");
    expected.insert(expected.find("\n\n[Software\\\\Wine") + 1, "\"g_VSyncOn\"=dword:00000001\n");
    assert(registry.text() == expected);
    assert(registry.string_value(kMw, "") == "INSERTYOURCDKEYHERE");

    // A key the file lacks: appended, stamped, set off by a blank line; quotes and
    // backslashes escaped as Wine escapes them.
    const std::string hklm = "Software\\Wow6432Node\\EA Games\\Need for Speed Most Wanted";
    assert(registry.set_string(hklm, "Install Dir", "C:\\Games\\NFSMW\\", 1791500000));
    assert(registry.set_string(hklm, "Quote", "say \"hi\"", 1791500000));
    assert(!registry.set_string(hklm, "Bad", "line\nbreak", 1791500000));
    assert(!registry.set_dword("", "x", 1, 1791500000));
    const std::string text = registry.text();
    const std::string block = "\"d3d9\"=\"native\"\n\n"
                              "[Software\\\\Wow6432Node\\\\EA Games\\\\Need for Speed Most Wanted] "
                              "1791500000\n#time=1dd5777d0653000\n"
                              "\"Install Dir\"=\"C:\\\\Games\\\\NFSMW\\\\\"\n"
                              "\"Quote\"=\"say \\\"hi\\\"\"\n\n";
    assert(text.ends_with(block));
    WineRegistry again;
    assert(again.parse(text, error) && again.text() == text);
    assert(again.string_value(hklm, "Install Dir") == "C:\\Games\\NFSMW\\");
    assert(again.string_value(hklm, "Quote") == "say \"hi\"");

    // Saved whole and read back.
    char folder[] = "/tmp/prospero-registry-XXXXXX";
    assert(mkdtemp(folder));
    const std::string path = std::string(folder) + "/user.reg";
    assert(save_registry(path, again, error));
    WineRegistry loaded;
    assert(load_registry(path, loaded, error) && loaded.text() == text);
    assert(!load_registry(std::string(folder) + "/none.reg", loaded, error));
    std::filesystem::remove_all(folder);

    // A real prefix's files, when given: read and written back unchanged.
    for (int i = 1; i < argc; ++i)
    {
        std::string data;
        assert(hui::save::read_file(argv[i], &data, 64u << 20));
        WineRegistry real;
        assert(real.parse(data, error));
        assert(real.text() == data);
        std::cout << "Round trip: " << argv[i] << "\n";
    }
    std::cout << "Wine registry checks passed\n";
}
