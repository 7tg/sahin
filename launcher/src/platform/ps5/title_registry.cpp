// Prospero BlackBox - The console's list of installed titles, through libSceAppInstUtil.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/title_registry.hpp"
#include "install/worker.hpp"
#include "platform/ps5/system.hpp"
#include "system/worker_launch.hpp"
#include <cstddef>

extern "C"
{
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv,
                                 unsigned flags, void *option, int *result);
    int sceKernelDlsym(int handle, const char *symbol, void **address);
}

namespace store::system
{
namespace
{
// An app may not import this library, so it is loaded by path when first
// needed (as ShadowMountPlus does). Until it has loaded, nothing is called.
using Initialize = int (*)();
using UnInstall = int (*)(const char *);
Initialize initialize_fn = nullptr;
UnInstall uninstall_fn = nullptr;
bool initialized = false;

int load()
{
    if (uninstall_fn)
        return 0;
    const int handle = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx", 0,
                                                nullptr, 0, nullptr, nullptr);
    if (handle < 0)
        return handle;
    void *init = nullptr, *remove = nullptr;
    if (sceKernelDlsym(handle, "sceAppInstUtilInitialize", &init) != 0 || !init ||
        sceKernelDlsym(handle, "sceAppInstUtilAppUnInstall", &remove) != 0 || !remove)
        return -2;
    initialize_fn = reinterpret_cast<Initialize>(init);
    uninstall_fn = reinterpret_cast<UnInstall>(remove);
    return 0;
}

int ready()
{
    if (const int loaded = load(); loaded != 0)
        return loaded;
    if (!initialized)
    {
        const int result = initialize_fn();
        if (result < 0)
            return result;
        initialized = true;
    }
    return 0;
}
} // namespace

int prepare_title_registry()
{
    return ready();
}

int unregister_title(const std::string &title_id)
{
    // A title ID is nine characters, four capitals and five digits (PPSA99400).
    if (title_id.size() != 9)
        return -1;
    for (std::size_t i = 0; i < title_id.size(); ++i)
    {
        const char c = title_id[i];
        if (i < 4 ? (c < 'A' || c > 'Z') : (c < '0' || c > '9'))
            return -1;
    }
#ifdef STORE_FILE_WORKER
    // The file worker, a loader payload, may use the service on every firmware tried;
    // the store itself may not on 12.70 (the library load answers 0x80020063).
    int code = -1;
    const int asked = install::worker_unregister(
        [](install::Channel &channel) { return launch_worker(channel); }, title_id, code);
    hui::sys::log("[STORE] unregister worker=%d code=0x%08x", asked, static_cast<unsigned>(code));
    if (asked == 1)
        return 0;
#endif
    if (const int state = ready(); state != 0)
        return state;
    return uninstall_fn(title_id.c_str());
}
} // namespace store::system
