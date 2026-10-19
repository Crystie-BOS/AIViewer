/**
 * llaicommands.cpp
 * Command dispatcher — runs on main thread via idle callback.
 */

#include "llviewerprecompiledheaders.h"
#include "llaicommands.h"
#include "llaisocket.h"
#include "llaistate.h"
#include "llairails.h"
#include "llagent.h"
#include "llagentui.h"
#include "llagentcamera.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llviewerregion.h"
#include "llviewerobjectlist.h"
#include "llviewerobject.h"
#include "llselectmgr.h"
#include "llfloaterreg.h"
#include "llfloater.h"
#include "llview.h"
#include "llfloatermap.h"
#include "llnetmap.h"
#include "llimagepng.h"
#include "lltoolgrab.h"
#include "llnotificationsutil.h"
#include "llviewermessage.h"
#include "llcallbacklist.h"
#include "llcoordframe.h"
#include "llsd.h"
#include "indra_constants.h"
#include "llviewerparcelmgr.h"
#include "llparcel.h"
#include "llvoavatarself.h"
#include "llviewerjointattachment.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llinventoryfunctions.h"
#include "llappearancemgr.h"
#include "lltooldraganddrop.h"
#include "llworld.h"
#include "RRInterface.h"
#include "message.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

// Forward declaration — defined in llfloaterimnearbychat.cpp
void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);

static constexpr const char* LOG_AI = "AI";

namespace
{
LLSD rect_to_llsd(const LLRect& rect)
{
    LLSD out;
    out["left"] = rect.mLeft;
    out["bottom"] = rect.mBottom;
    out["right"] = rect.mRight;
    out["top"] = rect.mTop;
    out["width"] = rect.getWidth();
    out["height"] = rect.getHeight();
    return out;
}

bool rect_is_valid(const LLRect& rect)
{
    return rect.getWidth() > 0 && rect.getHeight() > 0;
}

LLRect clamp_rect_to_bounds(const LLRect& rect, const LLRect& bounds)
{
    LLRect clipped(
        llmax(rect.mLeft, bounds.mLeft),
        llmin(rect.mTop, bounds.mTop),
        llmin(rect.mRight, bounds.mRight),
        llmax(rect.mBottom, bounds.mBottom));
    if (!rect_is_valid(clipped))
    {
        return LLRect();
    }
    return clipped;
}

LLRect map_rect_to_image(const LLRect& rect, const LLRect& basis, S32 out_width, S32 out_height)
{
    LLRect clipped = clamp_rect_to_bounds(rect, basis);
    if (!rect_is_valid(clipped) || basis.getWidth() <= 0 || basis.getHeight() <= 0)
    {
        return LLRect();
    }

    const F32 scale_x = (F32)out_width / (F32)basis.getWidth();
    const F32 scale_y = (F32)out_height / (F32)basis.getHeight();
    LLRect mapped(
        llround((clipped.mLeft - basis.mLeft) * scale_x),
        llround((clipped.mTop - basis.mBottom) * scale_y),
        llround((clipped.mRight - basis.mLeft) * scale_x),
        llround((clipped.mBottom - basis.mBottom) * scale_y));
    return clamp_rect_to_bounds(mapped, LLRect(0, out_height, out_width, 0));
}

std::string replace_extension(const std::string& path, const std::string& suffix)
{
    const std::string::size_type slash = path.find_last_of("/\\");
    const std::string::size_type dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    {
        return path + suffix;
    }
    return path.substr(0, dot) + suffix + path.substr(dot);
}

bool save_png_image(LLImageRaw* raw, const std::string& path)
{
    if (!raw)
    {
        return false;
    }
    LLPointer<LLImagePNG> png = new LLImagePNG();
    return png->encode(raw, 0.0f) && png->save(path);
}

bool crop_and_save_png(LLImageRaw* source, const LLRect& image_rect, const std::string& path)
{
    if (!source || !rect_is_valid(image_rect))
    {
        return false;
    }

    const S32 src_width = source->getWidth();
    const S32 src_height = source->getHeight();
    const S32 components = source->getComponents();
    if (src_width <= 0 || src_height <= 0 || components <= 0)
    {
        return false;
    }

    LLRect clipped = clamp_rect_to_bounds(image_rect, LLRect(0, src_height, src_width, 0));
    if (!rect_is_valid(clipped))
    {
        return false;
    }

    LLPointer<LLImageRaw> cropped = new LLImageRaw();
    if (!cropped->resize((U16)clipped.getWidth(), (U16)clipped.getHeight(), (S8)components))
    {
        return false;
    }

    const U8* src_data = source->getData();
    U8* dst_data = cropped->getData();
    const S32 src_stride = src_width * components;
    const S32 dst_stride = clipped.getWidth() * components;

    for (S32 row = 0; row < clipped.getHeight(); ++row)
    {
        const S32 src_y = clipped.mBottom + row;
        std::memcpy(
            dst_data + row * dst_stride,
            src_data + src_y * src_stride + clipped.mLeft * components,
            (size_t)dst_stride);
    }

    return save_png_image(cropped, path);
}

F32 current_heading_deg()
{
    LLVector3 at = gAgent.getFrameAgent().getAtAxis();
    F32 heading = (F32)(std::atan2(at.mV[VX], at.mV[VY]) * RAD_TO_DEG);
    while (heading < 0.f) heading += 360.f;
    while (heading >= 360.f) heading -= 360.f;
    return heading;
}

LLSD vector3d_to_array(const LLVector3d& value)
{
    LLSD out = LLSD::emptyArray();
    out[0] = value.mdV[VX];
    out[1] = value.mdV[VY];
    out[2] = value.mdV[VZ];
    return out;
}

LLSD vector3_to_array(const LLVector3& value)
{
    LLSD out = LLSD::emptyArray();
    out[0] = value.mV[VX];
    out[1] = value.mV[VY];
    out[2] = value.mV[VZ];
    return out;
}

bool llsd_to_vector3d(const LLSD& value, LLVector3d& out)
{
    if (!value.isArray() || value.size() < 3) return false;
    out = LLVector3d(value[0].asReal(), value[1].asReal(), value[2].asReal());
    return true;
}

bool llsd_to_vector3(const LLSD& value, LLVector3& out)
{
    if (!value.isArray() || value.size() < 3) return false;
    out = LLVector3((F32)value[0].asReal(), (F32)value[1].asReal(), (F32)value[2].asReal());
    return true;
}

void clear_movement_flags()
{
    gAgent.clearControlFlags(
        AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
        AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
        AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG);
}

bool apply_move_dir(const std::string& dir)
{
    if      (dir == "forward") { gAgent.moveAt(1); return true; }
    else if (dir == "back")    { gAgent.moveAt(-1); return true; }
    else if (dir == "left")    { gAgent.moveLeft(1); return true; }
    else if (dir == "right")   { gAgent.moveLeft(-1); return true; }
    else if (dir == "up")      { gAgent.moveUp(1); return true; }
    else if (dir == "down")    { gAgent.moveUp(-1); return true; }
    return false;
}

bool apply_move_pulse_dir(const std::string& dir)
{
    if      (dir == "forward") { gAgent.setControlFlags(AGENT_CONTROL_AT_POS); return true; }
    else if (dir == "back")    { gAgent.setControlFlags(AGENT_CONTROL_AT_NEG); return true; }
    else if (dir == "left")    { gAgent.setControlFlags(AGENT_CONTROL_LEFT_POS); return true; }
    else if (dir == "right")   { gAgent.setControlFlags(AGENT_CONTROL_LEFT_NEG); return true; }
    return false;
}

void apply_move_pulse_stop()
{
    clear_movement_flags();
    gAgent.setControlFlags(AGENT_CONTROL_STOP);
}

F64 velocity_length(const LLVector3& value)
{
    return std::sqrt(
        (F64)value.mV[VX] * (F64)value.mV[VX] +
        (F64)value.mV[VY] * (F64)value.mV[VY] +
        (F64)value.mV[VZ] * (F64)value.mV[VZ]);
}

LLRect screen_rect_for_view(LLView* view)
{
    if (!view || !view->isInVisibleChain())
    {
        return LLRect();
    }
    LLRect rect;
    view->localRectToScreen(view->getLocalRect(), &rect);
    return rect;
}

void add_region_metadata(LLSD& regions,
                         const std::string& name,
                         const LLRect& source_rect,
                         const LLRect& image_rect,
                         const std::string& crop_path,
                         bool crop_saved)
{
    if (!rect_is_valid(source_rect) || !rect_is_valid(image_rect))
    {
        return;
    }

    LLSD region;
    region["source_rect"] = rect_to_llsd(source_rect);
    region["image_rect"] = rect_to_llsd(image_rect);
    if (!crop_path.empty())
    {
        region["crop_path"] = crop_path;
        region["crop_saved"] = crop_saved;
    }
    regions[name] = region;
}

void collect_requested_regions(const LLSD& value, std::set<std::string>& out)
{
    if (value.isArray())
    {
        for (LLSD::array_const_iterator it = value.beginArray(); it != value.endArray(); ++it)
        {
            const std::string region = it->asString();
            if (!region.empty())
            {
                out.insert(region);
            }
        }
    }
    else if (value.isString() && !value.asString().empty())
    {
        out.insert(value.asString());
    }
}
}

