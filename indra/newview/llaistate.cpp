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
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "pipeline.h"
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

// Floor probes. Her eyes read open sky past a skybox edge as clear ground and
// she walked off the polo field four times (2026-10-18); the bounding boxes of
// its rock floor overhang the real edge. A ray straight down at a point ahead
// meets the real surface -- a prim (invisible ones too: skyboxes use them as
// floors) or the sim's terrain, whichever is higher. Avatars and anything worn
// are passed through, so nobody's body reads as floor.
const F32 FLOOR_PROBE_UP_M   = 0.3f;  // ray starts this far above her body centre
const F32 FLOOR_PROBE_DOWN_M = 4.0f;  // ...and ends this far below it (~3 m under her feet)

// Whiskers (2026-10-21): what is solid in front of her, every tick. She ran
// into poles, fences, rock edges and glass that the object list does not give
// a size for (or does not list at all, a building's own mesh): 153 object
// stops and 75 stalls in 14 h. A level ray at knee and at chest height, along
// her facing and 25 degrees either side, out to WHISKER_RANGE_M. Not avatars
// or attachments, not phantom things (fog, grass), and not a walkable slope
// (a surface facing mostly up is ground rising, not a wall).
const F32 WHISKER_RANGE_M = 4.0f;
const F32 WHISKER_START_M = 0.3f;      // clear of her own body
const F32 BODY_ABOVE_FEET_M = 1.0f;    // her position is her body, about this far above her feet

LLSD whisker(const LLVector3& body, F32 heading_deg, F32 rel_deg, F32 above_feet_m)
{
    const F32 rad = (heading_deg + rel_deg) * DEG_TO_RAD;
    const F32 sx = std::sin(rad), sy = std::cos(rad);
    const F32 z = body.mV[VZ] - BODY_ABOVE_FEET_M + above_feet_m;
    LLSD out;
    out["a"] = rel_deg;
    out["h"] = above_feet_m;
    F32 from = WHISKER_START_M;
    for (int i = 0; i < 3 && from < WHISKER_RANGE_M; ++i)
    {
        LLVector4a start, end, hit, normal;
        start.set(body.mV[VX] + sx * from, body.mV[VY] + sy * from, z);
        end.set(body.mV[VX] + sx * WHISKER_RANGE_M, body.mV[VY] + sy * WHISKER_RANGE_M, z);
        S32 face = -1;
        LLViewerObject* obj = gPipeline.lineSegmentIntersectInWorld(
            start, end, false /*transparent*/, false /*rigged*/, true /*unselectable*/, false /*reflection probes*/,
            &face, nullptr, nullptr, &hit, nullptr, &normal);
        if (!obj)
        {
            break;
        }
        const F32 hx = hit.getF32ptr()[0] - body.mV[VX];
        const F32 hy = hit.getF32ptr()[1] - body.mV[VY];
        const F32 dist = std::sqrt(hx * hx + hy * hy);
        LLViewerObject* root = obj;
        while (root && root->isAttachment())
        {
            root = (LLViewerObject*)root->getParent();
        }
        const bool body_part = obj->isAvatar() || obj->isAttachment() || (root && root->isAvatar());
        LLViewerObject* top = obj->getRootEdit() ? obj->getRootEdit() : obj;
        const bool walk_through = obj->flagPhantom() || top->flagPhantom();
        const bool ground_rising = normal.getF32ptr()[2] > 0.6f;
        if (body_part || walk_through || ground_rising)
        {
            from = dist + 0.05f;   // look on past it
            continue;
        }
        out["d"] = dist;
        out["id"] = top->getID().asString();
        break;
    }
    return out;
}

