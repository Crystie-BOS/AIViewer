/**
 * llaievents.cpp
 * Signal connections and event marshaling.
 */

#include "llviewerprecompiledheaders.h"
#include "llaievents.h"
#include "llaisocket.h"
#include "llimview.h"
#include "llnotifications.h"
#include "llchat.h"
#include "llsd.h"
#include "lllog.h"
#include "lluuid.h"

static const std::string LOG_AI("AI");

LLAIEvents::LLAIEvents() = default;

void LLAIEvents::start()
{
    if (mStarted) return;

    // IM new-message signal
    auto imConn = LLIMModel::instance().addNewMsgCallback(
        [this](const LLSD& data) { return onIMMessage(data); });
    mConnections.emplace_back(std::move(imConn));

    // All-notifications signal via the LLNotifications root channel
    auto notifConn = LLNotifications::instance().connectChanged(
        [this](const LLSD& data) { return onNotification(data); });
    mConnections.emplace_back(std::move(notifConn));

    mStarted = true;
    LL_INFOS(LOG_AI) << "AI event hooks started" << LL_ENDL;
}

void LLAIEvents::stop()
{
    mConnections.clear();
    mStarted = false;
}

// ── chat (called from LLFloaterIMNearbyChat::addMessage) ─────────────────────

void LLAIEvents::onChat(const LLChat& chat)
{
    // Skip system/invisible messages and our own outgoing messages
    if (chat.mChatType == CHAT_TYPE_START || chat.mChatType == CHAT_TYPE_STOP) return;
    if (chat.mSourceType == CHAT_SOURCE_SYSTEM && chat.mText.empty()) return;

    LLSD ev;
    ev["event"]   = "chat";
    ev["from"]    = chat.mFromName;
    ev["from_id"] = chat.mFromID.asString();
    ev["text"]    = chat.mText;
    ev["channel"] = (LLSD::Integer)0; // local chat is always ch 0
    ev["source"]  = (LLSD::Integer)chat.mSourceType;
    ev["ts"]      = (LLSD::Integer)LLTimer::getTotalSeconds();
    LLAISocket::instance().sendEvent(ev);
}

// ── IM ────────────────────────────────────────────────────────────────────────

bool LLAIEvents::onIMMessage(const LLSD& data)
{
    // data keys: session_id, from_id, from, message, time
    LLSD ev;
    ev["event"]      = "im";
    ev["from"]       = data.has("from") ? data["from"] : LLSD("");
    ev["from_id"]    = data.has("from_id") ? data["from_id"].asString() : LLSD("");
    ev["text"]       = data.has("message") ? data["message"] : LLSD("");
    ev["session_id"] = data.has("session_id") ? data["session_id"].asString() : LLSD("");
    ev["ts"]         = (LLSD::Integer)LLTimer::getTotalSeconds();
    LLAISocket::instance().sendEvent(ev);
    return false; // do not consume — let UI handle it too
}

// ── notifications/dialogs ─────────────────────────────────────────────────────

bool LLAIEvents::onNotification(const LLSD& data)
{
    // data["sigtype"] = "add" | "change" | "delete"
    // We only care about new ("add") notifications
    std::string sigtype = data.has("sigtype") ? data["sigtype"].asString() : "";
    if (sigtype != "add") return false;

    LLNotificationPtr notif = LLNotifications::instance().find(data["id"].asUUID());
    if (!notif) return false;

    std::string type = notif->getName();

    LLSD ev;
    ev["event"]  = "dialog";
    ev["type"]   = type;
    ev["id"]     = notif->getID().asString();
    ev["msg"]    = notif->getMessage();

    // Flag important categories
    if (type.find("ObjectGiveItem") != std::string::npos ||
        type.find("UserGiveItem") != std::string::npos)
    {
        ev["category"] = "inventory_offer";
    }
    else if (type.find("Permission") != std::string::npos ||
             type.find("ScriptQuestion") != std::string::npos)
    {
        ev["category"] = "permission_request";
    }
    else if (type.find("JoinGroup") != std::string::npos ||
             type.find("GroupInvitation") != std::string::npos)
    {
        ev["category"] = "group_invite";
    }
    else if (type.find("AddFriend") != std::string::npos ||
             type.find("FriendshipOffer") != std::string::npos)
    {
        ev["category"] = "friend_request";
    }
    else
    {
        ev["category"] = "general";
    }

    LLAISocket::instance().sendEvent(ev);
    return false; // do not suppress — UI still shows it
}
