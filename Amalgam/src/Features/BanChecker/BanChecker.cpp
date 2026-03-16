#include "BanChecker.h"
#include "../../SDK/SDK.h"
#include "../Configs/Configs.h"

#include <windows.h>
#include <winhttp.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <fstream>

#pragma comment(lib, "winhttp.lib")

void CBanChecker::Initialize(const std::string& sAPIKey)
{
    if (!sAPIKey.empty())
        m_sAPIKey = sAPIKey;

    m_bRunning = true;
    m_tLastRequest = std::chrono::steady_clock::now() - std::chrono::milliseconds(m_iRequestDelay);
    m_tLastSteamRequest = std::chrono::steady_clock::now() - std::chrono::milliseconds(m_iSteamRequestDelay);
    LoadCache();
    m_tWorkerThread = std::thread(&CBanChecker::WorkerThread, this);
    m_tSteamWorkerThread = std::thread(&CBanChecker::SteamWorkerThread, this);
}

void CBanChecker::Shutdown()
{
    m_bRunning = false;
    if (m_tWorkerThread.joinable())
        m_tWorkerThread.join();
    if (m_tSteamWorkerThread.joinable())
        m_tSteamWorkerThread.join();

    SaveCache();
}

void CBanChecker::QueueCheck(uint32_t uAccountID)
{
    std::lock_guard<std::mutex> lock(m_tMutex);

    if (IsCached(uAccountID))
        return;

    if (!m_sProcessing.contains(uAccountID))
    {
        m_sProcessing.insert(uAccountID);
        m_qPendingChecks.push(uAccountID);
    }

    if (!m_sSteamProcessing.contains(uAccountID))
    {
        m_sSteamProcessing.insert(uAccountID);
        m_qPendingSteamChecks.push(uAccountID);
    }
}

BanInfo_t CBanChecker::GetBanInfo(uint32_t uAccountID)
{
    std::lock_guard<std::mutex> lock(m_tMutex);

    if (m_mBanCache.contains(uAccountID))
        return m_mBanCache[uAccountID];

    return BanInfo_t();
}

bool CBanChecker::IsCached(uint32_t uAccountID)
{
    return m_mBanCache.contains(uAccountID) &&
        m_mBanCache[uAccountID].m_bFetched &&
        m_mBanCache[uAccountID].m_bSteamAPIFetched;
}

