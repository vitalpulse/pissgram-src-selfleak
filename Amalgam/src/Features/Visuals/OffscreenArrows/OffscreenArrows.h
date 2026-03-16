#pragma once
#include "../../../SDK/SDK.h"
#include <unordered_map>
#include "../../Visuals/Groups/Groups.h"

struct ArrowCache_t
{
	Color_t m_tColor = {};
	int m_iOffset = 0;
	float m_flDistance = 0.f;
	float m_flMaxDistance = 0.f;
	float m_flMinSize = 0.f;
	float m_flFlickerDistance = 0.f;
	float m_flMaxSize = 0.f;
	float m_flLastValidDistance = 0.f;
	float m_flBaseAlpha = 1.f; // Base alpha before edge fading
	bool m_bWasDormant = false;
};

class COffscreenArrows
{
private:
	void DrawArcTo(const Vec3& vFromPos, const Vec3& vToPos, Color_t tColor, int iIndex, int iOffset, float flMaxDistance, float flMaxArcSize, float flMinArcSize);

	std::unordered_map<CBaseEntity*, ArrowCache_t> m_mCache;
public:
	void Store(CTFPlayer* pLocal);
	void Draw(CTFPlayer* pLocal);
};

ADD_FEATURE(COffscreenArrows, Arrows);