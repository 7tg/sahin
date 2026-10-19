// Prospero BlackBox - Fuzz target for everything the installer parses: ZIP and JSON.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog/catalog.hpp"
#include "install/archive.hpp"
#include "install/files.hpp"
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace
{
const std::string &scratch()
{
    static const std::string path = "/tmp/prospero-fuzz-" + std::to_string(getpid());
    return path;
}
void archive(std::string_view bytes)
{
    const auto file = scratch() + ".zip", folder = scratch() + ".out";
    const int descriptor = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0)
        __builtin_trap();
    const auto count = write(descriptor, bytes.data(), bytes.size());
    close(descriptor);
    if (count != static_cast<ssize_t>(bytes.size()))
        __builtin_trap();
    store::install::ArchiveInfo info;
    std::string error;
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> written{0};
    if (store::install::inspect_archive(file, "PPSA99500", info, error))
    {
        const bool unpacked =
            store::install::extract_archive(file, "PPSA99500", folder, cancelled, written, error);
        // Never more than the directory declared, and exactly that on success.
        if (written > info.unpacked || (unpacked && written != info.unpacked))
            __builtin_trap();
    }
    if (!store::install::remove_tree(folder))
        __builtin_trap();
}
void records(std::string_view body)
{
    using namespace store::catalog;
    std::string error;
    Entry entry;
    Receipt receipt;
    Journal journal;
    std::vector<Entry> entries;
    std::map<std::string, std::string> versions;
    parse_index(body, entries, error);
    parse_detail(body, "PPSA99500", entry, error);
    parse_versions(body, versions, error);
    parse_installed(body, entry, error);
    // What a record parser accepts must survive being written out again.
    if (parse_receipt(body, receipt, error))
    {
        Receipt again;
        if (!parse_receipt(format_receipt(receipt), again, error) ||
            again.location != receipt.location || again.release_tag != receipt.release_tag)
            __builtin_trap();
    }
    if (parse_journal(body, journal, error))
    {
        Journal again;
        if (!parse_journal(format_journal(journal), again, error) ||
            again.location != journal.location || again.state != journal.state)
            __builtin_trap();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::string_view body(reinterpret_cast<const char *>(data) + 1, size - 1);
    if (data[0] & 1)
        archive(body);
    else
        records(body);
    return 0;
}
