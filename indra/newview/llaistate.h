/**
 * llaistate.h
 * Publishes structured state snapshots and transition events to the AI socket.
 */

#ifndef LL_LLAISTATE_H
#define LL_LLAISTATE_H

#include "llsingleton.h"
#include <string>

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

private:
    static void idleCB(void* data);
    void onIdle();

    void publishState();
    void publishNearby();

    F64  mLastStateTick { 0.0 };   // timestamp of last state_tick send
    F64  mLastNearbyTick { 0.0 };  // timestamp of last nearby_avatars send

    // Transition detection
    int         mLastTeleportState { 0 };
    std::string mLastRegion;

    bool mRegistered { false };
};

#endif // LL_LLAISTATE_H
