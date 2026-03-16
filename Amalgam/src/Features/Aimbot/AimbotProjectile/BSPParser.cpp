#include "BSPParser.h"
#include <fstream>
#include <cmath>
#include "../../../SDK/SDK.h"

CBSPParser g_BSPParser;
#define BSP_LOG(msg) { std::ofstream log("BSP_debug.txt", std::ios::app); log << msg << "\n"; }

inline bool IsValidVector(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void CBSPParser::DebugRender(float flDuration)
{
    auto it = m_mMapEdges.find(m_sCurrentMap);
    if (it == m_mMapEdges.end() || it->second.empty())
        return;

    const float flBoxHalf = 4.0f;
    const float flLineLen = 16.0f;

    for (const auto& edge : it->second)
    {
        if (!IsValidVector(edge.m_vPos) || !IsValidVector(edge.m_vNormal))
            continue;

        Vec3 pos = edge.m_vPos;
        Vec3 n = edge.m_vNormal.Normalized();
        Vec3 t = n.Cross(Vec3(0.001f, 1.0f, 0.001f)).Normalized();
        if (t.IsZero())
            t = n.Cross(Vec3(1.0f, 0.001f, 0.001f)).Normalized();
        Vec3 s = n.Cross(t).Normalized();

        Vec3 a = pos + t * flLineLen * 0.5f;
        Vec3 b = pos - t * flLineLen * 0.5f;
        Vec3 c = pos + s * flLineLen * 0.5f;
        Vec3 d = pos - s * flLineLen * 0.5f;

        float score = std::clamp(edge.m_flScore / 3.0f, 0.0f, 1.0f);
        Color_t col((unsigned char)(255 * score), (unsigned char)(255 * (1.0f - score)), 32, 200);

        G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(a, b), I::GlobalVars->curtime + flDuration, col);
        G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(c, d), I::GlobalVars->curtime + flDuration, col);

        Vec3 mins(-flBoxHalf, -flBoxHalf, -flBoxHalf);
        Vec3 maxs(flBoxHalf, flBoxHalf, flBoxHalf);
        Vec3 angs(0, 0, 0);
        G::BoxStorage.emplace_back(pos, mins, maxs, angs, I::GlobalVars->curtime + flDuration, col.Alpha((unsigned char)(col.a * 0.7f)), Color_t(0, 0, 0, 0));
    }
}

bool CBSPParser::OnLevelInit(const char* szMapName)
{
    BSP_LOG("OnLevelInit called for map: %s", szMapName ? szMapName : "null");

    if (!szMapName)
    {
        BSP_LOG("Map name null, returning");
        return false;
    }

    m_sCurrentMap = szMapName;
    m_vCurrentMapSurfaces.clear();

    if (m_mMapEdges.find(m_sCurrentMap) != m_mMapEdges.end())
    {
        BSP_LOG("Map already parsed, skipping");
        return true;
    }

    bool bParsed = false;
    try
    {
        bParsed = ParseBSPFile(szMapName);
    }
    catch (...)
    {
        BSP_LOG("Exception caught during ParseBSPFile");
        bParsed = false;
    }

    if (!bParsed)
    {
        BSP_LOG("ParseBSPFile failed, skipping edge extraction");
        return false;
    }

    try
    {
        ExtractEdgesFromSurfaces();
    }
    catch (...)
    {
        BSP_LOG("Exception caught during ExtractEdgesFromSurfaces");
    }

    BSP_LOG("OnLevelInit finished for map: %s", szMapName);
    return true;
}


void CBSPParser::OnLevelShutdown()
{
    I::CVar->ConsolePrintf("CBSPParser::OnLevelShutdown called for map: %s\n", m_sCurrentMap.c_str());
    m_vCurrentMapSurfaces.clear();
}