LLSD floor_probe(const LLVector3& body, F32 heading_deg, F32 dist_m)
{
    const F32 rad = heading_deg * DEG_TO_RAD;
    const F32 px = body.mV[VX] + std::sin(rad) * dist_m;
    const F32 py = body.mV[VY] + std::cos(rad) * dist_m;
    F32 top = body.mV[VZ] + FLOOR_PROBE_UP_M;
    const F32 bottom = body.mV[VZ] - FLOOR_PROBE_DOWN_M;

    LLSD out;
    out["d"] = dist_m;
    F32 surface = bottom - 1.f;
    std::string hit_id;
    for (int i = 0; i < 4 && top > bottom; ++i)
    {
        LLVector4a start, end, hit;
        start.set(px, py, top);
        end.set(px, py, bottom);
        S32 face = -1;
        LLViewerObject* obj = gPipeline.lineSegmentIntersectInWorld(
            start, end, true /*transparent*/, false /*rigged*/, true /*unselectable*/, false /*reflection probes*/,
            &face, nullptr, nullptr, &hit);
        if (!obj)
        {
            break;
        }
        LLViewerObject* root = obj;
        while (root && root->isAttachment())
        {
            root = (LLViewerObject*)root->getParent();
        }
        if (obj->isAvatar() || obj->isAttachment() || (root && root->isAvatar()))
        {
            top = hit.getF32ptr()[2] - 0.05f;   // through the body; keep looking below it
            continue;
        }
        surface = hit.getF32ptr()[2];
        hit_id = obj->getID().asString();
        break;
    }
    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(LLVector3(px, py, body.mV[VZ]));
    if (land >= bottom && land <= body.mV[VZ] + FLOOR_PROBE_UP_M && land > surface)
    {
        surface = land;
        hit_id = "land";
    }
    if (surface >= bottom)
    {
        out["z"] = surface;
        out["what"] = hit_id;
    }
    return out;   // no "z": nothing within ~3 m below her feet
}

F32 avatar_heading_deg(const LLUUID& avatar_id)
{
    LLViewerObject* obj = gObjectList.findObject(avatar_id);
    while (obj && obj->isAttachment())
    {
        obj = (LLViewerObject*)obj->getParent();
    }

    if (!obj || !obj->isAvatar())
    {
        return -1.f;
    }

    LLVOAvatar* avatar = (LLVOAvatar*)obj;
    LLVector3 at_axis = avatar->getRenderRotation().getMatrix3().getFwdRow();
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
    // Height of the sim's terrain (the land itself, not prims) under her, in
    // region metres. Her map had no true ground: the floor of a 6 m hollow
    // read as ground, so on the grass beside it she thought she was upstairs
    // (2026-10-13). pos z well below this means she is under the ground.
    ev["terrain_z"] = (LLSD::Real)LLWorld::getInstance()->resolveLandHeightGlobal(posGlobal);
    // The floor under her and 0.75 / 1.5 / 2.5 m ahead along her facing, in
    // region metres (see floor_probe). The controller compares "ahead" with
    // "here", so her body height does not matter.
    {
        const LLVector3 body = gAgent.getPositionAgent();
        ev["floor"]["heading"] = headingDeg;
        ev["floor"]["here"] = floor_probe(body, headingDeg, 0.f);
        ev["floor"]["ahead"] = LLSD::emptyArray();
        ev["floor"]["ahead"].append(floor_probe(body, headingDeg, 0.75f));
        ev["floor"]["ahead"].append(floor_probe(body, headingDeg, 1.5f));
        ev["floor"]["ahead"].append(floor_probe(body, headingDeg, 2.5f));
        // Whiskers: {"a": angle off her facing, "h": height above her feet,
        // "d": metres to the first solid thing (absent if none within range),
        // "id": its root object}.
        ev["whiskers"] = LLSD::emptyArray();
        for (F32 rel : {-25.f, 0.f, 25.f})
        {
            for (F32 h : {0.5f, 1.2f})
            {
                ev["whiskers"].append(whisker(body, headingDeg, rel, h));
            }
        }
    }
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
        F32 heading = avatar_heading_deg(ids[i]);
        if (heading >= 0.f)
        {
            entry["heading"] = heading;
        }
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
