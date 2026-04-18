/**
 * llaicommands.h
 * Dispatches JSON commands from the AI socket to viewer functions.
 */

#ifndef LL_LLAICOMMANDS_H
#define LL_LLAICOMMANDS_H

#include "llsingleton.h"
#include "llsd.h"

class LLAICommands : public LLSingleton<LLAICommands>
{
    LLSINGLETON(LLAICommands);

public:
    // Register/unregister idle callback.
    void start();
    void stop();

private:
    static void idleCB(void* data);
    void onIdle();

    void dispatch(const LLSD& cmd);

    // Command handlers
    void cmdMove(const LLSD& p);
    void cmdTurn(const LLSD& p);
    void cmdFace(const LLSD& p);
    void cmdChat(const LLSD& p);
    void cmdIM(const LLSD& p);
    void cmdTeleport(const LLSD& p);
    void cmdTeleportLandmark(const LLSD& p);
    void cmdSit(const LLSD& p);
    void cmdStand(const LLSD& p);
    void cmdScreenshot(const LLSD& p);
    void cmdGetState(const LLSD& p);
    void cmdGetNearby(const LLSD& p);
    void cmdPause(const LLSD& p);
    void cmdResume(const LLSD& p);
    void cmdDismissDialog(const LLSD& p);

    void sendOk(const std::string& cmd);
    void sendError(const std::string& cmd, const std::string& reason);

    bool mRegistered { false };
};

#endif // LL_LLAICOMMANDS_H