bool CBSPParser::ParseBSPFile(const char* szMapName)
{
    BSP_LOG("ParseBSPFile started: %s", szMapName);

    // Build path to BSP file
    char szPath[512];
    const char* szGameDir = I::EngineClient->GetGameDirectory();
    sprintf_s(szPath, "%s/maps/%s.bsp", szGameDir, szMapName);

    BSP_LOG("Loading BSP file: %s", szPath);

    std::ifstream file(szPath, std::ios::binary);
    if (!file.is_open())
    {
        BSP_LOG("Failed to open BSP: %s", szPath);
        return false;
    }

    // Read header
    BSP_dheader_t header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.ident != IDBSPHEADER || header.version != BSPVERSION)
    {
        BSP_LOG("BSP header/version mismatch for %s", szPath);
        file.close();
        return false;
    }

    // Lump names
    const char* lumpNames[64] = {
        "Entities", "Planes", "Textures", "Vertices", "Visibility",
        "Nodes", "Texinfo", "Faces", "Lighting", "Leaves",
        "LeafFaces", "LeafBrushes", "Edges", "SurfEdges", "Models"
    }; 

    for (int i = 0; i < 64; i++)
    {
        const BSP_lump_t& lump = header.lumps[i];
        BSP_LOG("Lump %d (%s): offset=%d size=%zu", i, lumpNames[i], lump.fileofs, lump.filelen);
    }

    // Read vertices
    BSP_lump_t& vertLump = header.lumps[LUMP_VERTEXES];
    if (vertLump.filelen % (sizeof(float) * 3) != 0)
        BSP_LOG("Warning: Vertex lump size not divisible by 12 bytes: %zu", vertLump.filelen);

    int nVertexCount = vertLump.filelen / (sizeof(float) * 3);
    std::vector<Vec3> vVertices(nVertexCount);
    file.seekg(vertLump.fileofs);
    file.read(reinterpret_cast<char*>(vVertices.data()), vertLump.filelen);

    BSP_LOG("Read lump Vertexes: count=%d size=%zu", nVertexCount, vertLump.filelen);

    file.close();

    BSP_LOG("BSP file parsing finished: %s", szMapName);
    return !vVertices.empty();
}

void CBSPParser::ExtractEdgesFromSurfaces()
{
    BSP_LOG("ExtractEdgesFromSurfaces started");

    std::vector<BSPEdge_t> vEdges;
    vEdges.reserve(m_vCurrentMapSurfaces.size() * 4);

    std::unordered_map<int, std::vector<int>> mSpatialHash;
    const float flCellSize = 128.0f;

    for (size_t i = 0; i < m_vCurrentMapSurfaces.size(); i++)
    {
        const BSPSurface_t& surface = m_vCurrentMapSurfaces[i];
        int iCellX = int(surface.m_vCenter.x / flCellSize);
        int iCellY = int(surface.m_vCenter.y / flCellSize);
        int iCellZ = int(surface.m_vCenter.z / flCellSize);
        int iHash = (iCellX * 73856093) ^ (iCellY * 19349663) ^ (iCellZ * 83492791);
        mSpatialHash[iHash].push_back((int)i);
    }

    // Extract edges
    for (size_t i = 0; i < m_vCurrentMapSurfaces.size(); i++)
    {
        const BSPSurface_t& surface = m_vCurrentMapSurfaces[i];

        for (size_t j = 0; j < surface.m_vVertices.size(); j++)
        {
            Vec3 v1 = surface.m_vVertices[j];
            Vec3 v2 = surface.m_vVertices[(j + 1) % surface.m_vVertices.size()];
            Vec3 vEdgeCenter = (v1 + v2) * 0.5f;

            std::vector<BSPSurface_t> vNearbySurfaces;

            int iCellX = int(vEdgeCenter.x / flCellSize);
            int iCellY = int(vEdgeCenter.y / flCellSize);
            int iCellZ = int(vEdgeCenter.z / flCellSize);

            for (int dx = -1; dx <= 1; dx++)
            {
                for (int dy = -1; dy <= 1; dy++)
                {
                    for (int dz = -1; dz <= 1; dz++)
                    {
                        int iHash = ((iCellX + dx) * 73856093) ^ ((iCellY + dy) * 19349663) ^ ((iCellZ + dz) * 83492791);
                        auto it = mSpatialHash.find(iHash);
                        if (it != mSpatialHash.end())
                        {
                            for (int idx : it->second)
                            {
                                if ((size_t)idx != i && m_vCurrentMapSurfaces[idx].m_vCenter.DistTo(vEdgeCenter) < 32.0f)
                                    vNearbySurfaces.push_back(m_vCurrentMapSurfaces[idx]);
                            }
                        }
                    }
                }
            }

            // Check if edge is a corner
            bool bIsCorner = false;
            for (const auto& nearby : vNearbySurfaces)
            {
                float flDot = surface.m_vNormal.Dot(nearby.m_vNormal);
                if (flDot < 0.9f)
                {
                    bIsCorner = true;
                    break;
                }
            }

            if (bIsCorner)
            {
                BSPEdge_t edge;
                edge.m_vPos = vEdgeCenter;
                edge.m_vNormal = surface.m_vNormal;
                edge.m_bIsCorner = true;

                try
                {
                    edge.m_flScore = CalculateEdgeScore(vEdgeCenter, surface.m_vNormal, vNearbySurfaces);
                }
                catch (...)
                {
                    BSP_LOG("Exception in CalculateEdgeScore for edge at (%f,%f,%f)", vEdgeCenter.x, vEdgeCenter.y, vEdgeCenter.z);
                    continue;
                }

                vEdges.push_back(edge);
            }
        }
    }

    m_mMapEdges[m_sCurrentMap] = std::move(vEdges);
    BSP_LOG("ExtractEdgesFromSurfaces finished: %zu edges stored", m_mMapEdges[m_sCurrentMap].size());
}


