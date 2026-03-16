#pragma once
#include "../../../SDK/SDK.h"
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>

#define IDBSPHEADER (('P'<<24)+('S'<<16)+('B'<<8)+'V')
#define BSPVERSION 20

#define LUMP_ENTITIES 0
#define LUMP_PLANES 1
#define LUMP_TEXDATA 2
#define LUMP_VERTEXES 3
#define LUMP_VISIBILITY 4
#define LUMP_NODES 5
#define LUMP_TEXINFO 6
#define LUMP_FACES 7
#define LUMP_LIGHTING 8
#define LUMP_OCCLUSION 9
#define LUMP_LEAFS 10
#define LUMP_EDGES 12
#define LUMP_SURFEDGES 13

#define SURF_LIGHT 0x0001
#define SURF_SKY2D 0x0002
#define SURF_SKY 0x0004
#define SURF_WARP 0x0008
#define SURF_TRANS 0x0010
#define SURF_NOPORTAL 0x0020
#define SURF_TRIGGER 0x0040
#define SURF_NODRAW 0x0080

using byte = unsigned char;

struct BSP_lump_t
{
	int fileofs;
	int filelen;
	int version;
	char fourCC[4];
};

struct BSP_dheader_t
{
	int ident;
	int version;
	BSP_lump_t lumps[64];
	int mapRevision;
};

struct BSP_dface_t
{
	unsigned short planenum;
	byte side;
	byte onNode;
	int firstedge;
	short numedges;
	short texinfo;
	short dispinfo;
	short surfaceFogVolumeID;
	byte styles[4];
	int lightofs;
	float area;
	int m_LightmapTextureMinsInLuxels[2];
	int m_LightmapTextureSizeInLuxels[2];
	int origFace;
	unsigned short numPrims;
	unsigned short firstPrimID;
	unsigned int smoothingGroups;
};

struct BSP_dedge_t
{
	unsigned short v[2];
};

struct BSP_dplane_t
{
	float normal[3];
	float dist;
	int type;
};

struct BSP_texinfo_t
{
	float textureVecs[2][4];
	float lightmapVecs[2][4];
	int flags;
	int texdata;
};

struct BSPEdge_t
{
	Vec3 m_vPos;
	Vec3 m_vNormal;
	float m_flScore;
	bool m_bIsCorner;
};

struct BSPSurface_t
{
	Vec3 m_vCenter;
	Vec3 m_vNormal;
	std::vector<Vec3> m_vVertices;
	int m_iFlags;
};

class CBSPParser
{
private:
	std::vector<BSPSurface_t> m_vCurrentMapSurfaces;
	std::string m_sCurrentMap;

	bool ParseBSPFile(const char* szMapName);
	void ExtractEdgesFromSurfaces();
	float CalculateEdgeScore(const Vec3& vPos, const Vec3& vNormal, const std::vector<BSPSurface_t>& vNearbySurfaces);

public:
	bool OnLevelInit(const char* szMapName);
	void OnLevelShutdown();
	void DebugRender(float flDuration = 0.05f); // test
	std::vector<BSPEdge_t> GetNearbyEdges(const Vec3& vPos, float flRadius);
	bool IsEdgePoint(const Vec3& vPos, const Vec3& vNormal, float flThreshold = 0.7f);
	std::unordered_map<std::string, std::vector<BSPEdge_t>> m_mMapEdges;
};

extern CBSPParser g_BSPParser;
