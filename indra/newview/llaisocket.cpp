/**
 * llaisocket.cpp
 * Unix domain socket server — Linux only.
 */

#include "llviewerprecompiledheaders.h"
#include "llaisocket.h"
#include "llsd.h"
#include "llsdjson.h"
#include "lllog.h"

#if LL_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <boost/json.hpp>
#endif

static const std::string LOG_AI("AI");

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
    // Send a greeting to a newly connected client fd
    auto sendGreeting = [&](int fd) {
        boost::json::object greet;
        greet["event"]   = "connected";
        greet["version"] = "1.0";
        greet["sock"]    = mSockPath;
        std::string msg  = boost::json::serialize(greet) + "\n";
        ::write(fd, msg.c_str(), msg.size());
    };

    while (!mShouldStop)
    {
        // Build pollfd array: server + all clients
        std::vector<struct pollfd> pfds;
        pfds.push_back({ mServerFd, POLLIN, 0 });
        for (auto& c : mClients)
            pfds.push_back({ c.fd, POLLIN, 0 });

        int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100 /*ms*/);
        if (ret < 0 && errno != EINTR) break;

        if (ret > 0)
        {
            // Accept new connection?
            if (pfds[0].revents & POLLIN)
            {
                int cfd = ::accept4(mServerFd, nullptr, nullptr, SOCK_NONBLOCK);
                if (cfd >= 0)
                {
                    mClients.push_back({ cfd, {} });
                    sendGreeting(cfd);
                    LL_INFOS(LOG_AI) << "AI socket: client connected (fd " << cfd << ")" << LL_ENDL;
                }
            }

            // Read from clients
            std::vector<int> toRemove;
            for (size_t i = 0; i < mClients.size(); ++i)
            {
                if (pfds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))
                    handleClientRead(mClients[i], toRemove);
            }

            // Remove disconnected clients
            if (!toRemove.empty())
            {
                mClients.erase(
                    std::remove_if(mClients.begin(), mClients.end(),
                        [&](const ClientConn& c) {
                            return std::find(toRemove.begin(), toRemove.end(), c.fd) != toRemove.end();
                        }),
                    mClients.end());
            }
        }

        // Flush outbound queue to all clients
        flushOutbound();
    }

    // Cleanup
    for (auto& c : mClients) ::close(c.fd);
    mClients.clear();
}

void LLAISocket::flushOutbound()
{
    std::deque<std::string> out;
    {
        std::lock_guard<std::mutex> lk(mOutMutex);
        out.swap(mOutQueue);
    }
    if (out.empty() || mClients.empty()) return;

    for (auto& msg : out)
        for (auto& c : mClients)
            ::write(c.fd, msg.c_str(), msg.size());
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
            ::write(c.fd, errMsg.c_str(), errMsg.size());
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