float CBSPParser::CalculateEdgeScore(const Vec3& vPos, const Vec3& vNormal, const std::vector<BSPSurface_t>& vNearbySurfaces)
{
    BSP_LOG("CalculateEdgeScore started for (%f,%f,%f)", vPos.x, vPos.y, vPos.z);

    float flScore = 1.0f;

    for (const auto& surface : vNearbySurfaces)
    {
        float flDot = fabsf(vNormal.Dot(surface.m_vNormal));
        if (flDot < 0.5f)
            flScore += 0.5f;
    }

    try
    {
        CGameTrace trace = {};
        CTraceFilterWorldAndPropsOnly filter = {};
        SDK::Trace(vPos, vPos - Vec3(0, 0, 128), MASK_SOLID, &filter, &trace);

        float flHeightOffGround = vPos.z - trace.endpos.z;
        if (flHeightOffGround > 32.0f && flHeightOffGround < 72.0f)
            flScore += 1.0f;
    }
    catch (...)
    {
        BSP_LOG("Exception in Trace calculation at (%f,%f,%f)", vPos.x, vPos.y, vPos.z);
    }

    BSP_LOG("CalculateEdgeScore finished: %f", flScore);
    return flScore;
}

std::vector<BSPEdge_t> CBSPParser::GetNearbyEdges(const Vec3& vPos, float flRadius)
{
    BSP_LOG("GetNearbyEdges called for (%f,%f,%f) radius %f", vPos.x, vPos.y, vPos.z, flRadius);
    std::vector<BSPEdge_t> vResult;

    auto it = m_mMapEdges.find(m_sCurrentMap);
    if (it == m_mMapEdges.end())
    {
        BSP_LOG("No edges for current map");
        return vResult;
    }

    const float flRadiusSq = flRadius * flRadius;

    for (const auto& edge : it->second)
    {
        if (edge.m_vPos.DistToSqr(vPos) < flRadiusSq)
            vResult.push_back(edge);
    }

    std::sort(vResult.begin(), vResult.end(), [](const BSPEdge_t& a, const BSPEdge_t& b) {
        return a.m_flScore > b.m_flScore;
        });

    BSP_LOG("GetNearbyEdges returning %zu edges", vResult.size());
    return vResult;
}

bool CBSPParser::IsEdgePoint(const Vec3& vPos, const Vec3& vNormal, float flThreshold)
{
    BSP_LOG("IsEdgePoint called for (%f,%f,%f)", vPos.x, vPos.y, vPos.z);
    auto vNearby = GetNearbyEdges(vPos, 24.0f);

    for (const auto& edge : vNearby)
    {
        if (vPos.DistTo(edge.m_vPos) < 16.0f)
        {
            float flDot = vNormal.Dot(edge.m_vNormal);
            if (flDot > flThreshold)
            {
                BSP_LOG("IsEdgePoint: true");
                return true;
            }
        }
    }

    BSP_LOG("IsEdgePoint: false");
    return false;
}