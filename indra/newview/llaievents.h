/**
 * llaievents.h
 * Event hooks: connects viewer signals to AI socket events.
 *
 * Chat hook: call LLAIEvents::instance().onChat(chat) from
 *   LLFloaterIMNearbyChat::addMessage().
 *
 * IM hook: connected via LLIMModel::addNewMsgCallback() in start().
 *
 * Dialog hook: connected via LLNotifications::instance().connectChanged().
 */

#ifndef LL_LLAIEVENTS_H
#define LL_LLAIEVENTS_H

#include "llsingleton.h"
#include "llnotifications.h"
#include "llsd.h"
#include "llchat.h"
#include <vector>
#include <boost/signals2.hpp>

class LLAIEvents : public LLSingleton<LLAIEvents>
{
    LLSINGLETON(LLAIEvents);

public:
    // Connect all signals. Called from LLAppViewer::handleLoginComplete().
    void start();

    // Disconnect all signals.
    void stop();

    // Called from LLFloaterIMNearbyChat::addMessage() for local chat events.
    void onChat(const LLChat& chat);

private:
    bool onIMMessage(const LLSD& data);
    bool onNotification(const LLSD& data);

    std::vector<boost::signals2::scoped_connection> mConnections;
    bool mStarted { false };
};

#endif // LL_LLAIEVENTS_H
