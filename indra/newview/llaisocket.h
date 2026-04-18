/**
 * llaisocket.h
 * Unix domain socket server for AI agent control API.
 * Linux only — compiles to empty stubs on other platforms.
 */

#ifndef LL_LLAISOCKET_H
#define LL_LLAISOCKET_H

#include "llsd.h"
#include "llsingleton.h"
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LLAISocket : public LLSingleton<LLAISocket>
{
    LLSINGLETON(LLAISocket);
    ~LLAISocket();

public:
    // Called from LLAppViewer::handleLoginComplete()
    void start();

    // Called from LLAppViewer shutdown path
    void stop();

    // Thread-safe: queue a JSON event line to all connected clients.
    void sendEvent(const LLSD& event);

    // Called from idle callback on main thread.
    // Returns true and fills out_cmd if a command is waiting.
    bool pollCommand(LLSD& out_cmd);

    std::string getSockPath() const { return mSockPath; }

private:
#if LL_LINUX
    struct ClientConn
    {
        int fd;
        std::string readBuf; // partial line accumulator
    };

    void socketThread();
    void flushOutbound();         // writes mOutQueue to all clients (called inside thread)
    void handleClientRead(ClientConn& c, std::vector<int>& to_remove);
    bool parseJsonLine(const std::string& line, LLSD& out);

    int              mServerFd  { -1 };
    std::string      mSockPath;
    std::thread      mThread;
    std::atomic_bool mShouldStop { false };

    // Outbound: main thread appends, socket thread drains
    std::mutex              mOutMutex;
    std::deque<std::string> mOutQueue;

    // Inbound: socket thread appends, main thread drains
    std::mutex   mCmdMutex;
    std::deque<LLSD> mCmdQueue;

    std::vector<ClientConn> mClients;
#endif
};

#endif // LL_LLAISOCKET_H