LLAICommands::LLAICommands() = default;

void LLAICommands::start()
{
    if (!mRegistered)
    {
        gIdleCallbacks.addFunction(&LLAICommands::idleCB, this);
        mRegistered = true;
    }
}

void LLAICommands::stop()
{
    if (mRegistered)
    {
        gIdleCallbacks.deleteFunction(&LLAICommands::idleCB, this);
        mRegistered = false;
    }
}

/*static*/ void LLAICommands::idleCB(void* data)
{
    static_cast<LLAICommands*>(data)->onIdle();
}

void LLAICommands::onIdle()
{
    // Drain deferred one-shot actions (e.g. DeGrab after Grab)
    while (!mDeferred.empty())
    {
        auto fn = mDeferred.front();
        mDeferred.pop_front();
        fn();
    }

    // Reapply held movement each idle frame so manual move commands behave as
    // continuous controls rather than one-frame nudges.
    tickMovePulse();
    tickHeldMove();

    // Tick navigation state machine
    tickNavigation();

    // Drain incoming commands
    LLSD cmd;
    while (LLAISocket::instance().pollCommand(cmd))
        dispatch(cmd);
}

void LLAICommands::deferOnce(std::function<void()> fn)
{
    mDeferred.push_back(std::move(fn));
}

// ── dispatcher ───────────────────────────────────────────────────────────────

void LLAICommands::dispatch(const LLSD& cmd)
{
    if (!cmd.has("cmd")) { sendError("unknown", "missing_cmd_field"); return; }

    std::string name = cmd["cmd"].asString();

    // Safety rail check (skip for pause/resume which manage the pause flag themselves)
    if (name != "pause" && name != "resume")
    {
        std::string reason;
        if (!LLAIRails::instance().allow(name, cmd, reason))
        {
            sendError(name, reason);
            LLAIRails::instance().audit(name, "blocked:" + reason);
            return;
        }
    }

    if      (name == "move")                  cmdMove(cmd);
    else if (name == "move_pulse")            cmdMovePulse(cmd);
    else if (name == "turn")                  cmdTurn(cmd);
    else if (name == "face")                  cmdFace(cmd);
    else if (name == "camera_look")           cmdCameraLook(cmd);
    else if (name == "camera_place")          cmdCameraPlace(cmd);
    else if (name == "chat")                  cmdChat(cmd);
    else if (name == "im")                    cmdIM(cmd);
    else if (name == "teleport")              cmdTeleport(cmd);
    else if (name == "teleport_landmark")     cmdTeleportLandmark(cmd);
    else if (name == "sit")                   cmdSit(cmd);
    else if (name == "stand")                 cmdStand(cmd);
    else if (name == "screenshot")            cmdScreenshot(cmd);
    else if (name == "capture_perception")    cmdCapturePerception(cmd);
    else if (name == "get_state")             cmdGetState(cmd);
    else if (name == "get_nearby")            cmdGetNearby(cmd);
    else if (name == "get_objects")           cmdGetObjects(cmd);
    else if (name == "touch")                 cmdTouch(cmd);
    else if (name == "request_object_props")  cmdRequestObjectProps(cmd);
    else if (name == "navigate_to")           cmdNavigateTo(cmd);
    else if (name == "navigate_stop")         cmdNavigateStop(cmd);
    else if (name == "pause")                 cmdPause(cmd);
    else if (name == "resume")                cmdResume(cmd);
    else if (name == "dismiss_dialog")        cmdDismissDialog(cmd);
    else if (name == "respond_dialog")        cmdRespondDialog(cmd);
    else if (name == "set_auto_capture")      cmdSetAutoCapture(cmd);
    else if (name == "get_parcel_info")       cmdGetParcelInfo(cmd);
    else if (name == "get_attachments")       cmdGetAttachments(cmd);
    else if (name == "list_inventory")        cmdListInventory(cmd);
    else if (name == "wear_item")             cmdWearItem(cmd);
    else if (name == "remove_item")           cmdRemoveItem(cmd);
    else if (name == "wear_outfit")           cmdWearOutfit(cmd);
    else if (name == "rez_item")              cmdRezItem(cmd);
    else
    {
        sendError(name, "unknown_command");
        return;
    }

    LLAIRails::instance().audit(name, "ok");
}

// ── movement ─────────────────────────────────────────────────────────────────

void LLAICommands::cmdMove(const LLSD& p)
{
    std::string dir = p.has("dir") ? p["dir"].asString() : "";
    LLAIState::instance().cancelTurn();
    if      (dir == "forward" || dir == "back" || dir == "left" ||
             dir == "right" || dir == "up" || dir == "down")
    {
        mMovePulse.active = false;
        mMove.active = true;
        mMove.dir = dir;
        tickHeldMove();
    }
    else if (dir == "stop")
    {
        mMove.active = false;
        mMove.dir.clear();
        mMovePulse.active = false;
        clear_movement_flags();
    }
    else { sendError("move", "invalid_dir"); return; }
    sendOk("move");
}

void LLAICommands::cmdMovePulse(const LLSD& p)
{
    std::string dir = p.has("dir") ? p["dir"].asString() : "";
    if (dir != "forward" && dir != "back" && dir != "left" && dir != "right")
    {
        sendError("move_pulse", "invalid_dir");
        return;
    }
    if (!gAgent.getRegion())
    {
        sendError("move_pulse", "no_region");
        return;
    }
    if (mNav.active)
    {
        sendError("move_pulse", "navigation_active");
        return;
    }
    if (mMovePulse.active)
    {
        sendError("move_pulse", "pulse_active");
        return;
    }

    S32 frames = p.has("frames") ? (S32)p["frames"].asInteger() : 1;
    frames = llclamp(frames, (S32)1, (S32)3);
    S32 settle_frames = p.has("settle_frames") ? (S32)p["settle_frames"].asInteger() : 6;
    settle_frames = llclamp(settle_frames, (S32)2, (S32)60);
    F64 max_distance = p.has("max_distance_m") ? p["max_distance_m"].asReal() : 0.12;
    max_distance = llclamp(max_distance, 0.02, 0.25);
    F64 max_drop = p.has("max_drop_m") ? p["max_drop_m"].asReal() : 0.08;
    max_drop = llclamp(max_drop, 0.01, 0.25);
    F64 max_settle_velocity = p.has("max_settle_velocity_mps") ? p["max_settle_velocity_mps"].asReal() : 0.15;
    max_settle_velocity = llclamp(max_settle_velocity, 0.01, 0.5);

    LLAIState::instance().cancelTurn();
    mMove.active = false;
    mMove.dir.clear();
    clear_movement_flags();

    mMovePulse.active = true;
    mMovePulse.started = false;
    mMovePulse.dir = dir;
    mMovePulse.id = p.has("id") ? p["id"].asString() : "";
    mMovePulse.frames = frames;
    mMovePulse.framesApplied = 0;
    mMovePulse.stopFrames = 4;
    mMovePulse.stopFramesApplied = 0;
    mMovePulse.settleFrames = settle_frames;
    mMovePulse.settleFramesApplied = 0;
    mMovePulse.maxDistance = max_distance;
    mMovePulse.maxDrop = max_drop;
    mMovePulse.maxSettleVelocity = max_settle_velocity;
    mMovePulse.failed = false;
    mMovePulse.failReason.clear();
    mMovePulse.startPos = gAgent.getPositionGlobal();
    mMovePulse.startRegion = gAgent.getRegion()->getName();
    mMovePulse.lastConsumedStateSeq = LLAIState::instance().getLastPublishedStateSeq();
    sendOk("move_pulse");
}

void LLAICommands::tickHeldMove()
{
    if (!mMove.active)
    {
        return;
    }

    const std::string& dir = mMove.dir;
    apply_move_dir(dir);
}

