/**
 * llaisocket.cpp
 * Unix domain socket server — Linux only.
 */

#include "llviewerprecompiledheaders.h"
#include "llaisocket.h"
#include "llsd.h"
#include "llsdjson.h"

#if LL_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <string_view>
#include <boost/json.hpp>
#endif

static constexpr const char* LOG_AI = "AI";

#if LL_LINUX
namespace
{
// Per-client outbound backlog limits.
//
// A message is never abandoned half-written: the old writer gave up after a
// 1 s poll and left a truncated line in the stream, which the controller read
// as the start of a very long line -- it lost her state feed for minutes at a
// time (2026-09-30, seen as "turn freezes"). Instead each client keeps its
// own buffer and the socket takes it as fast as it can.
//
// Past the soft cap, automatic camera frames are skipped whole (the next is
// under a second away; state, replies and requested captures never are).
// Past the hard cap the client is too far behind to be useful: it is closed
// and reconnects cleanly.
constexpr size_t AI_OUT_SOFT_CAP = 4u * 1024u * 1024u;
constexpr size_t AI_OUT_HARD_CAP = 32u * 1024u * 1024u;

bool isAutoCameraFrame(const std::string& msg)
{
    // LLSD maps serialise with sorted keys, so "auto" and "event" lead.
    const std::string_view head(msg.data(), std::min<size_t>(msg.size(), 256));
    return head.find("\"perception_capture\"") != std::string_view::npos
        && head.find("\"auto\":true") != std::string_view::npos;
}
}
#endif

LLAISocket::LLAISocket()  = default;
LLAISocket::~LLAISocket() { stop(); }

// ── public ──────────────────────────────────────────────────────────────────

void LLAISocket::start()
{
#if LL_LINUX
    if (mServerFd >= 0) return; // already running

    mSockPath = "/tmp/ai-viewer-" + std::to_string(::getpid()) + ".sock";
    ::unlink(mSockPath.c_str()); // remove stale socket

    mServerFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (mServerFd < 0)
    {
        LL_WARNS(LOG_AI) << "AI socket: socket() failed: " << ::strerror(errno) << LL_ENDL;
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, mSockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(mServerFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        LL_WARNS(LOG_AI) << "AI socket: bind() failed: " << ::strerror(errno) << LL_ENDL;
        ::close(mServerFd);
        mServerFd = -1;
        return;
    }

    ::listen(mServerFd, 4);
    mShouldStop = false;
    mThread = std::thread(&LLAISocket::socketThread, this);
    LL_INFOS(LOG_AI) << "AI socket listening at " << mSockPath << LL_ENDL;
#endif
}

void LLAISocket::stop()
{
#if LL_LINUX
    mShouldStop = true;
    if (mThread.joinable()) mThread.join();
    if (mServerFd >= 0)
    {
        ::close(mServerFd);
        mServerFd = -1;
        ::unlink(mSockPath.c_str());
    }
#endif
}

void LLAISocket::sendEvent(const LLSD& event)
{
#if LL_LINUX
    try
    {
        boost::json::value jv = LlsdToJson(event);
        std::string line = boost::json::serialize(jv) + "\n";
        std::lock_guard<std::mutex> lk(mOutMutex);
        mOutQueue.push_back(std::move(line));
    }
    catch (...)
    {
        LL_WARNS(LOG_AI) << "AI socket: sendEvent serialization failed" << LL_ENDL;
    }
#endif
}

bool LLAISocket::pollCommand(LLSD& out_cmd)
{
#if LL_LINUX
    std::lock_guard<std::mutex> lk(mCmdMutex);
    if (mCmdQueue.empty()) return false;
    out_cmd = mCmdQueue.front();
    mCmdQueue.pop_front();
    return true;
#else
    return false;
#endif
}

// ── private (Linux only) ─────────────────────────────────────────────────────

#if LL_LINUX

void LLAISocket::socketThread()
{
    // Greeting for a newly connected client (sent with the next flush)
    auto greetingFor = [&]() {
        boost::json::object greet;
        greet["event"]   = "connected";
        greet["version"] = "1.0";
        greet["sock"]    = mSockPath;
        return boost::json::serialize(greet) + "\n";
    };

    while (!mShouldStop)
    {
        // Build pollfd array: server + all clients; ask to hear when a client
        // with a backlog can take more.
        std::vector<struct pollfd> pfds;
        pfds.push_back({ mServerFd, POLLIN, 0 });
        for (auto& c : mClients)
        {
            short events = POLLIN;
            if (c.outOff < c.outBuf.size())
                events |= POLLOUT;
            pfds.push_back({ c.fd, events, 0 });
        }

        int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100 /*ms*/);
        if (ret < 0 && errno != EINTR) break;

        std::vector<int> toRemove;
        if (ret > 0)
        {
            // Read from the clients polled above (a client accepted below is
            // not in pfds yet)
            const size_t polled = pfds.size() - 1;
            for (size_t i = 0; i < polled; ++i)
            {
                if (pfds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))
                    handleClientRead(mClients[i], toRemove);
            }

            // Accept new connection?
            if (pfds[0].revents & POLLIN)
            {
                int cfd = ::accept4(mServerFd, nullptr, nullptr, SOCK_NONBLOCK);
                if (cfd >= 0)
                {
                    ClientConn conn;
                    conn.fd = cfd;
                    queueForClient(conn, greetingFor());
                    mClients.push_back(std::move(conn));
                    mClientCount.store((int)mClients.size());
                    LL_INFOS(LOG_AI) << "AI socket: client connected (fd " << cfd << ")" << LL_ENDL;
                }
            }
        }

        // Queue new events and write whatever each socket will take now
        flushOutbound(toRemove);

        // Remove disconnected clients
        if (!toRemove.empty())
        {
            mClients.erase(
                std::remove_if(mClients.begin(), mClients.end(),
                    [&](const ClientConn& c) {
                        return std::find(toRemove.begin(), toRemove.end(), c.fd) != toRemove.end();
                    }),
                mClients.end());
            mClientCount.store((int)mClients.size());
        }
    }

    // Cleanup
    for (auto& c : mClients) ::close(c.fd);
    mClients.clear();
    mClientCount.store(0);
}

