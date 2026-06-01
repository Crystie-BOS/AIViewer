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
#include "llviewerwindow.h"
#include "llselectmgr.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llsd.h"
#include "llcoordframe.h"
#include "llimagejpeg.h"
#include "llbase64.h"
#include <cmath>

static constexpr const char* LOG_AI = "AI";
static const F64 STATE_TICK_HZ  = 10.0; // state_tick frequency
static const F64 NEARBY_TICK_S  = 2.0;  // nearby_avatars update interval (seconds)

namespace
{
F32 normalize_heading_deg(F32 heading)
{
    while (heading < 0.f) heading += 360.f;
    while (heading >= 360.f) heading -= 360.f;
    return heading;
}

F32 current_heading_deg()
{
    LLVector3 at_axis = gAgent.getFrameAgent().getAtAxis();
    return normalize_heading_deg((F32)(std::atan2(at_axis.mV[VX], at_axis.mV[VY]) * RAD_TO_DEG));
}

F32 shortest_heading_delta(F32 current_heading, F32 target_heading)
{
    F32 delta = target_heading - current_heading;
    while (delta > 180.f) delta -= 360.f;
    while (delta < -180.f) delta += 360.f;
    return delta;
}

LLSD vector3_to_array(const LLVector3& value)
{
    LLSD out = LLSD::emptyArray();
    out[0] = value.mV[VX];
    out[1] = value.mV[VY];
    out[2] = value.mV[VZ];
    return out;
}

LLSD vector3d_to_array(const LLVector3d& value)
{
    LLSD out = LLSD::emptyArray();
    out[0] = value.mdV[VX];
    out[1] = value.mdV[VY];
    out[2] = value.mdV[VZ];
    return out;
}

const char* camera_mode_name(ECameraMode mode)
{
    switch (mode)
    {
    case CAMERA_MODE_THIRD_PERSON: return "third_person";
    case CAMERA_MODE_MOUSELOOK: return "mouselook";
    case CAMERA_MODE_CUSTOMIZE_AVATAR: return "customize_avatar";
    case CAMERA_MODE_FOLLOW: return "follow";
    default: return "unknown";
    }
}

LLSD build_object_snapshot(LLViewerObject* object, LLSelectNode* node = nullptr)
{
    if (!object || object->isDead())
    {
        return LLSD();
    }

    LLSD out;
    out["id"] = object->getID().asString();
    out["local_id"] = (LLSD::Integer)object->getLocalID();
    out["root_id"] = object->getRootEdit() ? object->getRootEdit()->getID().asString() : object->getID().asString();
    out["pcode"] = (LLSD::Integer)object->getPCode();
    out["click_action"] = (LLSD::Integer)object->getClickAction();
    out["has_touch"] = (bool)object->flagHandleTouch();
    out["is_avatar"] = object->isAvatar();
    out["is_attachment"] = object->isAttachment();
    out["position"] = vector3d_to_array(object->getPositionGlobal());
    out["size"] = vector3_to_array(object->getScale());
    out["name"] = node ? node->mName : "";
    out["description"] = node ? node->mDescription : "";
    if (node)
    {
        out["selected_face"] = (LLSD::Integer)node->getLastSelectedTE();
    }
    return out;
}

LLSD build_selection_snapshot()
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (selection.isNull() || selection->getObjectCount() <= 0)
    {
        return LLSD();
    }

    LLSelectNode* node = selection->getFirstRootNode(nullptr, true);
    if (!node)
    {
        node = selection->getFirstNode();
    }
    if (!node)
    {
        return LLSD();
    }

    LLSD out = build_object_snapshot(node->getObject(), node);
    out["count"] = (LLSD::Integer)selection->getObjectCount();
    out["select_type"] = (LLSD::Integer)selection->getSelectType();
    return out;
}

LLSD build_hover_snapshot()
{
    LLSelectNode* node = LLSelectMgr::getInstance()->getPrimaryHoverNode();
    if (!node)
    {
        node = LLSelectMgr::getInstance()->getHoverNode();
    }
    return node ? build_object_snapshot(node->getObject(), node) : LLSD();
}
}

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

    updateTurnController();

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

    if (mAutoCaptureEnabled && (now - mLastCaptureTick) >= (1.0 / (F64)mAutoCaptureHz))
    {
        mLastCaptureTick = now;
        captureAndStream();
    }
}

void LLAIState::startTurnToHeading(F32 target_heading_deg, F32 tolerance_deg)
{
    mTurnTargetHeading = normalize_heading_deg(target_heading_deg);
    mTurnToleranceDeg = llclamp(tolerance_deg, 0.5f, 45.0f);
    mTurnActive = true;
    updateTurnController();
}

