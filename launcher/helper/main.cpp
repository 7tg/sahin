// Prospero BlackBox - The file worker: unpacks and removes apps outside the store's process.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console limits how fast an app may write to its storage: after a few
// hundred megabytes the store itself is held to about two megabytes a second,
// and every file it creates or removes then takes a tenth of a second. A
// process started by the payload loader is not limited. The store sends this
// program to the loader for each job; the connection becomes its standard input
// and output, and it speaks the lines described in install/worker.hpp.
#include "install/archive.hpp"
#include "install/files.hpp"
#include "install/worker.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef STORE_WORKER_CONSOLE
#include <ps5/kernel.h>
extern "C"
{
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv,
                                 unsigned flags, void *option, int *result);
    int sceUserServiceInitialize(void *parameters);
}
#endif

namespace
{
std::atomic<bool> cancelled{false};
std::atomic<bool> working{false};
std::atomic<std::uint64_t> written{0};
std::mutex output;

void say(const std::string &line)
{
    std::lock_guard lock(output);
    std::size_t done = 0;
    while (done < line.size())
    {
        const auto count = write(STDOUT_FILENO, line.data() + done, line.size() - done);
        if (count <= 0)
        {
            cancelled = true; // Nobody is listening any more.
            return;
        }
        done += static_cast<std::size_t>(count);
    }
}

bool read_line(std::string &line)
{
    line.clear();
    for (;;)
    {
        char byte = 0;
        const auto count = read(STDIN_FILENO, &byte, 1);
        if (count <= 0)
            return false;
        if (byte == '\n')
            return true;
        if (line.size() >= store::install::kWorkerLine)
            return false;
        line.push_back(byte);
    }
}

bool read_exact(void *data, std::size_t size)
{
    auto *bytes = static_cast<char *>(data);
    while (size)
    {
        const auto count = read(STDIN_FILENO, bytes, size);
        if (count <= 0)
            return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

// Saves a download. The file exists only once every piece and the end mark arrived.
bool save_download(const std::string &path)
{
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return false;
    std::vector<char> piece(store::install::kWorkerPiece);
    bool complete = false, saved = true;
    for (;;)
    {
        unsigned char header[4];
        if (!read_exact(header, sizeof(header)))
            break;
        const std::size_t size = static_cast<std::size_t>(header[0]) | (header[1] << 8) |
                                 (header[2] << 16) | (static_cast<std::size_t>(header[3]) << 24);
        if (size == 0)
        {
            complete = true;
            break;
        }
        if (size > piece.size() || !read_exact(piece.data(), size))
            break;
        for (std::size_t done = 0; done < size && saved;)
        {
            const auto count = write(descriptor, piece.data() + done, size - done);
            if (count <= 0)
                saved = false;
            else
                done += static_cast<std::size_t>(count);
        }
        if (!saved)
            break;
    }
    saved = complete && saved && fsync(descriptor) == 0;
    saved = close(descriptor) == 0 && saved;
    if (!saved)
        unlink(path.c_str());
    return saved;
}

// After the request, anything the store sends, and the store going away, both
// mean the same: stop.
void *watch(void *)
{
    char byte = 0;
    (void)!read(STDIN_FILENO, &byte, 1);
    cancelled = true;
    return nullptr;
}

void *report(void *)
{
    while (working.load())
    {
        say("p " + std::to_string(written.load()) + "\n");
        usleep(200000);
    }
    return nullptr;
}
} // namespace

// Takes title off the home screen through the console's app-install service. A
// payload that imports the library never starts on firmware 12.70, so it is loaded
// by path and its two functions are found through the payload SDK. 0: done.
int unregister(const std::string &title)
{
#ifdef STORE_WORKER_CONSOLE
    (void)sceUserServiceInitialize(nullptr);
    const int handle = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx", 0,
                                                nullptr, 0, nullptr, nullptr);
    if (handle < 0)
        return handle;
    const auto initialize = reinterpret_cast<int (*)()>(
        kernel_dynlib_dlsym(getpid(), static_cast<std::uint32_t>(handle), "sceAppInstUtilInitialize"));
    const auto remove = reinterpret_cast<int (*)(const char *)>(kernel_dynlib_dlsym(
        getpid(), static_cast<std::uint32_t>(handle), "sceAppInstUtilAppUnInstall"));
    if (!initialize || !remove)
        return -2;
    if (const int ready = initialize(); ready < 0)
        return ready;
    return remove(title.c_str());
#else
    (void)title;
    return -3; // a host build has no home screen
#endif
}

// Gives a folder and everything in it to this user and opens it to all, never through a
// link. False when something could not be changed.
bool reclaim(const std::string &path, int depth = 0)
{
    struct stat info
    {
    };
    if (lstat(path.c_str(), &info) != 0)
        return false;
    if (S_ISLNK(info.st_mode))
        return true;
    bool ok = lchown(path.c_str(), geteuid(), getegid()) == 0;
    if (S_ISDIR(info.st_mode))
    {
        ok = chmod(path.c_str(), 0777) == 0 && ok;
        DIR *folder = depth < 12 ? opendir(path.c_str()) : nullptr;
        if (!folder)
            return false;
        while (const dirent *entry = readdir(folder))
        {
            const std::string name = entry->d_name;
            if (name != "." && name != "..")
                ok = reclaim(path + "/" + name, depth + 1) && ok;
        }
        closedir(folder);
    }
    else if (S_ISREG(info.st_mode))
        ok = chmod(path.c_str(), 0666) == 0 && ok;
    return ok;
}

int main()
{
    using namespace store::install;
    std::signal(SIGPIPE, SIG_IGN);
    std::string magic, verb, first, title, destination;
    if (!read_line(magic) || magic != kWorkerMagic || !read_line(verb) || !read_line(first))
        return 1;
    const bool extract = verb == "extract";
    if (extract && (!read_line(title) || !read_line(destination)))
        return 1;
    if (verb == "store")
    {
        if (!worker_path(first))
        {
            say("fail The request was refused\n");
            return 1;
        }
        say("ready\n");
        const bool saved = save_download(first);
        say(saved ? "ok\n" : "fail The download could not be saved\n");
        return saved ? 0 : 1;
    }
    if (verb == "reclaim")
    {
        if (!worker_folder(first))
        {
            say("fail The request was refused\n");
            return 1;
        }
        say("ready\n");
        const bool done = reclaim(first);
        say(done ? "ok\n" : "fail The folder could not be reclaimed\n");
        return done ? 0 : 1;
    }
    if (verb == "unregister")
    {
        if (!title_id_plain(first))
        {
            say("fail ffffffff\n");
            return 1;
        }
        say("ready\n");
        const int code = unregister(first);
        char reply[32];
        std::snprintf(reply, sizeof(reply), code == 0 ? "ok\n" : "fail %08x\n",
                      static_cast<unsigned>(code));
        say(reply);
        return code == 0 ? 0 : 1;
    }
    if ((!extract && verb != "remove") || !worker_path(first) ||
        (extract && !worker_path(destination)))
    {
        say("fail The request was refused\n");
        return 1;
    }
    say("ready\n");
    working = true;
    pthread_t watcher{}, reporter{};
    const bool watching = pthread_create(&watcher, nullptr, watch, nullptr) == 0;
    const bool reporting = pthread_create(&reporter, nullptr, report, nullptr) == 0;
    if (watching)
        pthread_detach(watcher);
    std::string reply;
    if (extract)
    {
        std::string error;
        ExtractTimes times;
        if (extract_archive(first, title, destination, cancelled, written, error, &times))
            reply = "ok " + std::to_string(times.write_ms) + " " + std::to_string(times.sync_ms) +
                    " " + std::to_string(times.total_ms) + " " + std::to_string(times.files) + " " +
                    std::to_string(written.load()) + "\n";
        else
            reply = "fail " + (error.empty() ? std::string("The app could not be unpacked") : error) +
                    "\n";
    }
    else
        reply = remove_tree(first) ? "ok\n" : "fail The files could not be removed\n";
    working = false;
    if (reporting)
        pthread_join(reporter, nullptr);
    say(reply);
    return reply[0] == 'o' ? 0 : 1;
}