void CBanChecker::WorkerThread()
{
    while (m_bRunning)
    {
        std::vector<uint32_t> vBatch;

        {
            std::lock_guard<std::mutex> lock(m_tMutex);

            while (!m_qPendingChecks.empty() && vBatch.size() < 100)
            {
                vBatch.push_back(m_qPendingChecks.front());
                m_qPendingChecks.pop();
            }
        }

        if (!vBatch.empty())
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_tLastRequest).count();
            if (elapsed < m_iRequestDelay)
            {
                auto sleepTime = m_iRequestDelay - elapsed;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
            }

            FetchResult_t result = FetchBanData(vBatch);
            ProcessBanResponse(result.sResponse, result.vAccountIDs);

            m_tLastRequest = std::chrono::steady_clock::now();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void CBanChecker::SteamWorkerThread()
{
    while (m_bRunning)
    {
        std::vector<uint32_t> vBatch;

        {
            std::lock_guard<std::mutex> lock(m_tMutex);

            while (!m_qPendingSteamChecks.empty() && vBatch.size() < 100)
            {
                vBatch.push_back(m_qPendingSteamChecks.front());
                m_qPendingSteamChecks.pop();
            }
        }

        if (!vBatch.empty())
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_tLastSteamRequest).count();
            if (elapsed < m_iSteamRequestDelay)
            {
                auto sleepTime = m_iSteamRequestDelay - elapsed;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
            }

            FetchResult_t result = FetchSteamBanData(vBatch);
            ProcessSteamBanResponse(result.sResponse, result.vAccountIDs);

            m_tLastSteamRequest = std::chrono::steady_clock::now();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

CBanChecker::FetchResult_t CBanChecker::FetchBanData(const std::vector<uint32_t>& vAccountIDs)
{
    FetchResult_t result;
    result.vAccountIDs = vAccountIDs;

    if (m_sAPIKey.empty())
        return result;

    std::string sSteamIDs;
    for (size_t i = 0; i < vAccountIDs.size(); i++)
    {
        uint64_t steamID64 = 76561197960265728ULL + vAccountIDs[i];
        sSteamIDs += std::to_string(steamID64);
        if (i < vAccountIDs.size() - 1)
            sSteamIDs += ",";
    }

    std::string sPath = "/api/sourcebans?key=" + m_sAPIKey + "&steamids=" + sSteamIDs + "&shouldkey=1";
    std::wstring wsPath(sPath.begin(), sPath.end());
    std::string sResponse;

    HINTERNET hSession = WinHttpOpen(L"pissgram/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession)
        return result;

    HINTERNET hConnect = WinHttpConnect(hSession, L"steamhistory.net",
        INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wsPath.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    BOOL bResults = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (bResults)
        bResults = WinHttpReceiveResponse(hRequest, NULL);

    if (bResults)
    {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        LPSTR pszOutBuffer;

        do
        {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
                break;

            if (dwSize == 0)
                break;

            pszOutBuffer = new char[dwSize + 1];
            if (!pszOutBuffer)
                break;

            ZeroMemory(pszOutBuffer, dwSize + 1);

            if (!WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded))
            {
                delete[] pszOutBuffer;
                break;
            }

            sResponse.append(pszOutBuffer, dwDownloaded);
            delete[] pszOutBuffer;

        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    result.sResponse = sResponse;
    return result;
}

CBanChecker::FetchResult_t CBanChecker::FetchSteamBanData(const std::vector<uint32_t>& vAccountIDs)
{
    FetchResult_t result;
    result.vAccountIDs = vAccountIDs;

    if (m_sSteamAPIKey.empty())
        return result;

    std::string sSteamIDs;
    for (size_t i = 0; i < vAccountIDs.size(); i++)
    {
        uint64_t steamID64 = 76561197960265728ULL + vAccountIDs[i];
        sSteamIDs += std::to_string(steamID64);
        if (i < vAccountIDs.size() - 1)
            sSteamIDs += ",";
    }

    std::string sPath = "/ISteamUser/GetPlayerBans/v1/?key=" + m_sSteamAPIKey + "&steamids=" + sSteamIDs;
    std::wstring wsPath(sPath.begin(), sPath.end());
    std::string sResponse;

    HINTERNET hSession = WinHttpOpen(L"pissgram/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession)
        return result;

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.steampowered.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wsPath.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    BOOL bResults = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (bResults)
        bResults = WinHttpReceiveResponse(hRequest, NULL);

    if (bResults)
    {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        LPSTR pszOutBuffer;

        do
        {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
                break;

            if (dwSize == 0)
                break;

            pszOutBuffer = new char[dwSize + 1];
            if (!pszOutBuffer)
                break;

            ZeroMemory(pszOutBuffer, dwSize + 1);

            if (!WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded))
            {
                delete[] pszOutBuffer;
                break;
            }

            sResponse.append(pszOutBuffer, dwDownloaded);
            delete[] pszOutBuffer;

        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    result.sResponse = sResponse;
    return result;
}

void CBanChecker::ProcessBanResponse(const std::string& sResponse, const std::vector<uint32_t>& vAccountIDs)
{
    if (sResponse.empty())
    {
        std::lock_guard<std::mutex> lock(m_tMutex);
        for (auto accountID : vAccountIDs)
        {
            m_sProcessing.erase(accountID);

            BanInfo_t& banInfo = m_mBanCache[accountID];
            banInfo.m_bFetched = true;
            banInfo.m_iSourceBanCount = 0;
        }

        return;
    }

    try
    {
        boost::property_tree::ptree root;
        std::stringstream ss(sResponse);
        boost::property_tree::read_json(ss, root);

        std::lock_guard<std::mutex> lock(m_tMutex);

        for (auto& [sSteamID64, tData] : root.get_child("response"))
        {
            uint64_t steamID64 = std::stoull(sSteamID64);
            uint32_t accountID = static_cast<uint32_t>(steamID64 - 76561197960265728ULL);

            BanInfo_t& banInfo = m_mBanCache[accountID];
            banInfo.m_bFetched = true;
            banInfo.m_iSourceBanCount = 0;
            banInfo.m_vBanDetails.clear();

            for (auto& item : tData)
            {
                try
                {
                    auto& tBan = item.second;
                    std::string sState = tBan.get<std::string>("CurrentState", "");

                    if (sState == "Permanent" || sState == "Temp-Ban")
                    {
                        banInfo.m_iSourceBanCount++;

                        std::string sServer = tBan.get<std::string>("Server", "Unknown");
                        std::string sReason = tBan.get<std::string>("BanReason", "No reason");
                        banInfo.m_vBanDetails.push_back(sServer + ": " + sReason);
                    }
                }
                catch (const std::exception& e)
                {
                    continue;
                }
            }

            m_sProcessing.erase(accountID);
        }

        for (auto accountID : vAccountIDs)
        {
            m_sProcessing.erase(accountID);

            if (!m_mBanCache.contains(accountID))
            {
                BanInfo_t& banInfo = m_mBanCache[accountID];
                banInfo.m_bFetched = true;
                banInfo.m_iSourceBanCount = 0;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::lock_guard<std::mutex> lock(m_tMutex);
        for (auto accountID : vAccountIDs)
            m_sProcessing.erase(accountID);
    }
}

void CBanChecker::ProcessSteamBanResponse(const std::string& sResponse, const std::vector<uint32_t>& vAccountIDs)
{
    if (sResponse.empty())
    {
        std::lock_guard<std::mutex> lock(m_tMutex);
        for (auto accountID : vAccountIDs)
        {
            m_sSteamProcessing.erase(accountID);

            BanInfo_t& banInfo = m_mBanCache[accountID];
            banInfo.m_bSteamAPIFetched = true;
            banInfo.m_bVACBanned = false;
            banInfo.m_iVACBanCount = 0;
            banInfo.m_bGameBanned = false;
            banInfo.m_iGameBanCount = 0;
            banInfo.m_bCommunityBanned = false;
        }
        return;
    }

    try
    {
        boost::property_tree::ptree root;
        std::stringstream ss(sResponse);
        boost::property_tree::read_json(ss, root);

        std::lock_guard<std::mutex> lock(m_tMutex);

        for (auto& item : root.get_child("players"))
        {
            try
            {
                auto& tPlayer = item.second;

                std::string sSteamID64 = tPlayer.get<std::string>("SteamId", "");
                if (sSteamID64.empty())
                    continue;

                uint64_t steamID64 = std::stoull(sSteamID64);
                uint32_t accountID = static_cast<uint32_t>(steamID64 - 76561197960265728ULL);

                BanInfo_t& banInfo = m_mBanCache[accountID];
                banInfo.m_bSteamAPIFetched = true;

                banInfo.m_bVACBanned = tPlayer.get<bool>("VACBanned", false);
                banInfo.m_iVACBanCount = tPlayer.get<int>("NumberOfVACBans", 0);
                banInfo.m_bGameBanned = tPlayer.get<int>("NumberOfGameBans", 0) > 0;
                banInfo.m_iGameBanCount = tPlayer.get<int>("NumberOfGameBans", 0);
                banInfo.m_bCommunityBanned = tPlayer.get<bool>("CommunityBanned", false);
                banInfo.m_iDaysSinceLastBan = tPlayer.get<int>("DaysSinceLastBan", 0);

                m_sSteamProcessing.erase(accountID);
            }
            catch (const std::exception& e)
            {
                continue;
            }
        }

        for (auto accountID : vAccountIDs)
        {
            m_sSteamProcessing.erase(accountID);

            if (!m_mBanCache.contains(accountID) || !m_mBanCache[accountID].m_bSteamAPIFetched)
            {
                BanInfo_t& banInfo = m_mBanCache[accountID];
                banInfo.m_bSteamAPIFetched = true;
                banInfo.m_bVACBanned = false;
                banInfo.m_iVACBanCount = 0;
                banInfo.m_bGameBanned = false;
                banInfo.m_iGameBanCount = 0;
                banInfo.m_bCommunityBanned = false;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::lock_guard<std::mutex> lock(m_tMutex);
        for (auto accountID : vAccountIDs)
            m_sSteamProcessing.erase(accountID);
    }
}

void CBanChecker::ClearCache()
{
    std::lock_guard<std::mutex> lock(m_tMutex);
    m_mBanCache.clear();
}

void CBanChecker::SaveCache()
{
    std::lock_guard<std::mutex> lock(m_tMutex);

    try
    {
        boost::property_tree::ptree root;

        for (auto& [accountID, banInfo] : m_mBanCache)
        {
            if (!banInfo.m_bFetched && !banInfo.m_bSteamAPIFetched)
                continue;

            boost::property_tree::ptree tEntry;
            tEntry.put("SourceBans", banInfo.m_iSourceBanCount);
            tEntry.put("VACBanned", banInfo.m_bVACBanned);
            tEntry.put("VACBanCount", banInfo.m_iVACBanCount);
            tEntry.put("GameBanned", banInfo.m_bGameBanned);
            tEntry.put("GameBanCount", banInfo.m_iGameBanCount);
            tEntry.put("CommunityBanned", banInfo.m_bCommunityBanned);
            tEntry.put("DaysSinceLastBan", banInfo.m_iDaysSinceLastBan);
            tEntry.put("Fetched", banInfo.m_bFetched);
            tEntry.put("SteamAPIFetched", banInfo.m_bSteamAPIFetched);

            boost::property_tree::ptree tDetails;
            for (auto& sDetail : banInfo.m_vBanDetails)
            {
                boost::property_tree::ptree tDetail;
                tDetail.put("", sDetail);
                tDetails.push_back(std::make_pair("", tDetail));
            }
            tEntry.add_child("Details", tDetails);

            root.add_child(std::to_string(accountID), tEntry);
        }

        //write_json(F::Configs.m_sCorePath + "Players.json", tWrite);

        std::ofstream file(F::Configs.m_sCorePath + "BanCache.json");
        if (file.is_open())
            boost::property_tree::write_json(file, root);
    }
    catch (const std::exception& e)
    {
    }
}

void CBanChecker::LoadCache()
{
    std::lock_guard<std::mutex> lock(m_tMutex);

    try
    {
        boost::property_tree::ptree root;
        std::ifstream file(F::Configs.m_sCorePath + "BanCache.json");

        if (!file.is_open())
            return;

        boost::property_tree::read_json(file, root);

        for (auto& [sAccountID, tData] : root)
        {
            uint32_t accountID = std::stoul(sAccountID);
            BanInfo_t& banInfo = m_mBanCache[accountID];

            banInfo.m_iSourceBanCount = tData.get<int>("SourceBans", 0);
            banInfo.m_bVACBanned = tData.get<bool>("VACBanned", false);
            banInfo.m_iVACBanCount = tData.get<int>("VACBanCount", 0);
            banInfo.m_bGameBanned = tData.get<bool>("GameBanned", false);
            banInfo.m_iGameBanCount = tData.get<int>("GameBanCount", 0);
            banInfo.m_bCommunityBanned = tData.get<bool>("CommunityBanned", false);
            banInfo.m_iDaysSinceLastBan = tData.get<int>("DaysSinceLastBan", 0);
            banInfo.m_bFetched = tData.get<bool>("Fetched", true);
            banInfo.m_bSteamAPIFetched = tData.get<bool>("SteamAPIFetched", false);

            if (tData.count("Details"))
            {
                for (auto& [_, tDetail] : tData.get_child("Details"))
                    banInfo.m_vBanDetails.push_back(tDetail.get_value<std::string>());
            }
        }
    }
    catch (const std::exception& e)
    {
    }
}