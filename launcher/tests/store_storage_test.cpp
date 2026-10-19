// Prospero BlackBox - Probe owns only its new directories and rejects symlink paths.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/storage_probe.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

int main()
{
    char temporary[] = "/tmp/prospero-blackbox-storage-XXXXXX";
    assert(mkdtemp(temporary));
    namespace fs = std::filesystem;
    // Its real path: the probe refuses symlinks, and macOS's /tmp is one.
    const fs::path root = fs::canonical(temporary);
    fs::create_directory(root / "title");
    std::ofstream(root / "title" / "keep.txt") << "unchanged";
    const auto result = store::system::probe_storage(root.string());
    assert(result.renamed && result.error.empty() && !result.filesystem.empty() &&
           result.available > 0);
    assert(std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 1);
    assert(fs::file_size(root / "title" / "keep.txt") == 9);
    fs::create_directory_symlink(root / "title", root / "link");
    assert(!store::system::probe_storage((root / "link").string()).renamed);
    assert(!store::system::probe_storage((root / "absent").string()).renamed);
    assert(!store::system::probe_storage(root.string() + "/../").renamed);
    fs::remove_all(root);
}
