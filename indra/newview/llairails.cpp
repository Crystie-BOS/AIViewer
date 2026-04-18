/**
 * llairails.cpp
 * Safety rails implementation.
 */

#include "llviewerprecompiledheaders.h"
#include "llairails.h"
#include "llfile.h"
#include "lldiriterator.h"
#include "lldir.h"
#include "lllog.h"
#include <fstream>
#include <sstream>
#include <ctime>

static const std::string LOG_AI("AI");

LLAIRails::LLAIRails()
{
    // Default rate limits (per minute, 0 = unlimited)
    mLimits["chat"]      = 10;
    mLimits["im"]        = 10;
    mLimits["teleport"]  = 5;
    mLimits["teleport_landmark"] = 5;
    mLimits["screenshot"] = 20;
}

void LLAIRails::loadConfig()
{
    // Audit log path: ~/.config/ai-viewer/audit.log (Linux) or app data dir
    std::string configDir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "");
    // Trim trailing separator
    while (!configDir.empty() && (configDir.back() == '/' || configDir.back() == '\\'))
        configDir.pop_back();

    mAuditPath = configDir + "/ai_audit.log";

    // Load allowed_regions.txt
    std::string regionsFile = configDir + "/allowed_regions.txt";
    std::ifstream rf(regionsFile);
    if (rf.is_open())
    {
        std::string line;
        while (std::getline(rf, line))
        {
            // trim
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) mAllowedRegions.insert(line);
        }
        if (!mAllowedRegions.empty())
            LL_INFOS(LOG_AI) << "AI rails: loaded " << mAllowedRegions.size()
                             << " allowed regions" << LL_ENDL;
    }

    // Load ai_rails.json for custom rate limits (optional)
    // (skipped for now — defaults are sane, users can extend later)
}

bool LLAIRails::allow(const std::string& cmd, const LLSD& /*params*/, std::string& reason)
{
    if (mPaused && cmd != "resume")
    {
        reason = "paused";
        return false;
    }
    return checkRate(cmd, reason);
}

bool LLAIRails::regionAllowed(const std::string& region_name) const
{
    if (mAllowedRegions.empty()) return true;
    return mAllowedRegions.count(region_name) > 0;
}

void LLAIRails::audit(const std::string& cmd, const std::string& outcome)
{
    if (mAuditPath.empty()) return;

    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));

    std::string entry = std::string("{\"ts\":\"") + ts + "\",\"cmd\":\"" + cmd
                        + "\",\"outcome\":\"" + outcome + "\"}\n";

    std::lock_guard<std::mutex> lk(mAuditMutex);
    std::ofstream af(mAuditPath, std::ios::app);
    if (af.is_open()) af << entry;
}

bool LLAIRails::checkRate(const std::string& cmd, std::string& reason)
{
    auto it = mLimits.find(cmd);
    if (it == mLimits.end() || it->second == 0) return true; // unlimited

    int limit = it->second;
    auto now  = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(60);

    std::lock_guard<std::mutex> lk(mRateMutex);
    auto& rw = mRateWindows[cmd];

    // Evict old entries outside the 60-second window
    while (!rw.hits.empty() && (now - rw.hits.front()) > window)
        rw.hits.pop_front();

    if ((int)rw.hits.size() >= limit)
    {
        reason = "rate_limited";
        return false;
    }

    rw.hits.push_back(now);
    return true;
}
