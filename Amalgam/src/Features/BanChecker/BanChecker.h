#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <chrono>

struct BanInfo_t
{
    int m_iSourceBanCount = 0;
    bool m_bVACBanned = false;
    int m_iVACBanCount = 0;
    bool m_bGameBanned = false;
    int m_iGameBanCount = 0;
    bool m_bCommunityBanned = false;
    int m_iDaysSinceLastBan = 0;
    bool m_bFetched = false;
    bool m_bSteamAPIFetched = false;
    std::vector<std::string> m_vBanDetails;
};

class CBanChecker
{
public:
    struct FetchResult_t
    {
        std::string sResponse;
        std::vector<uint32_t> vAccountIDs;
    };

    std::unordered_map<uint32_t, BanInfo_t> m_mBanCache;
    std::mutex m_tMutex;

    void Initialize(const std::string& sAPIKey = "");
    void Shutdown();
    void QueueCheck(uint32_t uAccountID);
    BanInfo_t GetBanInfo(uint32_t uAccountID);
    bool IsCached(uint32_t uAccountID);
    void ClearCache();
    void SaveCache();
    void LoadCache();

private:
    std::string m_sAPIKey = ""; //steamhistory api key
    std::string m_sSteamAPIKey = ""; // steam api key
    std::queue<uint32_t> m_qPendingChecks;
    std::queue<uint32_t> m_qPendingSteamChecks;
    std::atomic<bool> m_bRunning = false;
    std::thread m_tWorkerThread;
    std::thread m_tSteamWorkerThread;
    std::unordered_set<uint32_t> m_sProcessing;
    std::unordered_set<uint32_t> m_sSteamProcessing;

    std::chrono::steady_clock::time_point m_tLastRequest;
    std::chrono::steady_clock::time_point m_tLastSteamRequest;
    const int m_iRequestDelay = 1000;
    const int m_iSteamRequestDelay = 1000;

    void WorkerThread();
    void SteamWorkerThread();
    FetchResult_t FetchBanData(const std::vector<uint32_t>& vAccountIDs);
    FetchResult_t FetchSteamBanData(const std::vector<uint32_t>& vAccountIDs);
    void ProcessBanResponse(const std::string& sResponse, const std::vector<uint32_t>& vAccountIDs);
    void ProcessSteamBanResponse(const std::string& sResponse, const std::vector<uint32_t>& vAccountIDs);
};

inline CBanChecker g_BanChecker;