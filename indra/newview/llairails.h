/**
 * llairails.h
 * Safety rails for AI API: rate limiting, pause mode, audit log, region allowlist.
 */

#ifndef LL_LLAIRAILS_H
#define LL_LLAIRAILS_H

#include "llsingleton.h"
#include "llsd.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class LLAIRails : public LLSingleton<LLAIRails>
{
    LLSINGLETON(LLAIRails);

public:
    // Returns true if the command is allowed. On false, reason is set.
    bool allow(const std::string& cmd, const LLSD& params, std::string& reason);

    // Write command + outcome to audit log.
    void audit(const std::string& cmd, const std::string& outcome);

    void setPaused(bool p) { mPaused = p; }
    bool isPaused()  const { return mPaused; }

    // Call once at startup to load allowed_regions.txt and ai_rails.json.
    void loadConfig();

    // Check teleport destination against allowlist (empty list = allow all).
    bool regionAllowed(const std::string& region_name) const;

private:
    bool checkRate(const std::string& cmd, std::string& reason);

    bool mPaused { false };

    // Rate limiter: sliding window counts per command bucket
    struct RateWindow
    {
        std::deque<std::chrono::steady_clock::time_point> hits;
    };
    std::unordered_map<std::string, RateWindow> mRateWindows;
    std::mutex mRateMutex;

    // Per-bucket limits (commands per minute, 0 = unlimited)
    std::unordered_map<std::string, int> mLimits;

    std::unordered_set<std::string> mAllowedRegions; // empty = allow all

    std::string mAuditPath;
    std::mutex  mAuditMutex;
};

#endif // LL_LLAIRAILS_H
