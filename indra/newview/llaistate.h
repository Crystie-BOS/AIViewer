/**
 * llaistate.h
 * Publishes structured state snapshots and transition events to the AI socket.
 */

#ifndef LL_LLAISTATE_H
#define LL_LLAISTATE_H

#include "llsingleton.h"
#include "llsd.h"
#include <string>
#include <map>
#include "lluuid.h"

class LLAIState : public LLSingleton<LLAIState>
{
    LLSINGLETON(LLAIState);

public:
    // Register/unregister the idle callback.
    void start();
    void stop();

    // Force an immediate state_tick (used by get_state command).
    void publishStateNow();

    // Force an immediate nearby_avatars event.
    void publishNearbyNow();

    // Return the current structured viewer state without emitting it.
    LLSD buildStateSnapshot() const;
    U32 getLastPublishedStateSeq() const;
    LLSD getLastPublishedState() const;

    // Enable/disable continuous frame streaming.  hz is frames per second.
    void setAutoCaptureMode(bool enabled, float hz = 1.5f);

    // Smoothly rotate the avatar body toward a heading via the idle loop.
    void startTurnToHeading(F32 target_heading_deg, F32 tolerance_deg = 2.0f);
    void cancelTurn();

private:
    static void idleCB(void* data);
    void onIdle();
    void updateTurnController();

    void publishState();
    void publishNearby();

    F64  mLastStateTick { 0.0 };   // timestamp of last state_tick send
    F64  mLastNearbyTick { 0.0 };  // timestamp of last nearby_avatars send
    U32  mLastPublishedStateSeq { 0 };
    LLSD mLastPublishedState;

    // Transition detection
    int         mLastTeleportState { 0 };
    std::string mLastRegion;

    // Track per-avatar fully-loaded state so we can emit avatar_appearance_loaded
    std::map<LLUUID, bool> mLastFullyLoadedState;

    // Continuous frame streaming
    bool  mAutoCaptureEnabled { false };
    float mAutoCaptureHz      { 1.5f  };
    F64   mLastCaptureTick    { 0.0   };
    void  captureAndStream();

    // Smooth heading controller
    bool  mTurnActive         { false };
    F32   mTurnTargetHeading  { 0.0f  };
    F32   mTurnToleranceDeg   { 2.0f  };

    bool mRegistered { false };
};

#endif // LL_LLAISTATE_H