void LLAICommands::tickMovePulse()
{
    if (!mMovePulse.active)
    {
        return;
    }

    if (!gAgent.getRegion())
    {
        mMovePulse.active = false;
        clear_movement_flags();
        sendError("move_pulse", "no_region");
        return;
    }

    if (!mMovePulse.started)
    {
        mMovePulse.started = true;
        mMovePulse.startPos = gAgent.getPositionGlobal();
        mMovePulse.startRegion = gAgent.getRegion()->getName();
    }

    if (mMovePulse.framesApplied < mMovePulse.frames)
    {
        if (!apply_move_pulse_dir(mMovePulse.dir))
        {
            mMovePulse.active = false;
            clear_movement_flags();
            sendError("move_pulse", "invalid_dir");
        }
        else
        {
            ++mMovePulse.framesApplied;
        }
        return;
    }

    if (mMovePulse.stopFramesApplied < mMovePulse.stopFrames)
    {
        apply_move_pulse_stop();
        ++mMovePulse.stopFramesApplied;
        return;
    }

    clear_movement_flags();
    if (mMovePulse.settleFramesApplied < mMovePulse.settleFrames)
    {
        const U32 published_state_seq = LLAIState::instance().getLastPublishedStateSeq();
        if (published_state_seq <= mMovePulse.lastConsumedStateSeq)
        {
            return;
        }
        LLSD state = LLAIState::instance().getLastPublishedState();
        LLVector3d currentPos;
        LLVector3 currentVel;
        if (!llsd_to_vector3d(state["pos"], currentPos) || !llsd_to_vector3(state["vel"], currentVel))
        {
            return;
        }
        mMovePulse.lastConsumedStateSeq = published_state_seq;
        LLVector3d currentDelta = currentPos - mMovePulse.startPos;
        if (currentDelta.length() > mMovePulse.maxDistance)
        {
            mMovePulse.failed = true;
            mMovePulse.failReason = "distance_limit_exceeded";
            mMovePulse.settleFramesApplied = mMovePulse.settleFrames;
        }
        else if (-currentDelta.mdV[VZ] > mMovePulse.maxDrop)
        {
            mMovePulse.failed = true;
            mMovePulse.failReason = "z_drop_limit_exceeded";
            mMovePulse.settleFramesApplied = mMovePulse.settleFrames;
        }
        else if (mMovePulse.settleFramesApplied + 1 >= mMovePulse.settleFrames &&
                 velocity_length(currentVel) > mMovePulse.maxSettleVelocity)
        {
            mMovePulse.failed = true;
            mMovePulse.failReason = "velocity_not_settled";
            mMovePulse.settleFramesApplied = mMovePulse.settleFrames;
        }
        else
        {
            ++mMovePulse.settleFramesApplied;
            return;
        }
    }

    clear_movement_flags();
    if (mMovePulse.settleFramesApplied < mMovePulse.settleFrames)
    {
        ++mMovePulse.settleFramesApplied;
        return;
    }

    LLSD final_state = LLAIState::instance().getLastPublishedState();
    LLVector3d endPos = gAgent.getPositionGlobal();
    LLVector3 vel = gAgent.getVelocity();
    llsd_to_vector3d(final_state["pos"], endPos);
    llsd_to_vector3(final_state["vel"], vel);
    LLVector3d delta = endPos - mMovePulse.startPos;
    const F64 final_velocity_mps = velocity_length(vel);
    if (!mMovePulse.failed)
    {
        if (delta.length() > mMovePulse.maxDistance)
        {
            mMovePulse.failed = true;
            mMovePulse.failReason = "distance_limit_exceeded";
        }
        else if (-delta.mdV[VZ] > mMovePulse.maxDrop)
        {
            mMovePulse.failed = true;
            mMovePulse.failReason = "z_drop_limit_exceeded";
        }
        else if (final_velocity_mps > mMovePulse.maxSettleVelocity)
        {
            mMovePulse.failed = true;
            mMovePulse.failReason = "velocity_not_settled";
        }
    }

    LLSD ev;
    ev["event"] = "move_pulse_complete";
    ev["cmd"] = "move_pulse";
    ev["ok"] = !mMovePulse.failed;
    ev["reason"] = mMovePulse.failed ? mMovePulse.failReason : "stopped_confirmed";
    ev["id"] = mMovePulse.id;
    ev["dir"] = mMovePulse.dir;
    ev["frames"] = (LLSD::Integer)mMovePulse.frames;
    ev["frames_applied"] = (LLSD::Integer)mMovePulse.framesApplied;
    ev["stop_frames"] = (LLSD::Integer)mMovePulse.stopFrames;
    ev["stop_frames_applied"] = (LLSD::Integer)mMovePulse.stopFramesApplied;
    ev["settle_frames"] = (LLSD::Integer)mMovePulse.settleFrames;
    ev["settle_frames_applied"] = (LLSD::Integer)mMovePulse.settleFramesApplied;
    ev["max_distance_m"] = mMovePulse.maxDistance;
    ev["max_drop_m"] = mMovePulse.maxDrop;
    ev["max_settle_velocity_mps"] = mMovePulse.maxSettleVelocity;
    ev["start_pos"] = vector3d_to_array(mMovePulse.startPos);
    ev["end_pos"] = vector3d_to_array(endPos);
    ev["delta"] = vector3d_to_array(delta);
    ev["distance_m"] = delta.length();
    ev["z_delta_m"] = delta.mdV[VZ];
    ev["velocity_mps"] = final_velocity_mps;
    ev["region"] = final_state.has("region") ? final_state["region"] : LLSD(gAgent.getRegion()->getName());
    ev["start_region"] = mMovePulse.startRegion;
    ev["vel"] = vector3_to_array(vel);
    ev["state_seq"] = (LLSD::Integer)mMovePulse.lastConsumedStateSeq;
    LLAISocket::instance().sendEvent(ev);

    mMovePulse.active = false;
}

void LLAICommands::cmdTurn(const LLSD& p)
{
    if (!p.has("degrees")) { sendError("turn", "missing_degrees"); return; }
    F32 deg = (F32)p["degrees"].asReal();
    const F32 target_heading = current_heading_deg() - deg;
    LLAIState::instance().startTurnToHeading(target_heading, 1.5f);
    sendOk("turn");
}

void LLAICommands::cmdFace(const LLSD& p)
{
    if (!p.has("heading")) { sendError("face", "missing_heading"); return; }
    F32 targetDeg = (F32)p["heading"].asReal();
    F32 toleranceDeg = p.has("tolerance") ? (F32)p["tolerance"].asReal() : 2.0f;
    LLAIState::instance().startTurnToHeading(targetDeg, toleranceDeg);
    sendOk("face");
}