void LLAISocket::flushOutbound(std::vector<int>& to_remove)
{
    std::deque<std::string> out;
    {
        std::lock_guard<std::mutex> lk(mOutMutex);
        out.swap(mOutQueue);
    }

    for (auto& c : mClients)
    {
        if (std::find(to_remove.begin(), to_remove.end(), c.fd) != to_remove.end())
            continue;
        const size_t skippedBefore = c.skippedFrames;
        for (auto& msg : out)
            queueForClient(c, msg);
        if (c.skippedFrames != skippedBefore && (skippedBefore == 0 || c.skippedFrames / 100 != skippedBefore / 100))
        {
            LL_INFOS(LOG_AI) << "AI socket: client fd " << c.fd << " is behind ("
                             << (c.outBuf.size() - c.outOff) << " bytes queued); skipped "
                             << c.skippedFrames << " camera frames so far" << LL_ENDL;
        }
        if (c.overflow)
        {
            LL_WARNS(LOG_AI) << "AI socket: client fd " << c.fd << " fell " << (c.outBuf.size() - c.outOff)
                             << " bytes behind; closing it so it reconnects clean" << LL_ENDL;
            ::close(c.fd);
            to_remove.push_back(c.fd);
            continue;
        }
        if (!drainClient(c))
        {
            LL_INFOS(LOG_AI) << "AI socket: client fd " << c.fd << " write failed: " << ::strerror(errno) << LL_ENDL;
            ::close(c.fd);
            to_remove.push_back(c.fd);
        }
    }
}

void LLAISocket::queueForClient(ClientConn& c, const std::string& msg)
{
    const size_t pending = c.outBuf.size() - c.outOff;
    if (pending + msg.size() > AI_OUT_HARD_CAP)
    {
        c.overflow = true;
        return;
    }
    if (pending > AI_OUT_SOFT_CAP && isAutoCameraFrame(msg))
    {
        ++c.skippedFrames;
        return;
    }
    if (c.outOff > 0 && c.outOff * 2 >= c.outBuf.size())
    {
        c.outBuf.erase(0, c.outOff);
        c.outOff = 0;
    }
    c.outBuf.append(msg);
}

bool LLAISocket::drainClient(ClientConn& c)
{
    while (c.outOff < c.outBuf.size())
    {
        const ssize_t written = ::send(
            c.fd,
            c.outBuf.data() + c.outOff,
            c.outBuf.size() - c.outOff,
            MSG_NOSIGNAL);
        if (written > 0)
        {
            c.outOff += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;  // the rest goes when the socket can take it
        return false;
    }
    c.outBuf.clear();
    c.outOff = 0;
    return true;
}

void LLAISocket::handleClientRead(ClientConn& c, std::vector<int>& to_remove)
{
    char buf[4096];
    ssize_t n = ::read(c.fd, buf, sizeof(buf));
    if (n <= 0)
    {
        LL_INFOS(LOG_AI) << "AI socket: client fd " << c.fd << " disconnected" << LL_ENDL;
        ::close(c.fd);
        to_remove.push_back(c.fd);
        return;
    }

    c.readBuf.append(buf, n);

    // Process complete lines
    size_t pos;
    while ((pos = c.readBuf.find('\n')) != std::string::npos)
    {
        std::string line = c.readBuf.substr(0, pos);
        c.readBuf.erase(0, pos + 1);
        if (line.empty()) continue;

        LLSD cmd;
        if (parseJsonLine(line, cmd))
        {
            std::lock_guard<std::mutex> lk(mCmdMutex);
            mCmdQueue.push_back(cmd);
        }
        else
        {
            // Send parse error back to client
            boost::json::object err;
            err["event"]  = "error";
            err["reason"] = "json_parse_failed";
            std::string errMsg = boost::json::serialize(err) + "\n";
            queueForClient(c, errMsg);
        }
    }
}

bool LLAISocket::parseJsonLine(const std::string& line, LLSD& out)
{
    try
    {
        boost::json::value jv = boost::json::parse(line);
        out = LlsdFromJson(jv);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

#endif // LL_LINUX
