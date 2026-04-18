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
#include "llnotificationsutil.h"
#include "llcallbacklist.h"
#include "llsd.h"
#include "lllog.h"
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
    LLSD cmd;
    while (LLAISocket::instance().pollCommand(cmd))
        dispatch(cmd);
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

    if      (name == "move")               cmdMove(cmd);
    else if (name == "turn")               cmdTurn(cmd);
    else if (name == "face")               cmdFace(cmd);
    else if (name == "chat")               cmdChat(cmd);
    else if (name == "im")                 cmdIM(cmd);
    else if (name == "teleport")           cmdTeleport(cmd);
    else if (name == "teleport_landmark")  cmdTeleportLandmark(cmd);
    else if (name == "sit")                cmdSit(cmd);
    else if (name == "stand")              cmdStand(cmd);
    else if (name == "screenshot")         cmdScreenshot(cmd);
    else if (name == "get_state")          cmdGetState(cmd);
    else if (name == "get_nearby")         cmdGetNearby(cmd);
    else if (name == "pause")              cmdPause(cmd);
    else if (name == "resume")             cmdResume(cmd);
    else if (name == "dismiss_dialog")     cmdDismissDialog(cmd);
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

    // Open/reuse session and send
    std::string text = p["text"].asString();
    LLUUID sessionId = gIMMgr->computeSessionID(IM_NOTHING_SPECIAL, toId);
    gIMMgr->addMessage(sessionId, gAgent.getID(), gAgent.getName(), text,
                       LLStringUtil::null, IM_NOTHING_SPECIAL, 0, LLUUID::null,
                       LLVector3::zero, true);
    pack_instant_message(gMessageSystem, gAgent.getID(), false, gAgent.getSessionID(),
                         toId, gAgent.getName(), text, IM_ONLINE, IM_NOTHING_SPECIAL,
                         sessionId, 0, LLUUID::null);
    gAgent.sendReliableMessage();
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