void LLAICommands::cmdCameraLook(const LLSD& p)
{
    if (!p.has("heading")) { sendError("camera_look", "missing_heading"); return; }
    if (!gAgent.getRegion()) { sendError("camera_look", "no_region"); return; }

    F32 headingDeg = (F32)p["heading"].asReal();
    while (headingDeg < 0.f) headingDeg += 360.f;
    while (headingDeg >= 360.f) headingDeg -= 360.f;

    const F32 pitchDeg = llmax(-60.f, llmin(45.f, p.has("pitch") ? (F32)p["pitch"].asReal() : -6.f));
    const F32 distanceM = llmax(0.5f, llmin(12.f, p.has("distance_m") ? (F32)p["distance_m"].asReal() : 3.f));
    const F32 focusDistanceM = llmax(0.75f, llmin(24.f, p.has("focus_distance_m") ? (F32)p["focus_distance_m"].asReal() : 4.f));
    const F32 focusZOffsetM = llmax(0.f, llmin(2.5f, p.has("focus_z_offset_m") ? (F32)p["focus_z_offset_m"].asReal() : 1.15f));
    const F32 cameraForwardOffsetM = llmax(-1.0f, llmin(2.0f, p.has("camera_forward_offset_m") ? (F32)p["camera_forward_offset_m"].asReal() : -distanceM));

    if (p.has("fov_deg"))
    {
        // Widen (or restore) the camera's field of view before positioning it,
        // so a single capture covers more of the surrounding scene -- closer to
        // peripheral awareness than the default ~60 deg. Self-clamped to the
        // viewer's own min/max view and RLV zoom limits by setDefaultFOV.
        const F32 fovDeg = llmax(20.f, llmin(150.f, (F32)p["fov_deg"].asReal()));
        LLViewerCamera::getInstance()->setDefaultFOV(fovDeg * DEG_TO_RAD);
    }

    const F64 headingRad = (F64)headingDeg * DEG_TO_RAD;
    const F64 pitchRad = (F64)pitchDeg * DEG_TO_RAD;
    const F64 dirX = std::sin(headingRad);
    const F64 dirY = std::cos(headingRad);
    const F64 horiz = std::cos(pitchRad);
    const F64 dirZ = std::sin(pitchRad);

    LLVector3d eye = gAgent.getPositionGlobal();
    eye.mdV[VZ] += 1.45;

    LLVector3d focus(
        eye.mdV[VX] + dirX * horiz * focusDistanceM,
        eye.mdV[VY] + dirY * horiz * focusDistanceM,
        eye.mdV[VZ] + dirZ * focusDistanceM);
    LLVector3d camera(
        eye.mdV[VX] - dirX * distanceM,
        eye.mdV[VY] - dirY * distanceM,
        eye.mdV[VZ] + 0.35);

    LLVector3d focusLocal;
    if (p.has("focus_position") && llsd_to_vector3d(p["focus_position"], focusLocal))
    {
        LLVector3d origin = gAgent.getRegion()->getOriginGlobal();
        focus = origin + focusLocal;
        focus.mdV[VZ] += focusZOffsetM;
        camera = LLVector3d(
            eye.mdV[VX] + dirX * cameraForwardOffsetM,
            eye.mdV[VY] + dirY * cameraForwardOffsetM,
            eye.mdV[VZ]);
    }

    gAgentCamera.setFocusOnAvatar(false, false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(camera, focus, LLUUID::null);

    LLSD ev;
    ev["event"] = "camera_look";
    ev["cmd"] = "camera_look";
    if (p.has("id")) ev["id"] = p["id"];
    ev["heading"] = headingDeg;
    ev["pitch"] = pitchDeg;
    ev["distance_m"] = distanceM;
    ev["focus_distance_m"] = focusDistanceM;
    ev["camera_pos"] = vector3d_to_array(camera);
    ev["focus"] = vector3d_to_array(focus);
    ev["target_aware"] = p.has("focus_position");
    ev["fov_deg"] = LLViewerCamera::getInstance()->getView() * RAD_TO_DEG;
    LLAISocket::instance().sendEvent(ev);
    sendOk("camera_look");
}

// Camera at an exact point, looking at another: aerial photos of the region,
// straight down from far above (camera_look is limited to 12 m and -60 deg,
// for her own eye-level looks). Region-local metres; both points must lie in or
// near her region, and the camera within 600 m of what it looks at.
// Optional fov_deg and draw_distance_m (RenderFarClip, 64-512).
void LLAICommands::cmdCameraPlace(const LLSD& p)
{
    if (!gAgent.getRegion()) { sendError("camera_place", "no_region"); return; }
    LLVector3d camLocal, focusLocal;
    if (!p.has("camera_position") || !llsd_to_vector3d(p["camera_position"], camLocal) ||
        !p.has("focus_position") || !llsd_to_vector3d(p["focus_position"], focusLocal))
    {
        sendError("camera_place", "missing_positions");
        return;
    }
    auto in_reach = [](const LLVector3d& v) {
        return v.mdV[VX] >= -128.0 && v.mdV[VX] <= 384.0 && v.mdV[VY] >= -128.0 && v.mdV[VY] <= 384.0 &&
               v.mdV[VZ] >= -10.0 && v.mdV[VZ] <= 4096.0;
    };
    if (!in_reach(camLocal) || !in_reach(focusLocal) || (camLocal - focusLocal).magVec() > 600.0)
    {
        sendError("camera_place", "out_of_reach");
        return;
    }
    if (p.has("fov_deg"))
    {
        const F32 fovDeg = llmax(20.f, llmin(150.f, (F32)p["fov_deg"].asReal()));
        LLViewerCamera::getInstance()->setDefaultFOV(fovDeg * DEG_TO_RAD);
    }
    if (p.has("draw_distance_m"))
    {
        const F32 far = llmax(64.f, llmin(512.f, (F32)p["draw_distance_m"].asReal()));
        gSavedSettings.setF32("RenderFarClip", far);
    }
    LLVector3d origin = gAgent.getRegion()->getOriginGlobal();
    LLVector3d camera = origin + camLocal;
    LLVector3d focus = origin + focusLocal;
    gAgentCamera.setFocusOnAvatar(false, false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(camera, focus, LLUUID::null);

    LLSD ev;
    ev["event"] = "camera_place";
    ev["cmd"] = "camera_place";
    if (p.has("id")) ev["id"] = p["id"];
    ev["camera_pos"] = vector3d_to_array(camera);
    ev["focus"] = vector3d_to_array(focus);
    ev["fov_deg"] = LLViewerCamera::getInstance()->getView() * RAD_TO_DEG;
    ev["draw_distance_m"] = gSavedSettings.getF32("RenderFarClip");
    LLAISocket::instance().sendEvent(ev);
    sendOk("camera_place");
}

// ── communication ─────────────────────────────────────────────────────────────

void LLAICommands::cmdChat(const LLSD& p)
{
    if (!p.has("text")) { sendError("chat", "missing_text"); return; }
    S32 channel = p.has("channel") ? (S32)p["channel"].asInteger() : 0;
    send_chat_from_viewer(p["text"].asString(), CHAT_TYPE_NORMAL, channel);
    sendOk("chat");
}

void LLAICommands::cmdIM(const LLSD& p)
{
    if (!p.has("to") || !p.has("text")) { sendError("im", "missing_to_or_text"); return; }

    LLUUID toId(p["to"].asString());
    if (toId.isNull()) { sendError("im", "invalid_uuid"); return; }

    std::string my_name;
    LLAgentUI::buildFullname(my_name);
    send_improved_im(toId, my_name, p["text"].asString());
    sendOk("im");
}

// ── teleport ─────────────────────────────────────────────────────────────────

void LLAICommands::cmdTeleport(const LLSD& p)
{
    if (!p.has("x") || !p.has("y") || !p.has("z"))
    { sendError("teleport", "missing_coords"); return; }

    // If region name given, check allowlist
    if (p.has("region"))
    {
        std::string rn = p["region"].asString();
        if (!LLAIRails::instance().regionAllowed(rn))
        { sendError("teleport", "region_not_allowed"); return; }
    }

    // Coordinates are region-local; convert to global via current region handle if needed.
    // If "global" key is present, treat as global coords directly.
    LLVector3d pos;
    if (p.has("global") && p["global"].asBoolean())
    {
        pos = LLVector3d(p["x"].asReal(), p["y"].asReal(), p["z"].asReal());
    }
    else if (gAgent.getRegion())
    {
        LLVector3d origin = gAgent.getRegion()->getOriginGlobal();
        pos = origin + LLVector3d(p["x"].asReal(), p["y"].asReal(), p["z"].asReal());
    }
    else
    { sendError("teleport", "no_region"); return; }

    gAgent.teleportViaLocation(pos);
    sendOk("teleport");
}

void LLAICommands::cmdTeleportLandmark(const LLSD& p)
{
    if (!p.has("id")) { sendError("teleport_landmark", "missing_id"); return; }
    LLUUID id(p["id"].asString());
    if (id.isNull()) { sendError("teleport_landmark", "invalid_uuid"); return; }
    gAgent.teleportViaLandmark(id);
    sendOk("teleport_landmark");
}

// ── avatar actions ───────────────────────────────────────────────────────────

void LLAICommands::cmdSit(const LLSD& /*p*/)
{
    gAgent.sitDown();
    sendOk("sit");
}

void LLAICommands::cmdStand(const LLSD& /*p*/)
{
    gAgent.standUp();
    sendOk("stand");
}

// ── screenshot ───────────────────────────────────────────────────────────────

void LLAICommands::cmdScreenshot(const LLSD& p)
{
    if (!p.has("path")) { sendError("screenshot", "missing_path"); return; }
    S32 w = p.has("w") ? (S32)p["w"].asInteger() : 1280;
    S32 h = p.has("h") ? (S32)p["h"].asInteger() : 720;
    const bool show_ui = p.has("show_ui") ? p["show_ui"].asBoolean() : false;
    const bool show_hud = p.has("show_hud") ? p["show_hud"].asBoolean() : false;

    std::string path = p["path"].asString();
    bool ok = gViewerWindow->saveSnapshot(
        path, w, h,
        show_ui, show_hud, /*do_rebuild*/false, /*show_balance*/false,
        LLSnapshotModel::SNAPSHOT_TYPE_COLOR,
        LLSnapshotModel::SNAPSHOT_FORMAT_PNG);

    if (ok)
    {
        LLSD ev;
        ev["event"] = "screenshot_ok";
        ev["path"]  = path;
        LLAISocket::instance().sendEvent(ev);
    }
    else
    {
        sendError("screenshot", "save_failed");
    }
}

void LLAICommands::cmdCapturePerception(const LLSD& p)
{
    if (!p.has("path")) { sendError("capture_perception", "missing_path"); return; }

    const S32 requested_w = p.has("w") ? (S32)p["w"].asInteger() : 1280;
    const S32 requested_h = p.has("h") ? (S32)p["h"].asInteger() : 720;
    const bool show_ui = p.has("show_ui") ? p["show_ui"].asBoolean() : true;
    const bool show_hud = p.has("show_hud") ? p["show_hud"].asBoolean() : true;
    const std::string path = p["path"].asString();

    LLPointer<LLImageRaw> raw = new LLImageRaw();
    bool ok = gViewerWindow->rawSnapshot(
        raw,
        requested_w,
        requested_h,
        true,
        false,
        show_ui,
        show_hud,
        false,
        false,
        false,
        LLSnapshotModel::SNAPSHOT_TYPE_COLOR);
    if (!ok || !save_png_image(raw, path))
    {
        sendError("capture_perception", "save_failed");
        return;
    }

    const LLRect window_rect = gViewerWindow->getWindowRectRaw();
    const LLRect world_rect = gViewerWindow->getWorldViewRectRaw();
    const LLRect basis_rect = show_ui ? window_rect : world_rect;
    const S32 image_width = raw->getWidth();
    const S32 image_height = raw->getHeight();
    const std::string focused_name =
        (gFloaterView && gFloaterView->getFocusedFloater()) ? gFloaterView->getFocusedFloater()->getName() : std::string();

    std::set<std::string> requested_regions;
    collect_requested_regions(p.has("crop_regions") ? p["crop_regions"] : LLSD(), requested_regions);

    struct RegionCandidate
    {
        std::string name;
        LLRect source_rect;
    };
    std::vector<RegionCandidate> candidates;
    candidates.push_back({"world_view", world_rect});

    if (LLFloater* nearby_chat = LLFloaterReg::findInstance("nearby_chat"))
    {
        candidates.push_back({"nearby_chat", screen_rect_for_view(nearby_chat)});
    }
    if (LLFloaterMap* mini_map = LLFloaterReg::findTypedInstance<LLFloaterMap>("mini_map"))
    {
        LLNetMap* net_map = mini_map->findChild<LLNetMap>("Net Map", true);
        candidates.push_back({"mini_map", net_map ? screen_rect_for_view(net_map) : screen_rect_for_view(mini_map)});
    }
    if (LLFloater* inventory = LLFloaterReg::findInstance("inventory"))
    {
        candidates.push_back({"inventory", screen_rect_for_view(inventory)});
    }
    if (gFloaterView)
    {
        if (LLFloater* focused = gFloaterView->getFocusedFloater())
        {
            candidates.push_back({"focused_floater", screen_rect_for_view(focused)});
        }
        if (LLFloater* frontmost = gFloaterView->getFrontmostClosableFloater())
        {
            candidates.push_back({"modal_dialog", screen_rect_for_view(frontmost)});
        }
    }

    LLSD regions;
    for (const RegionCandidate& candidate : candidates)
    {
        const LLRect image_rect = map_rect_to_image(candidate.source_rect, basis_rect, image_width, image_height);
        if (!rect_is_valid(image_rect))
        {
            continue;
        }

        std::string crop_path;
        bool crop_saved = false;
        if (requested_regions.find(candidate.name) != requested_regions.end())
        {
            crop_path = replace_extension(path, "." + candidate.name);
            crop_saved = crop_and_save_png(raw, image_rect, crop_path);
        }
        add_region_metadata(regions, candidate.name, candidate.source_rect, image_rect, crop_path, crop_saved);
    }

    LLSD state = LLAIState::instance().buildStateSnapshot();
    LLSD ev;
    ev["event"] = "perception_capture";
    ev["path"] = path;
    ev["width"] = image_width;
    ev["height"] = image_height;
    ev["show_ui"] = show_ui;
    ev["show_hud"] = show_hud;
    ev["capture_space"] = show_ui ? "window" : "world";
    ev["window_rect"] = rect_to_llsd(window_rect);
    ev["world_rect"] = rect_to_llsd(world_rect);
    ev["capture_rect"] = rect_to_llsd(basis_rect);
    ev["ui_regions"] = regions;
    ev["focused_floater_name"] = focused_name;
    ev["state"] = state;
    ev["selection"] = state["selection"];
    ev["hover"] = state["hover"];
    ev["camera"] = state["camera"];
    ev["last_pick"] = state["last_pick"];
    ev["mouse"] = LLSD::emptyArray();
    ev["mouse"][0] = gViewerWindow->getCurrentMouseX();
    ev["mouse"][1] = gViewerWindow->getCurrentMouseY();
    if (p.has("capture_id"))
    {
        ev["capture_id"] = p["capture_id"];
    }
    LLAISocket::instance().sendEvent(ev);
}

// ── query ─────────────────────────────────────────────────────────────────────

void LLAICommands::cmdGetState(const LLSD& /*p*/)
{
    LLAIState::instance().publishStateNow();
}

void LLAICommands::cmdGetNearby(const LLSD& /*p*/)
{
    LLAIState::instance().publishNearbyNow();
}

// ── rails ─────────────────────────────────────────────────────────────────────

void LLAICommands::cmdPause(const LLSD& /*p*/)
{
    LLAIRails::instance().setPaused(true);
    sendOk("pause");
}

void LLAICommands::cmdResume(const LLSD& /*p*/)
{
    LLAIRails::instance().setPaused(false);
    sendOk("resume");
}

void LLAICommands::cmdDismissDialog(const LLSD& p)
{
    if (!p.has("id")) { sendError("dismiss_dialog", "missing_id"); return; }
    LLUUID notifId(p["id"].asString());
    LLNotificationPtr notif = LLNotifications::instance().find(notifId);
    if (notif)
    {
        LLNotifications::instance().cancel(notif);
        sendOk("dismiss_dialog");
    }
    else
    {
        sendError("dismiss_dialog", "not_found");
    }
}

void LLAICommands::cmdRespondDialog(const LLSD& p)
{
    if (!p.has("id")) { sendError("respond_dialog", "missing_id"); return; }
    LLUUID notifId(p["id"].asString());
    LLNotificationPtr notif = LLNotifications::instance().find(notifId);
    if (!notif)
    {
        sendError("respond_dialog", "not_found");
        return;
    }

    std::string button = p.has("button") ? p["button"].asString() : "Yes";
    // Or by position among the dialog's buttons (the "buttons" list in its
    // dialog event): exact labels can carry padding no one can type.
    if (p.has("index"))
    {
        S32 want = (S32)p["index"].asInteger();
        S32 seen = 0;
        bool found = false;
        LLNotificationFormPtr form = notif->getForm();
        for (S32 i = 0; form && i < form->getNumElements(); ++i)
        {
            LLSD el = form->getElement(i);
            if (el["type"].asString() != "button") continue;
            if (seen++ == want)
            {
                button = el["name"].asString();
                found = true;
                break;
            }
        }
        if (!found)
        {
            sendError("respond_dialog", "bad_index");
            return;
        }
    }
    if (button.empty())
    {
        button = "Yes";
    }

    LLSD response = notif->getResponseTemplate();
    response[button] = true;
    notif->respond(response);

    LLSD ev;
    ev["event"] = "dialog_responded";
    ev["cmd"] = "respond_dialog";
    ev["id"] = notifId.asString();
    ev["button"] = button;
    LLAISocket::instance().sendEvent(ev);
    sendOk("respond_dialog");
}

void LLAICommands::cmdSetAutoCapture(const LLSD& p)
{
    bool enabled = p.has("enabled") ? p["enabled"].asBoolean() : true;
    float hz = p.has("hz") ? (float)p["hz"].asReal() : 1.5f;
    LLAIState::instance().setAutoCaptureMode(enabled, hz);
    sendOk("set_auto_capture");
}

// ── helpers ───────────────────────────────────────────────────────────────────

// ── get_objects ───────────────────────────────────────────────────────────────

void LLAICommands::cmdGetObjects(const LLSD& p)
{
    if (!gAgent.getRegion()) { sendError("get_objects", "no_region"); return; }

    F32 radius = p.has("radius") ? (F32)p["radius"].asReal() : 96.f;
    LLVector3d agentPos = gAgent.getPositionGlobal();

    LLSD ev;
    ev["event"]   = "nearby_objects";
    ev["cmd"]     = "get_objects";
    ev["objects"] = LLSD::emptyArray();
    ev["radius"]  = radius;
    ev["agent_pos"] = vector3d_to_array(agentPos);

    S32 count = gObjectList.getNumObjects();
    S32 dead_count = 0;
    S32 avatar_count = 0;
    S32 attachment_count = 0;
    S32 outside_radius_count = 0;
    S32 returned_count = 0;
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->isDead()) { ++dead_count; continue; }
        if (obj->isAvatar()) { ++avatar_count; continue; }
        if (obj->isAttachment()) { ++attachment_count; continue; } // skip attachments

        LLVector3d objPos = obj->getPositionGlobal();
        F64 dist = (objPos - agentPos).length();
        if (dist > radius) { ++outside_radius_count; continue; }

        LLNameValue* name_nv = obj->getNVPair("Name");
        LLNameValue* desc_nv = obj->getNVPair("Description");
        const std::string name = (name_nv && name_nv->getString()) ? name_nv->getString() : std::string();
        const std::string desc = (desc_nv && desc_nv->getString()) ? desc_nv->getString() : std::string();
        LLViewerObject* root = obj->getRootEdit();
        const bool is_root = (root == obj);
        LLVector3 scale = obj->getScale();

        LLSD entry;
        entry["id"]          = obj->getID().asString();
        entry["local_id"]    = (LLSD::Integer)obj->getLocalID();
        entry["name"]        = name;
        entry["desc"]        = desc;
        entry["parent_id"]   = (!is_root && root) ? root->getID().asString() : "";
        entry["is_root"]     = is_root;
        entry["pos"]         = LLSD::emptyArray();
        entry["pos"][0]      = objPos.mdV[VX];
        entry["pos"][1]      = objPos.mdV[VY];
        entry["pos"][2]      = objPos.mdV[VZ];
        entry["size"]        = LLSD::emptyArray();
        entry["size"][0]     = scale.mV[VX];
        entry["size"][1]     = scale.mV[VY];
        entry["size"][2]     = scale.mV[VZ];
        entry["dist"]        = dist;
        entry["has_touch"]   = (bool)obj->flagHandleTouch();
        entry["click_action"]= (LLSD::Integer)obj->getClickAction();
        entry["pcode"]       = (LLSD::Integer)obj->getPCode();
        // Rotation in region frame, and where the object's local X and Y axes
        // point: "size" is along those axes, so a road tile turned 90 deg or
        // laid diagonally covers different ground than an axis-aligned box
        // of the same size (her map put such roads on the grass, 2026-10-09).
        const LLQuaternion rot = obj->getRotationRegion();
        entry["rot"]         = LLSD::emptyArray();
        entry["rot"][0]      = rot.mQ[VX];
        entry["rot"][1]      = rot.mQ[VY];
        entry["rot"][2]      = rot.mQ[VZ];
        entry["rot"][3]      = rot.mQ[VW];
        const LLVector3 xAxis = LLVector3::x_axis * rot;
        const LLVector3 yAxis = LLVector3::y_axis * rot;
        entry["x_axis"]      = LLSD::emptyArray();
        entry["x_axis"][0]   = xAxis.mV[VX];
        entry["x_axis"][1]   = xAxis.mV[VY];
        entry["x_axis"][2]   = xAxis.mV[VZ];
        entry["y_axis"]      = LLSD::emptyArray();
        entry["y_axis"][0]   = yAxis.mV[VX];
        entry["y_axis"][1]   = yAxis.mV[VY];
        entry["y_axis"][2]   = yAxis.mV[VZ];
        ev["objects"].append(entry);
        ++returned_count;
    }

    ev["total_object_count"] = count;
    ev["returned_object_count"] = returned_count;
    ev["dead_or_null_count"] = dead_count;
    ev["avatar_count"] = avatar_count;
    ev["attachment_count"] = attachment_count;
    ev["outside_radius_count"] = outside_radius_count;
    LLAISocket::instance().sendEvent(ev);
    LL_INFOS("AI") << "get_objects total=" << count
                   << " returned=" << returned_count
                   << " dead_or_null=" << dead_count
                   << " avatars=" << avatar_count
                   << " attachments=" << attachment_count
                   << " outside_radius=" << outside_radius_count
                   << " radius=" << radius << LL_ENDL;
}

