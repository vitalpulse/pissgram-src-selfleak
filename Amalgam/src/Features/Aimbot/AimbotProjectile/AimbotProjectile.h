#pragma once
#include "../../../SDK/SDK.h"

#include "../AimbotGlobal/AimbotGlobal.h"

Enum(PointType, None = 0, Regular = 1 << 0, Obscured = 1 << 1, ObscuredExtra = 1 << 2, ObscuredMulti = 1 << 3)
Enum(Calculated, Pending, Good, Time, Bad)

struct Solution_t
{
	float m_flPitch = 0.f;
	float m_flYaw = 0.f;
	float m_flTime = 0.f;
	int m_iCalculated = CalculatedEnum::Pending;
};

struct Point_t
{
	Vec3 m_vPoint = {};
	Solution_t m_tSolution = {};
};

struct GeometryInfo
{
	std::vector<Vec3> m_vWallNormals;
	std::vector<Vec3> m_vCorners;
	bool m_bNearWall = false;
	bool m_bInCorner = false;
	float m_flGroundDistance = 0.0f;
};

struct Info_t
{
	CTFPlayer* m_pLocal = nullptr;
	CTFWeaponBase* m_pWeapon = nullptr;

	Vec3 m_vLocalEye = {};
	Vec3 m_vTargetEye = {};

	float m_flLatency = 0.f;

	Vec3 m_vHull = {};
	Vec3 m_vOffset = {};
	Vec3 m_vAngFix = {};
	float m_flVelocity = 0.f;
	float m_flGravity = 0.f;
	float m_flRadius = 0.f;
	float m_flRadiusTime = 0.f;
	float m_flBoundingTime = 0.f;
	float m_flOffsetTime = 0.f;
	int m_iSplashCount = 0;
	int m_iSplashMode = 0;
	float m_flPrimeTime = 0;
	int m_iPrimeTime = 0;
};

struct SphereKey {
	float r;
	int samples;
	float rx, ry;
	bool operator==(SphereKey const& o) const {
		return r == o.r && samples == o.samples && rx == o.rx && ry == o.ry;
	}
};

struct GeometryCacheEntry {
	GeometryInfo tInfo;
	float flTimestamp;
	Vec3 vLastPosition;
};

struct SphereKeyHash {
	std::size_t operator()(SphereKey const& k) const noexcept {
		uint64_t a = std::hash<float>{}(k.r);
		uint64_t b = std::hash<int>{}(k.samples);
		uint64_t c = std::hash<float>{}(k.rx);
		uint64_t d = std::hash<float>{}(k.ry);
		return (a * 1315423911u) ^ (b << 13) ^ (c << 7) ^ d;
	}
};

struct SpatialHash
{
	static constexpr int GRID_SIZE = 32; // 32-unit grid cells
	std::unordered_map<uint64_t, std::vector<int>> m_mGrid;

	static inline uint64_t GetHash(const Vec3& vPos)
	{
		int x = static_cast<int>(vPos.x / GRID_SIZE);
		int y = static_cast<int>(vPos.y / GRID_SIZE);
		int z = static_cast<int>(vPos.z / GRID_SIZE);
		return (static_cast<uint64_t>(x) << 42) | (static_cast<uint64_t>(y) << 21) | static_cast<uint64_t>(z);
	}

	void Insert(const Vec3& vPos, int iIndex)
	{
		m_mGrid[GetHash(vPos)].push_back(iIndex);
	}

	void Clear() { m_mGrid.clear(); }
};

class CAimbotProjectile
{
private:
	std::vector<Target_t> GetTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon);
	std::vector<Target_t> SortTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon);
	std::unordered_map<int, Vec3> GetDirectPoints(Target_t& tTarget, CBaseEntity* pProjectile = nullptr);
	std::vector<Point_t> GetSplashPoints(Target_t& tTarget, std::vector<std::pair<Vec3, int>>& vSpherePoints, int iSimTime);

	void SetupSplashPoints(Target_t& tTarget, std::vector<std::pair<Vec3, int>>& vSpherePoints,
		std::vector<std::pair<Vec3, Vec3>>& vSimplePoints,
		std::vector<Vec3>& vEdgePoints);

	void SetupSplashPointsPoints(Target_t& tTarget, std::vector<std::pair<Vec3, int>>& vSpherePoints,
		std::vector<std::pair<Vec3, Vec3>>& vSimplePoints);

	std::vector<Point_t> GetSplashPointsSimple(Target_t& tTarget,
		std::vector<std::pair<Vec3, Vec3>>& vSpherePoints,
		std::vector<Vec3>& vEdgePoints,
		int iSimTime);

	void CalculateAngle(const Vec3& vLocalPos, const Vec3& vTargetPos, int iSimTime, Solution_t& out, bool bAccuracy = true);
	bool TestAngle(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, Target_t& tTarget, Vec3& vPoint, Vec3& vAngles, int iSimTime, bool bSplash, bool* pHitSolid = nullptr, std::vector<Vec3>* pProjectilePath = nullptr);
	bool TestAnglePoints(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, Target_t& tTarget, Vec3& vPoint, Vec3& vAngles, int iSimTime, bool bSplash, bool* pHitSolid = nullptr, std::vector<Vec3>* pProjectilePath = nullptr);


	int CanHit(Target_t& tTarget, CTFPlayer* pLocal, CTFWeaponBase* pWeapon);
	bool RunMain(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);

	bool CanHit(Target_t& tTarget, CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pProjectile);
	bool TestAngle(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pProjectile, Target_t& tTarget, Vec3& vPoint, Vec3& vAngles, int iSimTime, bool bSplash, std::vector<Vec3>* pProjectilePath = nullptr);

	bool Aim(Vec3 vCurAngle, Vec3 vToAngle, Vec3& vOut, int iMethod = Vars::Aimbot::General::AimType.Value);
	void Aim(CUserCmd* pCmd, Vec3& vAngle, int iMethod = Vars::Aimbot::General::AimType.Value);

	Info_t m_tInfo = {};

	bool m_bLastTickHeld = false;

	float m_flTimeTo = std::numeric_limits<float>::max();
	std::vector<Vec3> m_vPlayerPath = {};
	std::vector<Vec3> m_vProjectilePath = {};
	std::vector<DrawBox_t> m_vBoxes = {};

public:
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
	float GetSplashRadius(CTFWeaponBase* pWeapon, CTFPlayer* pPlayer);
	
	bool AutoAirblast(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, CBaseEntity* pProjectile);
	float GetSplashRadius(CBaseEntity* pProjectile, CTFWeaponBase* pWeapon = nullptr, CTFPlayer* pPlayer = nullptr, float flScale = 1.f, CTFWeaponBase* pAirblast = nullptr);
	static std::unordered_map<int, GeometryCacheEntry> s_GeometryCache; // Cache per entity

	int m_iLastTickCancel = 0;
};

ADD_FEATURE(CAimbotProjectile, AimbotProjectile);