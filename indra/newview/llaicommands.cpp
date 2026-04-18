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
#include "llagentcamera.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "llviewerregion.h"
#include "llviewerobjectlist.h"
#include "llviewerobject.h"
#include "llselectmgr.h"
#include "lltoolgrab.h"
#include "llnotificationsutil.h"
#include "llviewermessage.h"
#include "llcallbacklist.h"
#include "llcoordframe.h"
#include "llsd.h"
#include "indra_constants.h"
#include <cmath>

// Forward declaration — defined in llfloaterimnearbychat.cpp
void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);

static const std::string LOG_AI("AI");

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
    else if (name == "turn")                  cmdTurn(cmd);
    else if (name == "face")                  cmdFace(cmd);
    else if (name == "chat")                  cmdChat(cmd);
    else if (name == "im")                    cmdIM(cmd);
    else if (name == "teleport")              cmdTeleport(cmd);
    else if (name == "teleport_landmark")     cmdTeleportLandmark(cmd);
    else if (name == "sit")                   cmdSit(cmd);
    else if (name == "stand")                 cmdStand(cmd);
    else if (name == "screenshot")            cmdScreenshot(cmd);
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
    if      (dir == "forward")  gAgent.moveAt(1);
    else if (dir == "back")     gAgent.moveAt(-1);
    else if (dir == "left")     gAgent.moveLeft(1);
    else if (dir == "right")    gAgent.moveLeft(-1);
    else if (dir == "up")       gAgent.moveUp(1);
    else if (dir == "down")     gAgent.moveUp(-1);
    else if (dir == "stop")
    {
        gAgent.clearControlFlags(
            AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
            AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
            AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG);
    }
    else { sendError("move", "invalid_dir"); return; }
    sendOk("move");
}

void LLAICommands::cmdTurn(const LLSD& p)
{
    if (!p.has("degrees")) { sendError("turn", "missing_degrees"); return; }
    F32 deg = (F32)p["degrees"].asReal();
    // moveYaw takes radians/frame — call it once for a discrete nudge
    gAgent.moveYaw(deg * DEG_TO_RAD);
    sendOk("turn");
}

void LLAICommands::cmdFace(const LLSD& p)
{
    if (!p.has("heading")) { sendError("face", "missing_heading"); return; }
    F32 targetDeg = (F32)p["heading"].asReal();

    // Current heading
    LLVector3 at = gAgent.getFrameAgent().getAtAxis();
    F32 curDeg   = (F32)(std::atan2(at.mV[VX], at.mV[VY]) * RAD_TO_DEG);
    if (curDeg < 0) curDeg += 360.f;

    F32 delta = targetDeg - curDeg;
    // Wrap to [-180, 180]
    while (delta >  180.f) delta -= 360.f;
    while (delta < -180.f) delta += 360.f;

    gAgent.moveYaw(delta * DEG_TO_RAD);
    sendOk("face");
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

    send_improved_im(toId, gAgent.getName(), p["text"].asString());
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

    std::string path = p["path"].asString();
    bool ok = gViewerWindow->saveSnapshot(
        path, w, h,
        /*show_ui*/false, /*show_hud*/false, /*do_rebuild*/false, /*show_balance*/false,
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

// ── helpers ───────────────────────────────────────────────────────────────────

// ── get_objects ───────────────────────────────────────────────────────────────

void LLAICommands::cmdGetObjects(const LLSD& p)
{
    if (!gAgent.getRegion()) { sendError("get_objects", "no_region"); return; }

    F32 radius = p.has("radius") ? (F32)p["radius"].asReal() : 96.f;
    LLVector3d agentPos = gAgent.getPositionGlobal();

    LLSD ev;
    ev["event"]   = "nearby_objects";
    ev["objects"] = LLSD::emptyArray();

    S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->isDead() || obj->isAvatar()) continue;
        if (obj->isAttachment()) continue; // skip attachments

        LLVector3d objPos = obj->getPositionGlobal();
        F64 dist = (objPos - agentPos).length();
        if (dist > radius) continue;

        LLSD entry;
        entry["id"]          = obj->getID().asString();
        entry["pos"]         = LLSD::emptyArray();
        entry["pos"][0]      = objPos.mdV[VX];
        entry["pos"][1]      = objPos.mdV[VY];
        entry["pos"][2]      = objPos.mdV[VZ];
        entry["dist"]        = dist;
        entry["has_touch"]   = (bool)obj->flagHandleTouch();
        entry["click_action"]= (LLSD::Integer)obj->getClickAction();
        entry["pcode"]       = (LLSD::Integer)obj->getPCode();
        ev["objects"].append(entry);
    }

    LLAISocket::instance().sendEvent(ev);
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

    // Build a minimal pick
    LLPickInfo pick;
    pick.mObjectID = obj->getID();

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

void LLAICommands::cmdNavigateStop(const LLSD& /*p*/)
{
    mNav.active = false;
    gAgent.clearControlFlags(
        AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
        AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG);
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
        LLSD ev;
        ev["event"] = "navigate_ok";
        ev["pos"]   = LLSD::emptyArray();
        ev["pos"][0] = curPos.mdV[VX];
        ev["pos"][1] = curPos.mdV[VY];
        ev["pos"][2] = curPos.mdV[VZ];
        LLAISocket::instance().sendEvent(ev);
        return;
    }

    // Face the target
    F32 targetDeg = (F32)(std::atan2(delta.mdV[VX], delta.mdV[VY]) * RAD_TO_DEG);
    LLVector3 at  = gAgent.getFrameAgent().getAtAxis();
    F32 curDeg    = (F32)(std::atan2(at.mV[VX], at.mV[VY]) * RAD_TO_DEG);
    F32 yawDelta  = targetDeg - curDeg;
    while (yawDelta >  180.f) yawDelta -= 360.f;
    while (yawDelta < -180.f) yawDelta += 360.f;

    if (std::abs(yawDelta) > 5.f)
        gAgent.moveYaw(yawDelta * DEG_TO_RAD * 0.1f);

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