// ── touch ─────────────────────────────────────────────────────────────────────

void LLAICommands::cmdTouch(const LLSD& p)
{
    LLViewerObject* obj = nullptr;

    if (p.has("id"))
    {
        LLUUID id(p["id"].asString());
        obj = gObjectList.findObject(id);
    }
    else if (p.has("name"))
    {
        // Search by cached name in object name-value pairs
        std::string target = p["name"].asString();
        LLVector3d agentPos = gAgent.getPositionGlobal();
        F32 radius = p.has("radius") ? (F32)p["radius"].asReal() : 32.f;
        F64 bestDist = radius;

        S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* candidate = gObjectList.getObject(i);
            if (!candidate || candidate->isDead() || candidate->isAvatar()) continue;
            LLNameValue* nv = candidate->getNVPair("Name");
            if (!nv) continue;
            std::string objName = nv->printData();
            if (objName.find(target) == std::string::npos) continue;
            F64 dist = (candidate->getPositionGlobal() - agentPos).length();
            if (dist < bestDist) { bestDist = dist; obj = candidate; }
        }
    }

    if (!obj) { sendError("touch", "object_not_found"); return; }

    // RestrainedLove: a script she wears may forbid touching this.
    if (gRRenabled && !gAgent.mRRInterface.canTouch(obj))
    {
        sendError("touch", "rlv_blocked");
        return;
    }

    // Build a minimal pick. A HUD or collar often tells its buttons apart by
    // the face touched (the BOS Handler HUD: faces 0-3) or by where on it
    // (llDetectedTouchST/UV), so those can be given (2026-10-19).
    LLPickInfo pick;
    pick.mObjectID = obj->getID();
    if (p.has("face"))
    {
        pick.mObjectFace = (S32)p["face"].asInteger();
    }
    if (p.has("st") && p["st"].isArray() && p["st"].size() >= 2)
    {
        pick.mSTCoords = LLVector2((F32)p["st"][0].asReal(), (F32)p["st"][1].asReal());
    }
    if (p.has("uv") && p["uv"].isArray() && p["uv"].size() >= 2)
    {
        pick.mUVCoords = LLVector2((F32)p["uv"][0].asReal(), (F32)p["uv"][1].asReal());
    }

    send_ObjectGrab_message(obj, pick, LLVector3::zero);

    // Schedule DeGrab for next idle tick
    LLUUID objId = obj->getID();
    deferOnce([objId, pick]() mutable {
        LLViewerObject* o = gObjectList.findObject(objId);
        if (o) send_ObjectDeGrab_message(o, pick);
    });

    LLSD ev;
    ev["event"] = "touch_ok";
    ev["id"]    = obj->getID().asString();
    ev["face"]  = (LLSD::Integer)pick.mObjectFace;
    LLAISocket::instance().sendEvent(ev);
}

