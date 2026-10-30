/**
 * llaicommands.h
 * Dispatches JSON commands from the AI socket to viewer functions.
 */

#ifndef LL_LLAICOMMANDS_H
#define LL_LLAICOMMANDS_H
#include <string>
#include <vector>
#include <set>

#include "llsingleton.h"
#include "llsd.h"
#include "v3dmath.h"
#include "v3math.h"
#include "lluuid.h"
#include <deque>
#include <functional>

class LLViewerRegion;

class LLAICommands : public LLSingleton<LLAICommands>
{
    LLSINGLETON(LLAICommands);

public:
    void start();
    void stop();

    // Schedule a one-shot action for the next idle tick (used for DeGrab delay).
    void deferOnce(std::function<void()> fn);
    // Runs every tick until it returns true (a held click's release).
    void deferTimed(std::function<bool()> fn) { mTimed.push_back(std::move(fn)); }

private:
    static void idleCB(void* data);
    void onIdle();
    void tickHeldMove();
    void tickMovePulse();
    void tickNavigation();

    void dispatch(const LLSD& cmd);

    // Command handlers
    void cmdMove(const LLSD& p);
    void cmdMovePulse(const LLSD& p);
    void cmdTurn(const LLSD& p);
    void cmdFace(const LLSD& p);
    void cmdCameraLook(const LLSD& p);
    void cmdCameraPlace(const LLSD& p);
    void cmdCameraFollow(const LLSD& p);
    void tickFollowCamera();
    void cmdChat(const LLSD& p);
    void cmdIM(const LLSD& p);
    void cmdTeleport(const LLSD& p);
    void cmdTeleportLandmark(const LLSD& p);
    void cmdSit(const LLSD& p);
    void cmdStand(const LLSD& p);
    void cmdScreenshot(const LLSD& p);
    void cmdCapturePerception(const LLSD& p);
    void cmdGetState(const LLSD& p);
    void cmdGetNearby(const LLSD& p);
    void cmdGetObjects(const LLSD& p);
    void cmdTouch(const LLSD& p);
    void cmdPickAt(const LLSD& p);
    void cmdClickAt(const LLSD& p);
    void cmdTouchables(const LLSD& p);
    void cmdClickObject(const LLSD& p);
    void cmdRequestObjectProps(const LLSD& p);
    void cmdNavigateTo(const LLSD& p);
    void cmdNavigateStop(const LLSD& p);
    void cmdPause(const LLSD& p);
    void cmdResume(const LLSD& p);
    void cmdDismissDialog(const LLSD& p);
    void cmdRespondDialog(const LLSD& p);
    void cmdSetAutoCapture(const LLSD& p);
    void cmdGetParcelInfo(const LLSD& p);
    void cmdMapGrid(const LLSD& p);
    void cmdLookAhead(const LLSD& p);
    void cmdFollowPath(const LLSD& p);
    void tickFollow();
    void endFollow(const std::string& event, const std::string& reason);
    void cmdGetAttachments(const LLSD& p);
    void cmdListInventory(const LLSD& p);
    void cmdWearItem(const LLSD& p);
    void cmdRemoveItem(const LLSD& p);
    void cmdWearOutfit(const LLSD& p);
    void cmdRezItem(const LLSD& p);

    void sendOk(const std::string& cmd);
    void sendError(const std::string& cmd, const std::string& reason);

    bool mRegistered { false };

    // Deferred one-shot actions
    std::deque<std::function<void()>> mDeferred;
    std::deque<std::function<bool()>> mTimed;

    struct MoveState
    {
        bool active { false };
        std::string dir;
    } mMove;

    struct MovePulseState
    {
        bool active { false };
        bool started { false };
        std::string dir;
        std::string id;
        S32 frames { 0 };
        S32 framesApplied { 0 };
        S32 stopFrames { 4 };
        S32 stopFramesApplied { 0 };
        S32 settleFrames { 0 };
        S32 settleFramesApplied { 0 };
        F64 maxDistance { 0.12 };
        F64 maxDrop { 0.08 };
        F64 maxSettleVelocity { 0.15 };
        bool failed { false };
        std::string failReason;
        LLVector3d startPos;
        std::string startRegion;
        U32 lastConsumedStateSeq { 0 };
    } mMovePulse;

    // Her eyes go with her (camera_follow): each frame the camera sits behind
    // her head, pulled in front of anything solid between it and her.
    struct FollowCamState
    {
        bool       active    { false };
        F32        distance  { 2.5f };
        F32        pitch     { -8.f };
        F32        focusDist { 6.f };
        bool       hasLast   { false };
        LLVector3d lastCam;
    } mFollowCam;

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

    // follow_path: the viewer walks a whole route, steering every frame.
    struct FollowState
    {
        bool                    active { false };
        std::string             id;
        LLViewerRegion*         region { nullptr };
        std::vector<LLVector3>  pts;          // region metres
        S32                     idx { 0 };
        F32                     stopDist { 1.f }, look { 2.5f }, guard { 1.f }, drop { 1.5f };
        std::set<LLUUID>        avoid;        // objects that are water to her
        F64                     lastCheck { 0.0 }, lastReport { 0.0 }, lastMoveTime { 0.0 };
        LLVector3d              lastPos;
    } mFollow;
};

#endif // LL_LLAICOMMANDS_H
