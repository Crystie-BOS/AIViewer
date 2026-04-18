/**
 * llaistate.cpp
 * State publisher: idle callback → JSON events via LLAISocket.
 */

#include "llviewerprecompiledheaders.h"
#include "llaistate.h"
#include "llaisocket.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llviewercamera.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llsd.h"
#include "llcoordframe.h"
#include <cmath>

static const std::string LOG_AI("AI");
static const F64 STATE_TICK_HZ  = 10.0; // state_tick frequency
static const F64 NEARBY_TICK_S  = 2.0;  // nearby_avatars update interval (seconds)

LLAIState::LLAIState() = default;

void LLAIState::start()
{
    if (!mRegistered)
    {
        gIdleCallbacks.addFunction(&LLAIState::idleCB, this);
        mRegistered = true;
        LL_INFOS(LOG_AI) << "AI state publisher started" << LL_ENDL;
    }
}

void LLAIState::stop()
{
    if (mRegistered)
    {
        gIdleCallbacks.deleteFunction(&LLAIState::idleCB, this);
        mRegistered = false;
    }
}

// ── idle callback ────────────────────────────────────────────────────────────

/*static*/ void LLAIState::idleCB(void* data)
{
    static_cast<LLAIState*>(data)->onIdle();
}

void LLAIState::onIdle()
{
    F64 now = LLTimer::getElapsedSeconds();

    if ((now - mLastStateTick) >= (1.0 / STATE_TICK_HZ))
    {
        mLastStateTick = now;
        publishState();
    }

    if ((now - mLastNearbyTick) >= NEARBY_TICK_S)
    {
        mLastNearbyTick = now;
        publishNearby();
    }
}

// ── state_tick ───────────────────────────────────────────────────────────────

void LLAIState::publishState()
{
    if (!gAgent.getRegion()) return; // not logged in yet

    // Position
    LLVector3d posGlobal = gAgent.getPositionGlobal();
    LLVector3  vel       = gAgent.getVelocity();

    // Heading: XY angle of agent's forward axis, degrees from +Y (north)
    LLVector3 atAxis = gAgent.getFrameAgent().getAtAxis();
    F32 headingDeg   = (F32)(std::atan2(atAxis.mV[VX], atAxis.mV[VY]) * RAD_TO_DEG);
    if (headingDeg < 0) headingDeg += 360.f;

    // Camera
    LLVector3 camPos = LLViewerCamera::instance().getOrigin();
    LLVector3 camDir = LLViewerCamera::instance().getAtAxis();
    F32       camFov = LLViewerCamera::instance().getView() * RAD_TO_DEG;

    // Region / parcel
    std::string regionName = gAgent.getRegion()->getName();
    std::string parcelName;
    LLParcel* parcel = LLViewerParcelMgr::instance().getAgentParcel();
    if (parcel) parcelName = parcel->getName();

    // Teleport state
    int tpState = (int)gAgent.getTeleportState();

    // Sitting
    bool sitting = gAgentAvatarp.notNull() && gAgentAvatarp->isSitting();

    // Assemble LLSD
    LLSD ev;
    ev["event"]   = "state_tick";
    ev["ts"]      = (LLSD::Integer)(S32)LLTimer::getTotalSeconds();
    ev["pos"]     = LLSD::emptyArray();
    ev["pos"][0]  = posGlobal.mdV[VX];
    ev["pos"][1]  = posGlobal.mdV[VY];
    ev["pos"][2]  = posGlobal.mdV[VZ];
    ev["vel"]     = LLSD::emptyArray();
    ev["vel"][0]  = vel.mV[VX];
    ev["vel"][1]  = vel.mV[VY];
    ev["vel"][2]  = vel.mV[VZ];
    ev["heading"] = headingDeg;
    ev["cam_pos"] = LLSD::emptyArray();
    ev["cam_pos"][0] = camPos.mV[VX];
    ev["cam_pos"][1] = camPos.mV[VY];
    ev["cam_pos"][2] = camPos.mV[VZ];
    ev["cam_dir"] = LLSD::emptyArray();
    ev["cam_dir"][0] = camDir.mV[VX];
    ev["cam_dir"][1] = camDir.mV[VY];
    ev["cam_dir"][2] = camDir.mV[VZ];
    ev["cam_fov"]         = camFov;
    ev["region"]          = regionName;
    ev["parcel"]          = parcelName;
    ev["teleport_state"]  = tpState;
    ev["sitting"]         = sitting;

    LLAISocket::instance().sendEvent(ev);

    // Detect teleport state transitions
    if (tpState != mLastTeleportState)
    {
        LLSD tev;
        if (tpState == LLAgent::TELEPORT_START || tpState == LLAgent::TELEPORT_REQUESTED)
        {
            tev["event"] = "teleport_start";
        }
        else if (tpState == LLAgent::TELEPORT_NONE && mLastTeleportState >= LLAgent::TELEPORT_MOVING)
        {
            tev["event"]  = "teleport_ok";
            tev["region"] = regionName;
            tev["pos"]    = ev["pos"];
        }
        if (!tev.isUndefined()) LLAISocket::instance().sendEvent(tev);
        mLastTeleportState = tpState;
    }

    // Detect region crossing
    if (regionName != mLastRegion && !mLastRegion.empty())
    {
        LLSD rev;
        rev["event"] = "region_crossed";
        rev["from"]  = mLastRegion;
        rev["to"]    = regionName;
        LLAISocket::instance().sendEvent(rev);
    }
    mLastRegion = regionName;
}

// ── nearby_avatars ───────────────────────────────────────────────────────────

void LLAIState::publishNearby()
{
    if (!gAgent.getRegion()) return;

    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::instance().getAvatars(&ids, &positions, gAgent.getPositionGlobal(), 96.f /*meters*/);

    LLSD ev;
    ev["event"]   = "nearby_avatars";
    ev["avatars"] = LLSD::emptyArray();

    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (ids[i] == gAgent.getID()) continue; // skip self

        LLVector3d delta = positions[i] - gAgent.getPositionGlobal();
        F64 dist = delta.length();

        // Name lookup — best-effort from cache
        std::string name;
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(ids[i], &av_name))
            name = av_name.getDisplayName();

        LLSD entry;
        entry["id"]   = ids[i].asString();
        entry["name"] = name;
        entry["pos"]  = LLSD::emptyArray();
        entry["pos"][0] = positions[i].mdV[VX];
        entry["pos"][1] = positions[i].mdV[VY];
        entry["pos"][2] = positions[i].mdV[VZ];
        entry["dist"] = dist;
        ev["avatars"].append(entry);
    }

    LLAISocket::instance().sendEvent(ev);
}

// ── public force-publish ─────────────────────────────────────────────────────

void LLAIState::publishStateNow()  { publishState();  }
void LLAIState::publishNearbyNow() { publishNearby(); }