// ── worn items ────────────────────────────────────────────────────────────────

namespace
{
std::string lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// One worn object: its id, which attach point, HUD or not, its prims (link
// order as the viewer holds them) with their face counts; for her own, the
// inventory item and its name.
LLSD describe_attachment(LLViewerObject* obj, LLViewerJointAttachment* point, bool self)
{
    LLSD a;
    a["id"]    = obj->getID().asString();
    a["point"] = point ? point->getName() : std::string();
    a["hud"]   = point ? point->getIsHUDAttachment() : false;
    if (self)
    {
        const LLUUID& item_id = obj->getAttachmentItemID();
        a["item_id"] = item_id.asString();
        if (LLViewerInventoryItem* item = gInventory.getItem(item_id))
        {
            a["name"] = item->getName();
        }
    }
    a["links"] = LLSD::emptyArray();
    LLSD root;
    root["id"]    = obj->getID().asString();
    root["link"]  = 1;
    root["faces"] = (LLSD::Integer)obj->getNumTEs();
    a["links"].append(root);
    S32 link = 2;
    for (const auto& child : obj->getChildren())
    {
        if (!child) continue;
        LLSD c;
        c["id"]    = child->getID().asString();
        c["link"]  = link++;
        c["faces"] = (LLSD::Integer)child->getNumTEs();
        a["links"].append(c);
    }
    return a;
}

LLSD describe_avatar_attachments(LLVOAvatar* av, bool self, bool request_names)
{
    LLSD out;
    out["id"]   = av->getID().asString();
    out["self"] = self;
    out["attachments"] = LLSD::emptyArray();
    for (const auto& kv : av->mAttachmentPoints)
    {
        LLViewerJointAttachment* point = kv.second;
        if (!point) continue;
        for (const auto& objp : point->mAttachedObjects)
        {
            LLViewerObject* obj = objp.get();
            if (!obj || obj->isDead()) continue;
            out["attachments"].append(describe_attachment(obj, point, self));
            if (!self && request_names)
            {
                // Names of other people's worn items come back as
                // object_props events (llaievents.cpp onObjectProperties).
                LLSelectMgr::instance().requestObjectPropertiesFamily(obj);
            }
        }
    }
    return out;
}
}

