// Prospero BlackBox - Talking to the file worker, the process that unpacks and removes.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/worker.hpp"
#include <algorithm>
#include <cstdlib>
#include <memory>

namespace store::install
{
bool worker_path(std::string_view path)
{
    if (path.size() < 2 || path.size() >= kWorkerLine || path.front() != '/' || path.back() == '/')
        return false;
    bool inside = false;
    std::size_t start = 1;
    while (start <= path.size())
    {
        std::size_t end = path.find('/', start);
        if (end == path.npos)
            end = path.size();
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (const char c : part)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                return false;
        // The work folder itself is never the target: something inside it is.
        if (part == "sahin" && end < path.size())
            inside = true;
        start = end + 1;
    }
    return inside;
}

namespace
{
struct Session
{
    Channel channel;
    std::string pending;
    bool open = false;
    ~Session()
    {
        if (open && channel.close)
            channel.close();
    }
    bool send(const std::string &text)
    {
        std::size_t done = 0;
        while (done < text.size())
        {
            const long count = channel.send(text.data() + done, text.size() - done);
            if (count <= 0)
                return false;
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    bool line(std::string &out)
    {
        for (;;)
        {
            const auto end = pending.find('\n');
            if (end != pending.npos)
            {
                out = pending.substr(0, end);
                pending.erase(0, end + 1);
                return true;
            }
            if (pending.size() > kWorkerLine)
                return false;
            char buffer[512];
            const long count = channel.receive(buffer, sizeof(buffer));
            if (count <= 0)
                return false;
            pending.append(buffer, static_cast<std::size_t>(count));
        }
    }
};

// Starts a worker and hands it the request. False: nothing was touched.
bool begin(const Connect &connect, Session &session, const std::string &request)
{
    if (!connect || !connect(session.channel))
        return false;
    session.open = true;
    std::string reply;
    return session.send(request) && session.line(reply) && reply == "ready";
}

// Follows the worker to its last line. progress and cancelled may be null.
bool follow(Session &session, const std::atomic<bool> *cancelled,
            std::atomic<std::uint64_t> *progress, std::string &last)
{
    bool told = false;
    for (;;)
    {
        if (cancelled && cancelled->load() && !told)
        {
            told = true;
            (void)session.send("c\n");
        }
        std::string reply;
        if (!session.line(reply))
            return false;
        if (reply.rfind("p ", 0) != 0)
        {
            last = reply;
            return true;
        }
        if (progress)
            *progress = std::strtoull(reply.c_str() + 2, nullptr, 10);
    }
}
} // namespace

int worker_extract(const Connect &connect, const std::string &archive, std::string_view title,
                   const std::string &destination, const std::atomic<bool> &cancelled,
                   std::atomic<std::uint64_t> &written, std::string &error, ExtractTimes &times)
{
    if (!worker_path(archive) || !worker_path(destination))
        return -1;
    Session session;
    std::string request(kWorkerMagic);
    request += "\nextract\n" + archive + "\n" + std::string(title) + "\n" + destination + "\n";
    if (!begin(connect, session, request))
        return -1;
    std::string last;
    if (!follow(session, &cancelled, &written, last))
    {
        error = cancelled.load() ? "Cancelled" : "The app could not be unpacked";
        return 0;
    }
    if (last.rfind("ok ", 0) == 0)
    {
        char *cursor = last.data() + 3;
        times.write_ms = std::strtoull(cursor, &cursor, 10);
        times.sync_ms = std::strtoull(cursor, &cursor, 10);
        times.total_ms = std::strtoull(cursor, &cursor, 10);
        times.files = static_cast<std::size_t>(std::strtoull(cursor, &cursor, 10));
        written = std::strtoull(cursor, &cursor, 10);
        error.clear();
        return 1;
    }
    error = last.rfind("fail ", 0) == 0 && last.size() > 5 ? last.substr(5)
                                                           : "The app could not be unpacked";
    return 0;
}

bool worker_store(const Connect &connect, const std::string &path, Writer &writer)
{
    if (!worker_path(path))
        return false;
    auto session = std::make_shared<Session>();
    std::string request(kWorkerMagic);
    request += "\nstore\n" + path + "\n";
    if (!begin(connect, *session, request))
        return false;
    const auto piece = [session](const char *data, std::uint32_t size)
    {
        const unsigned char header[4] = {
            static_cast<unsigned char>(size), static_cast<unsigned char>(size >> 8),
            static_cast<unsigned char>(size >> 16), static_cast<unsigned char>(size >> 24)};
        std::string out(reinterpret_cast<const char *>(header), 4);
        out.append(data, size);
        return session->send(out);
    };
    writer.write = [piece](std::string_view data)
    {
        while (!data.empty())
        {
            const auto size = std::min(data.size(), kWorkerPiece);
            if (!piece(data.data(), static_cast<std::uint32_t>(size)))
                return false;
            data.remove_prefix(size);
        }
        return true;
    };
    writer.finish = [piece, session]
    {
        std::string reply;
        return piece(nullptr, 0) && session->line(reply) && reply == "ok";
    };
    return true;
}

bool title_id_plain(std::string_view title)
{
    if (title.size() != 9)
        return false;
    for (std::size_t i = 0; i < title.size(); ++i)
        if (i < 4 ? (title[i] < 'A' || title[i] > 'Z') : (title[i] < '0' || title[i] > '9'))
            return false;
    return true;
}

int worker_unregister(const Connect &connect, const std::string &title, int &code)
{
    code = -1;
    if (!title_id_plain(title))
        return -1;
    Session session;
    std::string request(kWorkerMagic);
    request += "\nunregister\n" + title + "\n";
    if (!begin(connect, session, request))
        return -1;
    std::string last;
    (void)follow(session, nullptr, nullptr, last);
    if (last == "ok")
    {
        code = 0;
        return 1;
    }
    if (last.rfind("fail ", 0) == 0)
        code = static_cast<int>(std::strtoul(last.c_str() + 5, nullptr, 16));
    return 0;
}

bool worker_folder(std::string_view path)
{
    constexpr std::string_view name = "/sahin";
    // The same rules as a path inside it, which is what the folder plus a name is.
    return path.size() > name.size() && path.ends_with(name) &&
           worker_path(std::string(path) + "/x");
}

int worker_reclaim(const Connect &connect, const std::string &folder)
{
    if (!worker_folder(folder))
        return -1;
    Session session;
    std::string request(kWorkerMagic);
    request += "\nreclaim\n" + folder + "\n";
    if (!begin(connect, session, request))
        return -1;
    std::string last;
    return follow(session, nullptr, nullptr, last) && last == "ok" ? 1 : 0;
}

int worker_remove(const Connect &connect, const std::string &path)
{
    if (!worker_path(path))
        return -1;
    Session session;
    std::string request(kWorkerMagic);
    request += "\nremove\n" + path + "\n";
    if (!begin(connect, session, request))
        return -1;
    std::string last;
    return follow(session, nullptr, nullptr, last) && last == "ok" ? 1 : 0;
}
} // namespace store::install
