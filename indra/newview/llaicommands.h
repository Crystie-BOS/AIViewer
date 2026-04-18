/**
 * llaicommands.h
 * Dispatches JSON commands from the AI socket to viewer functions.
 */

#ifndef LL_LLAICOMMANDS_H
#define LL_LLAICOMMANDS_H

#include "llsingleton.h"
#include "llsd.h"
#include "llvector3d.h"
#include <deque>
#include <functional>

class LLAICommands : public LLSingleton<LLAICommands>
{
    LLSINGLETON(LLAICommands);

public:
    void start();
    void stop();

    // Schedule a one-shot action for the next idle tick (used for DeGrab delay).
    void deferOnce(std::function<void()> fn);

private:
    static void idleCB(void* data);
    void onIdle();
    void tickNavigation();

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
    void cmdGetObjects(const LLSD& p);
    void cmdTouch(const LLSD& p);
    void cmdRequestObjectProps(const LLSD& p);
    void cmdNavigateTo(const LLSD& p);
    void cmdNavigateStop(const LLSD& p);
    void cmdPause(const LLSD& p);
    void cmdResume(const LLSD& p);
    void cmdDismissDialog(const LLSD& p);

    void sendOk(const std::string& cmd);
    void sendError(const std::string& cmd, const std::string& reason);

    bool mRegistered { false };

    // Deferred one-shot actions
    std::deque<std::function<void()>> mDeferred;

    // Navigation state machine
    struct NavState
    {
        bool      active    { false };
        LLVector3d target;
        F32       stopDist  { 2.f };
        LLVector3d lastPos;
        F64       lastMoveTime { 0.0 };
        int       stuckCount   { 0 };
    } mNav;
};

#endif // LL_LLAICOMMANDS_H