void LLAICommands::cmdGetAttachments(const LLSD& p)
{
    if (!gAgentAvatarp.notNull()) { sendError("get_attachments", "no_avatar"); return; }
    const bool names = p.has("names") ? p["names"].asBoolean() : true;
    LLSD ev;
    ev["event"]   = "attachments";
    ev["avatars"] = LLSD::emptyArray();

    if (p.has("avatar_id"))
    {
        LLUUID id(p["avatar_id"].asString());
        LLViewerObject* obj = gObjectList.findObject(id);
        if (id == gAgent.getID())
        {
            ev["avatars"].append(describe_avatar_attachments(gAgentAvatarp, true, names));
        }
        else if (obj && obj->isAvatar())
        {
            ev["avatars"].append(describe_avatar_attachments((LLVOAvatar*)obj, false, names));
        }
        else
        {
            sendError("get_attachments", "avatar_not_found");
            return;
        }
    }
    else
    {
        // Her own, plus everyone within radius (default 10 m).
        ev["avatars"].append(describe_avatar_attachments(gAgentAvatarp, true, false));
        const F32 radius = p.has("radius") ? (F32)p["radius"].asReal() : 10.f;
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        LLWorld::instance().getAvatars(&ids, &positions, gAgent.getPositionGlobal(), radius);
        for (const LLUUID& id : ids)
        {
            if (id == gAgent.getID()) continue;
            LLViewerObject* obj = gObjectList.findObject(id);
            if (obj && obj->isAvatar())
            {
                ev["avatars"].append(describe_avatar_attachments((LLVOAvatar*)obj, false, names));
            }
        }
    }
    LLAISocket::instance().sendEvent(ev);
}

// ── inventory ─────────────────────────────────────────────────────────────────

void LLAICommands::cmdListInventory(const LLSD& p)
{
    const std::string query  = lower_copy(p.has("query") ? p["query"].asString() : std::string());
    const std::string folder = lower_copy(p.has("folder") ? p["folder"].asString() : std::string());
    const bool worn_only = p.has("worn_only") && p["worn_only"].asBoolean();
    const S32 limit = p.has("limit") ? (S32)p["limit"].asInteger() : 200;

    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH);

    LLSD ev;
    ev["event"]   = "inventory";
    ev["folders"] = LLSD::emptyArray();
    ev["items"]   = LLSD::emptyArray();
    if (!folder.empty())
    {
        for (const auto& cat : cats)
        {
            if (cat && lower_copy(cat->getName()).find(folder) != std::string::npos)
            {
                LLSD c;
                c["id"]     = cat->getUUID().asString();
                c["name"]   = cat->getName();
                c["parent"] = cat->getParentUUID().asString();
                ev["folders"].append(c);
            }
        }
    }
    S32 count = 0;
    bool truncated = false;
    for (const auto& item : items)
    {
        if (!item) continue;
        if (!query.empty() && lower_copy(item->getName()).find(query) == std::string::npos) continue;
        LLViewerInventoryCategory* parent = gInventory.getCategory(item->getParentUUID());
        if (!folder.empty() && (!parent || lower_copy(parent->getName()).find(folder) == std::string::npos)) continue;
        const bool worn = get_is_item_worn(item->getUUID());
        if (worn_only && !worn) continue;
        if (count >= limit) { truncated = true; break; }
        LLSD i;
        i["id"]     = item->getUUID().asString();
        i["name"]   = item->getName();
        i["type"]   = LLAssetType::lookup(item->getType());
        i["folder"] = parent ? parent->getName() : std::string();
        i["worn"]   = worn;
        i["link"]   = item->getIsLinkType();
        ev["items"].append(i);
        ++count;
    }
    ev["truncated"] = truncated;
    LLAISocket::instance().sendEvent(ev);
}

// ── wearing ───────────────────────────────────────────────────────────────────
// Through LLAppearanceMgr, as the inventory panel's Wear / Add / Take Off do,
// so RestrainedLove locks on her outfit apply just as they would to a person.

void LLAICommands::cmdWearItem(const LLSD& p)
{
    if (!p.has("id")) { sendError("wear_item", "missing_id"); return; }
    LLUUID id(p["id"].asString());
    if (!gInventory.getItem(id)) { sendError("wear_item", "item_not_found"); return; }
    const bool replace = p.has("replace") && p["replace"].asBoolean();
    LLAppearanceMgr::instance().wearItemOnAvatar(id, true /*do_update*/, replace);
    sendOk("wear_item");
}

void LLAICommands::cmdRemoveItem(const LLSD& p)
{
    if (!p.has("id")) { sendError("remove_item", "missing_id"); return; }
    LLUUID id(p["id"].asString());
    if (!gInventory.getItem(id)) { sendError("remove_item", "item_not_found"); return; }
    LLAppearanceMgr::instance().removeItemFromAvatar(id);
    sendOk("remove_item");
}

void LLAICommands::cmdWearOutfit(const LLSD& p)
{
    if (!p.has("folder_id")) { sendError("wear_outfit", "missing_folder_id"); return; }
    LLViewerInventoryCategory* cat = gInventory.getCategory(LLUUID(p["folder_id"].asString()));
    if (!cat) { sendError("wear_outfit", "folder_not_found"); return; }
    const bool append = p.has("append") && p["append"].asBoolean();
    LLAppearanceMgr::instance().wearInventoryCategory(cat, false /*copy*/, append);
    sendOk("wear_outfit");
}

// ── rezzing ───────────────────────────────────────────────────────────────────
// As dragging an object out of inventory onto the ground does
// (LLToolDragAndDrop::dropObject), at a spot she names: region metres "pos",
// or "ahead_m" in front of her (default 1.5 m). No-copy items leave her
// inventory, exactly as they would for a person.

void LLAICommands::cmdRezItem(const LLSD& p)
{
    if (!p.has("id")) { sendError("rez_item", "missing_id"); return; }
    LLViewerInventoryItem* item = gInventory.getItem(LLUUID(p["id"].asString()));
    if (!item || !item->isFinished()) { sendError("rez_item", "item_not_found"); return; }
    if (item->getType() != LLAssetType::AT_OBJECT) { sendError("rez_item", "not_an_object"); return; }
    if (gRRenabled && (gAgent.mRRInterface.mContainsRez || gAgent.mRRInterface.mContainsInteract))
    {
        sendError("rez_item", "rlv_blocked");
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) { sendError("rez_item", "no_region"); return; }

    LLVector3 target;
    if (p.has("pos") && p["pos"].isArray() && p["pos"].size() >= 3)
    {
        target.set((F32)p["pos"][0].asReal(), (F32)p["pos"][1].asReal(), (F32)p["pos"][2].asReal());
    }
    else
    {
        const F32 ahead = p.has("ahead_m") ? (F32)p["ahead_m"].asReal() : 1.5f;
        LLVector3 at = gAgent.getFrameAgent().getAtAxis();
        at.mV[VZ] = 0.f;
        at.normVec();
        target = gAgent.getPositionAgent() + at * ahead;
    }

    const bool remove_from_inventory = !item->getPermissions().allowCopyBy(gAgent.getID());

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_RezObject);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
    msg->nextBlock("RezData");
    msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
    msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
    msg->addVector3Fast(_PREHASH_RayStart, target + LLVector3(0.f, 0.f, 1.f));
    msg->addVector3Fast(_PREHASH_RayEnd, target);
    msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
    msg->addBOOLFast(_PREHASH_RayEndIsIntersection, false);
    msg->addBOOLFast(_PREHASH_RezSelected, false);
    msg->addBOOLFast(_PREHASH_RemoveItem, remove_from_inventory);
    pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
    msg->nextBlockFast(_PREHASH_InventoryData);
    item->packMessage(msg);
    msg->sendReliable(region->getHost());

    LLSD ev;
    ev["event"]   = "rez_sent";
    ev["id"]      = item->getUUID().asString();
    ev["name"]    = item->getName();
    ev["pos"]     = LLSD::emptyArray();
    ev["pos"][0]  = target.mV[VX];
    ev["pos"][1]  = target.mV[VY];
    ev["pos"][2]  = target.mV[VZ];
    ev["removed_from_inventory"] = remove_from_inventory;
    LLAISocket::instance().sendEvent(ev);
}