void LLAIState::cancelTurn()
{
    mTurnActive = false;
    gAgentCamera.setYawKey(0.f);
    gAgent.clearControlFlags(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
}

void LLAIState::updateTurnController()
{
    if (!mTurnActive || !gAgent.getRegion())
    {
        return;
    }

    const F32 current_heading = current_heading_deg();
    const F32 delta = shortest_heading_delta(current_heading, mTurnTargetHeading);
    if (std::fabs(delta) <= mTurnToleranceDeg)
    {
        cancelTurn();
        return;
    }

    // Yaw key magnitude maps to turn speed, where 1.0 is ~90 deg/sec.
    const F32 yaw_key = llclamp(std::fabs(delta) / 30.0f, 0.15f, 1.0f);
    const F32 signed_yaw_key = (delta > 0.f) ? -yaw_key : yaw_key;

    gAgent.clearControlFlags(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
    gAgent.moveYaw(signed_yaw_key);
}

// ── state_tick ───────────────────────────────────────────────────────────────

LLSD LLAIState::buildStateSnapshot() const
{
    if (!gAgent.getRegion()) return LLSD(); // not logged in yet

    // Position
    LLVector3d posGlobal = gAgent.getPositionGlobal();
    LLVector3  vel       = gAgent.getVelocity();

    // Heading: XY angle of agent's forward axis, degrees from +Y (north)
    F32 headingDeg = current_heading_deg();

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
    bool afk = gAgent.getAFK();

    LLViewerObject* focus_object = gAgentCamera.getFocusObject();
    const LLPickInfo& last_pick = gViewerWindow->getLastPick();

    // Assemble LLSD
    LLSD ev;
    ev["event"]   = "state_tick";
    ev["ts"]      = (LLSD::Integer)(S32)LLTimer::getTotalSeconds();
    ev["pos"]     = vector3d_to_array(posGlobal);
    ev["vel"]     = vector3_to_array(vel);
    ev["heading"] = headingDeg;
    ev["cam_pos"] = vector3_to_array(camPos);
    ev["cam_dir"] = vector3_to_array(camDir);
    ev["cam_fov"]         = camFov;
    ev["region"]          = regionName;
    ev["parcel"]          = parcelName;
    ev["teleport_state"]  = tpState;
    ev["sitting"]         = sitting;
    ev["afk"]             = afk;
    ev["camera"]["mode"] = camera_mode_name(gAgentCamera.getCameraMode());
    ev["camera"]["focus_on_avatar"] = gAgentCamera.getFocusOnAvatar();
    ev["camera"]["focus_global"] = vector3d_to_array(gAgentCamera.getFocusGlobal());
    ev["camera"]["focus_target_global"] = vector3d_to_array(gAgentCamera.getFocusTargetGlobal());
    ev["camera"]["focus_object_id"] = focus_object ? focus_object->getID().asString() : "";
    ev["selection"] = build_selection_snapshot();
    ev["hover"] = build_hover_snapshot();
    ev["last_pick"]["valid"] = last_pick.isValid();
    ev["last_pick"]["object_id"] = last_pick.getObjectID().asString();
    ev["last_pick"]["face"] = (LLSD::Integer)last_pick.mObjectFace;
    ev["last_pick"]["pick_type"] = (LLSD::Integer)last_pick.mPickType;
    ev["last_pick"]["screen"] = LLSD::emptyArray();
    ev["last_pick"]["screen"][0] = last_pick.mPickPt.mX;
    ev["last_pick"]["screen"][1] = last_pick.mPickPt.mY;
    ev["last_pick"]["position"] = vector3d_to_array(last_pick.mPosGlobal);
    ev["last_pick"]["intersection"] = vector3_to_array(last_pick.mIntersection);
    return ev;
}

void LLAIState::publishState()
{
    LLSD ev = buildStateSnapshot();
    if (ev.isUndefined()) return;

    ++mLastPublishedStateSeq;
    ev["seq"] = (LLSD::Integer)mLastPublishedStateSeq;
    mLastPublishedState = ev;
    LLAISocket::instance().sendEvent(ev);

    // Detect teleport state transitions
    const std::string regionName = ev["region"].asString();
    const int tpState = ev["teleport_state"].asInteger();
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

// ── auto-capture streaming ────────────────────────────────────────────────────

void LLAIState::setAutoCaptureMode(bool enabled, float hz)
{
    if (hz < 0.1f) hz = 0.1f;
    if (hz > 10.0f) hz = 10.0f;
    mAutoCaptureEnabled = enabled;
    mAutoCaptureHz = hz;
    LL_INFOS(LOG_AI) << "AI auto-capture " << (enabled ? "ON" : "OFF")
                     << " @ " << hz << " Hz" << LL_ENDL;
}

void LLAIState::captureAndStream()
{
    if (!gViewerWindow || !gAgent.getRegion()) return;

    // Capture a half-resolution frame without UI — clean world view
    LLPointer<LLImageRaw> raw = new LLImageRaw();
    bool ok = gViewerWindow->rawSnapshot(
        raw,
        640, 360,
        true,    // keepAspectRatio
        false,   // thumbnail
        false,   // show_ui
        false,   // show_hud
        false, false, false,
        LLSnapshotModel::SNAPSHOT_TYPE_COLOR);
    if (!ok || !raw->getData()) return;

    // JPEG encode at quality 50 — small & fast (~20-40 KB per frame)
    LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(50);
    if (!jpeg->encode(raw, 0.0f)) return;

    const U8* data = jpeg->getData();
    const S32 size = jpeg->getDataSize();
    if (!data || size <= 0) return;

    // Base64-encode inline — receiver writes to local file, no SCP needed
    std::string encoded = LLBase64::encode(data, (size_t)size);

    LLSD ev;
    ev["event"]        = "perception_capture";
    ev["image_format"] = "jpeg";
    ev["image_data"]   = encoded;
    ev["width"]        = (LLSD::Integer)raw->getWidth();
    ev["height"]       = (LLSD::Integer)raw->getHeight();
    ev["auto"]         = true;
    ev["state"]        = buildStateSnapshot();
    LLAISocket::instance().sendEvent(ev);
}

// ── public force-publish ─────────────────────────────────────────────────────

void LLAIState::publishStateNow()  { publishState();  }
void LLAIState::publishNearbyNow() { publishNearby(); }
U32 LLAIState::getLastPublishedStateSeq() const { return mLastPublishedStateSeq; }
LLSD LLAIState::getLastPublishedState() const { return mLastPublishedState; }
