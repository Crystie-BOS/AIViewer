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
#include "lluuid.h"

static constexpr const char* LOG_AI = "AI";

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
    ev["from_id"]    = data.has("from_id") ? LLSD(data["from_id"].asString()) : LLSD("");
    ev["text"]       = data.has("message") ? data["message"] : LLSD("");
    ev["session_id"] = data.has("session_id") ? LLSD(data["session_id"].asString()) : LLSD("");
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

    // The buttons, in order, so the controller can read a menu and press one
    // by its position -- a dance machine's or a teleport board's menu was
    // only a title to her before: labels like "Female ||" could not be
    // guessed (2026-10-03).
    LLSD buttons = LLSD::emptyArray();
    LLNotificationFormPtr form = notif->getForm();
    if (form)
    {
        for (S32 i = 0; i < form->getNumElements(); ++i)
        {
            LLSD el = form->getElement(i);
            if (el["type"].asString() != "button") continue;
            LLSD b;
            b["name"] = el["name"];
            b["text"] = el.has("text") ? el["text"] : el["name"];
            buttons.append(b);
        }
    }
    ev["buttons"] = buttons;
    const LLSD& payload = notif->getPayload();
    if (payload.has("object_id")) ev["object_id"] = payload["object_id"];
    if (payload.has("chat_channel")) ev["chat_channel"] = payload["chat_channel"];
    // Who sent it, by avatar UUID: a teleport offer is only accepted from
    // trusted people, and names are not identities (2026-10-11).
    if (payload.has("from_id")) ev["from_id"] = payload["from_id"].asString();
    if (payload.has("from_name")) ev["from_name"] = payload["from_name"];

    // Flag important categories
    if (type.find("ObjectGiveItem") != std::string::npos ||
        type.find("UserGiveItem") != std::string::npos)
    {
        ev["category"] = "inventory_offer";
    }
    else if (type.find("TeleportOffered") != std::string::npos)
    {
        ev["category"] = "teleport_offer";
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

// ── object properties (called from llselectmgr.cpp patch) ────────────────────

void LLAIEvents::onObjectProperties(const LLUUID& id, const std::string& name,
                                     const std::string& desc, const LLUUID& owner_id,
                                     const LLUUID& creator_id)
{
    LLSD ev;
    ev["event"]      = "object_props";
    ev["id"]         = id.asString();
    ev["name"]       = name;
    ev["desc"]       = desc;
    ev["owner_id"]   = owner_id.asString();
    ev["creator_id"] = creator_id.asString();
    LLAISocket::instance().sendEvent(ev);
}

// Free function called from llselectmgr.cpp via extern declaration
void ai_on_object_properties(const LLUUID& id, const std::string& name,
                              const std::string& desc, const LLUUID& owner_id,
                              const LLUUID& creator_id)
{
    LLAIEvents::instance().onObjectProperties(id, name, desc, owner_id, creator_id);
}