// ── request_object_props ──────────────────────────────────────────────────────

void LLAICommands::cmdRequestObjectProps(const LLSD& p)
{
    if (!p.has("id")) { sendError("request_object_props", "missing_id"); return; }
    LLUUID id(p["id"].asString());
    LLViewerObject* obj = gObjectList.findObject(id);
    if (!obj) { sendError("request_object_props", "object_not_found"); return; }

    LLSelectMgr::instance().requestObjectPropertiesFamily(obj);
    // Response fires llaievents.cpp::onObjectProperties() when the sim replies
    sendOk("request_object_props");
}

// ── navigate_to ───────────────────────────────────────────────────────────────

void LLAICommands::cmdNavigateTo(const LLSD& p)
{
    if (!p.has("x") || !p.has("y") || !p.has("z"))
    { sendError("navigate_to", "missing_coords"); return; }

    mMove.active = false;
    mMove.dir.clear();
    mNav.active     = true;
    mNav.stopDist   = p.has("stop_dist") ? (F32)p["stop_dist"].asReal() : 2.f;
    mNav.stuckCount = 0;

    if (p.has("global") && p["global"].asBoolean())
    {
        mNav.target = LLVector3d(p["x"].asReal(), p["y"].asReal(), p["z"].asReal());
    }
    else if (gAgent.getRegion())
    {
        LLVector3d origin = gAgent.getRegion()->getOriginGlobal();
        mNav.target = origin + LLVector3d(p["x"].asReal(), p["y"].asReal(), p["z"].asReal());
    }
    else { sendError("navigate_to", "no_region"); return; }

    mNav.lastPos      = gAgent.getPositionGlobal();
    mNav.lastMoveTime = LLTimer::getElapsedSeconds();

    sendOk("navigate_to");
}

// Stop any navigation turn. moveYaw sets a yaw key that keeps turning her
// after the key flags are cleared; without zeroing it she kept turning past
// the target heading and orbited it, and the turn carried over into the
// next walk (2026-10-10). LLAIState::cancelTurn does the same.
static void stop_nav_turn()
{
    gAgent.clearControlFlags(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
    gAgentCamera.setYawKey(0.f);
}

void LLAICommands::cmdNavigateStop(const LLSD& /*p*/)
{
    mMove.active = false;
    mMove.dir.clear();
    mNav.active = false;
    gAgent.clearControlFlags(
        AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
        AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG);
    stop_nav_turn();
    sendOk("navigate_stop");
}

void LLAICommands::tickNavigation()
{
    if (!mNav.active || !gAgent.getRegion()) return;

    LLVector3d curPos = gAgent.getPositionGlobal();

    // XY distance only
    LLVector3d delta = mNav.target - curPos;
    delta.mdV[VZ] = 0.0;
    F64 dist = delta.length();

    if (dist <= mNav.stopDist)
    {
        // Arrived
        mNav.active = false;
        gAgent.clearControlFlags(AGENT_CONTROL_AT_POS);
        stop_nav_turn();
        LLSD ev;
        ev["event"] = "navigate_ok";
        ev["pos"]   = LLSD::emptyArray();
        ev["pos"][0] = curPos.mdV[VX];
        ev["pos"][1] = curPos.mdV[VY];
        ev["pos"][2] = curPos.mdV[VZ];
        LLAISocket::instance().sendEvent(ev);
        return;
    }

    // Face the target. Headings here are compass degrees (clockwise from
    // north, atan2(x, y)) but a positive yaw key turns counter-clockwise, so
    // the key takes the opposite sign of the heading error -- as in
    // LLAIState::updateTurnController. With the sign the same, every
    // correction turned her further away until she settled facing straight
    // away from the target and walked off backwards (2026-10-09).
    F32 targetDeg = (F32)(std::atan2(delta.mdV[VX], delta.mdV[VY]) * RAD_TO_DEG);
    LLVector3 at  = gAgent.getFrameAgent().getAtAxis();
    F32 curDeg    = (F32)(std::atan2(at.mV[VX], at.mV[VY]) * RAD_TO_DEG);
    F32 yawDelta  = targetDeg - curDeg;
    while (yawDelta >  180.f) yawDelta -= 360.f;
    while (yawDelta < -180.f) yawDelta += 360.f;

    if (std::abs(yawDelta) > 5.f)
    {
        const F32 yawKey = llclamp(std::abs(yawDelta) / 30.f, 0.15f, 1.f);
        gAgent.clearControlFlags(AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG);
        gAgent.moveYaw(yawDelta > 0.f ? -yawKey : yawKey);
    }
    else
    {
        stop_nav_turn(); // lined up: stop turning
    }

    // Turn mostly round before stepping off, rather than walking a wide arc.
    if (std::abs(yawDelta) < 60.f)
        gAgent.moveAt(1); // walk forward

    // Stuck detection: if position barely moved in 3 seconds
    F64 now = LLTimer::getElapsedSeconds();
    if ((curPos - mNav.lastPos).length() < 0.3)
    {
        if ((now - mNav.lastMoveTime) > 3.0)
        {
            ++mNav.stuckCount;
            mNav.lastMoveTime = now;

            LLSD ev;
            ev["event"]       = "navigate_stuck";
            ev["stuck_count"] = (LLSD::Integer)mNav.stuckCount;
            ev["dist"]        = dist;
            LLAISocket::instance().sendEvent(ev);

            // Try a small random yaw to get unstuck
            gAgent.moveYaw(45.f * DEG_TO_RAD * (mNav.stuckCount % 2 == 0 ? 1.f : -1.f));

            if (mNav.stuckCount >= 5)
            {
                // Give up
                mNav.active = false;
                gAgent.clearControlFlags(AGENT_CONTROL_AT_POS);
                stop_nav_turn();
                LLSD fail;
                fail["event"]  = "navigate_failed";
                fail["reason"] = "stuck";
                LLAISocket::instance().sendEvent(fail);
            }
        }
    }
    else
    {
        mNav.lastPos      = curPos;
        mNav.lastMoveTime = now;
    }
}

// ── get_parcel_info ───────────────────────────────────────────────────────────

void LLAICommands::cmdGetParcelInfo(const LLSD& cmd)
{
    LLViewerRegion* region = gAgent.getRegion();
    LLParcel* parcel = LLViewerParcelMgr::instance().getAgentParcel();

    LLSD ev;
    ev["event"] = "parcel_info";
    ev["cmd"]   = "get_parcel_info";

    if (!parcel || !region)
    {
        ev["ok"]     = false;
        ev["reason"] = "no_parcel";
        LLAISocket::instance().sendEvent(ev);
        return;
    }

    // Parcel identity
    ev["ok"]          = true;
    ev["parcel_name"] = parcel->getName();
    ev["parcel_desc"] = parcel->getDesc();
    ev["local_id"]    = (LLSD::Integer)parcel->getLocalID();

    // AABB bounds in local region coordinates (metres from region origin)
    const LLVector3& bmin = parcel->getAABBMin();
    const LLVector3& bmax = parcel->getAABBMax();
    ev["aabb_min_x"] = (LLSD::Real)bmin.mV[VX];
    ev["aabb_min_y"] = (LLSD::Real)bmin.mV[VY];
    ev["aabb_max_x"] = (LLSD::Real)bmax.mV[VX];
    ev["aabb_max_y"] = (LLSD::Real)bmax.mV[VY];
    ev["aabb_min_z"] = (LLSD::Real)bmin.mV[VZ];
    ev["aabb_max_z"] = (LLSD::Real)bmax.mV[VZ];

    // Region origin in global coords so controller can compute global parcel bounds
    LLVector3d origin = region->getOriginGlobal();
    ev["region_origin_x"] = (LLSD::Real)origin.mdV[VX];
    ev["region_origin_y"] = (LLSD::Real)origin.mdV[VY];
    ev["region_name"]     = region->getName();

    LLAISocket::instance().sendEvent(ev);
}

// ── helpers ───────────────────────────────────────────────────────────────────

void LLAICommands::sendOk(const std::string& cmd)
{
    LLSD ev;
    ev["event"] = "ok";
    ev["cmd"]   = cmd;
    LLAISocket::instance().sendEvent(ev);
}

void LLAICommands::sendError(const std::string& cmd, const std::string& reason)
{
    LLSD ev;
    ev["event"]  = "error";
    ev["cmd"]    = cmd;
    ev["reason"] = reason;
    LLAISocket::instance().sendEvent(ev);
    LL_WARNS(LOG_AI) << "AI cmd error: " << cmd << " → " << reason << LL_ENDL;
}
