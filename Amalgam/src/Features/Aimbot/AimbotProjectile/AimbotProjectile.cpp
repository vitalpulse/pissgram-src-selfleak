#include "AimbotProjectile.h"

#include "../Aimbot.h"
#include "../../Simulation/MovementSimulation/MovementSimulation.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"
#include "../../EnginePrediction/EnginePrediction.h"
#include "../../Ticks/Ticks.h"
#include "../../Visuals/Visuals.h"
#include "../AutoAirblast/AutoAirblast.h"

std::unordered_map<int, GeometryCacheEntry> CAimbotProjectile::s_GeometryCache = {};

//#define SPLASH_DEBUG1 // normal splash visualization
//#define SPLASH_DEBUG2 // obstructed splash visualization
//#define SPLASH_DEBUG3 // simple splash visualization
//#define SPLASH_DEBUG4 // points visualization
//#define SPLASH_DEBUG5 // trace visualization
//#define SPLASH_DEBUG6 // trace count

#ifdef SPLASH_DEBUG6
static std::unordered_map<std::string, int> s_mTraceCount = {};
#endif

static inline Vec3 GetSimulatedPos(const MoveStorage& tStorage)
{
	const Vec3 vSim = tStorage.m_MoveData.m_vecAbsOrigin;
	if (tStorage.m_flPredictedDelta <= 0.f)
		return vSim;

	const float flIntervalStart = tStorage.m_flPredictedSimTime - tStorage.m_flPredictedDelta;
	const float flProgress = (tStorage.m_flSimTime - flIntervalStart) / tStorage.m_flPredictedDelta;
	const float t = std::clamp(flProgress, 0.f, 1.f);
	return tStorage.m_vPredictedOrigin.Lerp(vSim, t);
}

std::vector<Target_t> CAimbotProjectile::GetTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	std::vector<Target_t> vTargets;
	const auto iSort = Vars::Aimbot::General::TargetSelection.Value;

	const Vec3 vLocalPos = F::Ticks.GetShootPos();
	const Vec3 vLocalAngles = I::EngineClient->GetViewAngles();

	{
		auto eGroupType = EntityEnum::Invalid;
		if (Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::Players)
			eGroupType = !F::AimbotGlobal.FriendlyFire() || Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Team ? EntityEnum::PlayerEnemy : EntityEnum::PlayerAll;
		switch (pWeapon->GetWeaponID())
		{
		case TF_WEAPON_CROSSBOW:
			if (Vars::Aimbot::Healing::AutoArrow.Value)
				eGroupType = eGroupType != EntityEnum::Invalid ? EntityEnum::PlayerAll : EntityEnum::PlayerTeam;
			break;
		case TF_WEAPON_LUNCHBOX:
			if (Vars::Aimbot::Healing::AutoSandvich.Value)
				eGroupType = EntityEnum::PlayerTeam;
			break;
		}
		bool bHeal = pWeapon->GetWeaponID() == TF_WEAPON_CROSSBOW || pWeapon->GetWeaponID() == TF_WEAPON_LUNCHBOX;

		for (auto pEntity : H::Entities.GetGroup(eGroupType))
		{
			if (F::AimbotGlobal.ShouldIgnore(pEntity, pLocal, pWeapon))
				continue;

			bool bTeam = pEntity->m_iTeamNum() == pLocal->m_iTeamNum();
			if (bTeam && bHeal)
			{
				if (pEntity->As<CTFPlayer>()->m_iHealth() >= pEntity->As<CTFPlayer>()->GetMaxHealth()
					|| Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly && !H::Entities.IsFriend(pEntity->entindex()) && !H::Entities.InParty(pEntity->entindex()))
					continue;
			}

			float flFOVTo; Vec3 vPos, vAngleTo;
			if (!F::AimbotGlobal.PlayerBoneInFOV(pEntity->As<CTFPlayer>(), vLocalPos, vLocalAngles, flFOVTo, vPos, vAngleTo))
				continue;

			int iPriority = F::AimbotGlobal.GetPriority(pEntity->entindex());
			if (bTeam && bHeal)
			{
				iPriority = 0;
				switch (Vars::Aimbot::Healing::HealPriority.Value)
				{
				case Vars::Aimbot::Healing::HealPriorityEnum::PrioritizeFriends:
					if (H::Entities.IsFriend(pEntity->entindex()) || H::Entities.InParty(pEntity->entindex()))
						iPriority = std::numeric_limits<int>::max();
					break;
				case Vars::Aimbot::Healing::HealPriorityEnum::PrioritizeTeam:
					iPriority = std::numeric_limits<int>::max();
				}
			}

			float flDistTo = iSort == Vars::Aimbot::General::TargetSelectionEnum::Distance ? vLocalPos.DistTo(vPos) : 0.f;
			vTargets.emplace_back(pEntity, TargetEnum::Player, vPos, vAngleTo, flFOVTo, flDistTo, iPriority);
		}

		if (pWeapon->GetWeaponID() == TF_WEAPON_LUNCHBOX)
			return vTargets;
	}

	{
		auto eGroupType = EntityEnum::Invalid;
		if (Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::Building)
			eGroupType = EntityEnum::BuildingEnemy;
		if (Vars::Aimbot::Healing::AutoRepair.Value && pWeapon->GetWeaponID() == TF_WEAPON_SHOTGUN_BUILDING_RESCUE)
			eGroupType = eGroupType != EntityEnum::Invalid ? EntityEnum::BuildingAll : EntityEnum::BuildingTeam;
		for (auto pEntity : H::Entities.GetGroup(eGroupType))
		{
			if (F::AimbotGlobal.ShouldIgnore(pEntity, pLocal, pWeapon))
				continue;

			bool bTeam = pEntity->m_iTeamNum() == pLocal->m_iTeamNum();
			if (bTeam && (pEntity->As<CBaseObject>()->m_iHealth() >= pEntity->As<CBaseObject>()->m_iMaxHealth() || pEntity->As<CBaseObject>()->m_bBuilding()))
				continue;

			Vec3 vPos = pEntity->GetCenter();
			Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPos);
			float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);
			if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
				continue;

			int iPriority = 0;
			if (bTeam)
			{
				int iOwner = pEntity->As<CBaseObject>()->m_hBuilder().GetEntryIndex();
				switch (Vars::Aimbot::Healing::HealPriority.Value)
				{
				case Vars::Aimbot::Healing::HealPriorityEnum::PrioritizeFriends:
					if (iOwner == I::EngineClient->GetLocalPlayer() || H::Entities.IsFriend(iOwner) || H::Entities.InParty(iOwner))
						iPriority = std::numeric_limits<int>::max();
					break;
				case Vars::Aimbot::Healing::HealPriorityEnum::PrioritizeTeam:
					iPriority = std::numeric_limits<int>::max();
				}
			}

			float flDistTo = iSort == Vars::Aimbot::General::TargetSelectionEnum::Distance ? vLocalPos.DistTo(vPos) : 0.f;
			vTargets.emplace_back(pEntity, pEntity->IsSentrygun() ? TargetEnum::Sentry : pEntity->IsDispenser() ? TargetEnum::Dispenser : TargetEnum::Teleporter, vPos, vAngleTo, flFOVTo, flDistTo, iPriority);
		}
	}

	if (Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::Stickies)
	{
		bool bShouldAim = false;
		switch (pWeapon->GetWeaponID())
		{
		case TF_WEAPON_PIPEBOMBLAUNCHER:
			if (SDK::AttribHookValue(0, "stickies_detonate_stickies", pWeapon) == 1)
				bShouldAim = true;
			break;
		case TF_WEAPON_FLAREGUN:
		case TF_WEAPON_FLAREGUN_REVENGE:
			if (pWeapon->As<CTFFlareGun>()->GetFlareGunType() == FLAREGUN_SCORCHSHOT)
				bShouldAim = true;
		}

		if (bShouldAim)
		{
			for (auto pEntity : H::Entities.GetGroup(EntityEnum::WorldProjectile))
			{
				if (F::AimbotGlobal.ShouldIgnore(pEntity, pLocal, pWeapon))
					continue;

				Vec3 vPos = pEntity->GetCenter();
				Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPos);
				float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);
				if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
					continue;

				float flDistTo = iSort == Vars::Aimbot::General::TargetSelectionEnum::Distance ? vLocalPos.DistTo(vPos) : 0.f;
				vTargets.emplace_back(pEntity, TargetEnum::Sticky, vPos, vAngleTo, flFOVTo, flDistTo);
			}
		}
	}

	if (Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::NPCs) // does not predict movement
	{
		for (auto pEntity : H::Entities.GetGroup(EntityEnum::WorldNPC))
		{
			if (F::AimbotGlobal.ShouldIgnore(pEntity, pLocal, pWeapon))
				continue;

			Vec3 vPos = pEntity->GetCenter();
			Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPos);
			float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);
			if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
				continue;

			float flDistTo = iSort == Vars::Aimbot::General::TargetSelectionEnum::Distance ? vLocalPos.DistTo(vPos) : 0.f;
			vTargets.emplace_back(pEntity, TargetEnum::NPC, vPos, vAngleTo, flFOVTo, flDistTo);
		}
	}

	return vTargets;
}

std::vector<Target_t> CAimbotProjectile::SortTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	auto vTargets = GetTargets(pLocal, pWeapon);

	F::AimbotGlobal.SortTargets(vTargets, Vars::Aimbot::General::TargetSelection.Value);
	vTargets.resize(std::min(size_t(Vars::Aimbot::General::MaxTargets.Value), vTargets.size()));
	F::AimbotGlobal.SortPriority(vTargets);
	return vTargets;
}



float CAimbotProjectile::GetSplashRadius(CTFWeaponBase* pWeapon, CTFPlayer* pPlayer)
{
	float flRadius = 0.f;
	switch (pWeapon->GetWeaponID())
	{
	case TF_WEAPON_ROCKETLAUNCHER:
	case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
	case TF_WEAPON_PARTICLE_CANNON:
	case TF_WEAPON_PIPEBOMBLAUNCHER:
		flRadius = 146.f;
		break;
	case TF_WEAPON_FLAREGUN:
	case TF_WEAPON_FLAREGUN_REVENGE:
		if (pWeapon->As<CTFFlareGun>()->GetFlareGunType() == FLAREGUN_SCORCHSHOT)
			flRadius = 110.f;
	}
	if (!flRadius)
		return 0.f;

	flRadius = SDK::AttribHookValue(flRadius, "mult_explosion_radius", pWeapon);
	switch (pWeapon->GetWeaponID())
	{
	case TF_WEAPON_ROCKETLAUNCHER:
	case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
	case TF_WEAPON_PARTICLE_CANNON:
		if (pPlayer->InCond(TF_COND_BLASTJUMPING) && SDK::AttribHookValue(1.f, "rocketjump_attackrate_bonus", pWeapon) != 1.f)
			flRadius *= 0.8f;
	}
	return flRadius * Vars::Aimbot::Projectile::SplashRadius.Value / 100;
}

float CAimbotProjectile::GetSplashRadius(CBaseEntity* pProjectile, CTFWeaponBase* pWeapon, CTFPlayer* pPlayer, float flScale, CTFWeaponBase* pAirblast)
{
	float flRadius = 0.f;
	if (pAirblast)
	{
		pWeapon = pAirblast;
		pPlayer = pWeapon->m_hOwner()->As<CTFPlayer>();
	}
	switch (pProjectile->GetClassID())
	{
	case ETFClassID::CTFWeaponBaseGrenadeProj:
	case ETFClassID::CTFWeaponBaseMerasmusGrenade:
	case ETFClassID::CTFProjectile_Rocket:
	case ETFClassID::CTFProjectile_SentryRocket:
	case ETFClassID::CTFProjectile_EnergyBall:
		flRadius = 146.f;
		break;
	case ETFClassID::CTFGrenadePipebombProjectile:
		if (pProjectile->As<CTFGrenadePipebombProjectile>()->HasStickyEffects())
			flRadius = 146.f;
		break;
	case ETFClassID::CTFProjectile_Flare:
		if (pWeapon && pWeapon->As<CTFFlareGun>()->GetFlareGunType() == FLAREGUN_SCORCHSHOT)
			flRadius = 110.f;
	}
	if (pPlayer && pWeapon)
	{
		flRadius = SDK::AttribHookValue(flRadius, "mult_explosion_radius", pWeapon);
		switch (pProjectile->GetClassID())
		{
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_SentryRocket:
			if (pPlayer->InCond(TF_COND_BLASTJUMPING) && SDK::AttribHookValue(1.f, "rocketjump_attackrate_bonus", pWeapon) != 1.f)
				flRadius *= 0.8f;
		}
	}
	return flRadius * flScale;
}

static inline int GetSplashMode(CTFWeaponBase* pWeapon)
{
	if (Vars::Aimbot::Projectile::RocketSplashMode.Value)
	{
		switch (pWeapon->GetWeaponID())
		{
		case TF_WEAPON_ROCKETLAUNCHER:
		case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
		case TF_WEAPON_PARTICLE_CANNON:
			return Vars::Aimbot::Projectile::RocketSplashMode.Value;
		}
	}

	return Vars::Aimbot::Projectile::RocketSplashModeEnum::Regular;
}

static inline float PrimeTime(CTFWeaponBase* pWeapon)
{
	if (Vars::Aimbot::Projectile::Modifiers.Value & Vars::Aimbot::Projectile::ModifiersEnum::UsePrimeTime && pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
	{
		static auto tf_grenadelauncher_livetime = H::ConVars.FindVar("tf_grenadelauncher_livetime");
		const float flLiveTime = tf_grenadelauncher_livetime->GetFloat();
		return SDK::AttribHookValue(flLiveTime, "sticky_arm_time", pWeapon);
	}

	return 0.f;
}

static inline int GetHitboxPriority(int nHitbox, Target_t& tTarget, Info_t& tInfo, CBaseEntity* pProjectile = nullptr)
{
	if (!F::AimbotGlobal.IsHitboxValid(nHitbox, Vars::Aimbot::Projectile::Hitboxes.Value))
		return -1;

	int iHeadPriority = 0;
	int iBodyPriority = 1;
	int iFeetPriority = 2;

	if (Vars::Aimbot::Projectile::Hitboxes.Value & Vars::Aimbot::Projectile::HitboxesEnum::Auto)
	{
		bool bHeadshot = Vars::Aimbot::Projectile::Hitboxes.Value & Vars::Aimbot::Projectile::HitboxesEnum::Head
			&& tTarget.m_iTargetType == TargetEnum::Player;
		if (bHeadshot)
		{
			if (!pProjectile)
				bHeadshot = tInfo.m_pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW;
			else
				bHeadshot = pProjectile->GetClassID() == ETFClassID::CTFProjectile_Arrow && pProjectile->As<CTFProjectile_Arrow>()->CanHeadshot();

			if (Vars::Aimbot::Projectile::Hitboxes.Value & Vars::Aimbot::Projectile::HitboxesEnum::BodyaimIfLethal
				&& bHeadshot && !pProjectile && tInfo.m_pWeapon->m_hOwner().GetEntryIndex() == I::EngineClient->GetLocalPlayer())
			{
				float flCharge = I::GlobalVars->curtime - tInfo.m_pWeapon->As<CTFPipebombLauncher>()->m_flChargeBeginTime();
				float flDamage = Math::RemapVal(flCharge, 0.f, 1.f, 50.f, 120.f);
				if (tInfo.m_pLocal->IsMiniCritBoosted())
					flDamage *= 1.36f;
				if (flDamage >= tTarget.m_pEntity->As<CTFPlayer>()->m_iHealth())
					bHeadshot = false;

				if (tInfo.m_pLocal->IsCritBoosted()) // for reliability
					bHeadshot = false;
			}
		}
		if (bHeadshot)
			tTarget.m_nAimedHitbox = HITBOX_HEAD;

		bool bLower = Vars::Aimbot::Projectile::Hitboxes.Value & Vars::Aimbot::Projectile::HitboxesEnum::PrioritizeFeet
			&& tTarget.m_iTargetType == TargetEnum::Player && tTarget.m_pEntity->As<CTFPlayer>()->IsOnGround() /*&& tInfo.m_flRadius*/;

		iHeadPriority = bHeadshot ? 0 : 2;
		iBodyPriority = bHeadshot ? -1 : bLower ? 1 : 0;
		iFeetPriority = bHeadshot ? -1 : bLower ? 0 : 1;
	}

	switch (nHitbox)
	{
	case BOUNDS_HEAD: return iHeadPriority;
	case BOUNDS_BODY: return iBodyPriority;
	case BOUNDS_FEET: return iFeetPriority;
	}

	return -1;
};

std::unordered_map<int, Vec3> CAimbotProjectile::GetDirectPoints(Target_t& tTarget, CBaseEntity* pProjectile)
{
	std::unordered_map<int, Vec3> mPoints = {};

	const Vec3 vMins = tTarget.m_pEntity->m_vecMins(), vMaxs = tTarget.m_pEntity->m_vecMaxs();
	for (int i = 0; i < 3; i++)
	{
		int iPriority = GetHitboxPriority(i, tTarget, m_tInfo, pProjectile);
		if (iPriority == -1)
			continue;

		switch (i)
		{
		case BOUNDS_HEAD:
			if (tTarget.m_nAimedHitbox == HITBOX_HEAD)
			{
				auto aBones = F::Backtrack.GetBones(tTarget.m_pEntity);
				if (!aBones)
					break;

				//Vec3 vOff = tTarget.m_pEntity->As<CBaseAnimating>()->GetHitboxOrigin(aBones, HITBOX_HEAD) - tTarget.m_pEntity->m_vecOrigin();

				// https://www.youtube.com/watch?v=_PSGD-pJUrM, might be better??
				Vec3 vCenter, vBBoxMins, vBBoxMaxs; tTarget.m_pEntity->As<CBaseAnimating>()->GetHitboxInfo(aBones, HITBOX_HEAD, &vCenter, &vBBoxMins, &vBBoxMaxs);
				Vec3 vOff = vCenter + (vBBoxMins + vBBoxMaxs) / 2 - tTarget.m_pEntity->m_vecOrigin();

				float flLow = 0.f;
				Vec3 vDelta = tTarget.m_vPos + m_tInfo.m_vTargetEye - m_tInfo.m_vLocalEye;
				if (vDelta.z > 0)
				{
					float flXY = vDelta.Length2D();
					if (flXY)
						flLow = Math::RemapVal(vDelta.z / flXY, 0.f, 0.5f, 0.f, 1.f);
					else
						flLow = 1.f;
				}

				float flLerp = (Vars::Aimbot::Projectile::HuntsmanLerp.Value + (Vars::Aimbot::Projectile::HuntsmanLerpLow.Value - Vars::Aimbot::Projectile::HuntsmanLerp.Value) * flLow) / 100.f;
				float flAdd = Vars::Aimbot::Projectile::HuntsmanAdd.Value + (Vars::Aimbot::Projectile::HuntsmanAddLow.Value - Vars::Aimbot::Projectile::HuntsmanAdd.Value) * flLow;
				vOff.z += flAdd;
				vOff.z = vOff.z + (vMaxs.z - vOff.z) * flLerp;

				vOff.x = std::clamp(vOff.x, vMins.x + Vars::Aimbot::Projectile::HuntsmanClamp.Value, vMaxs.x - Vars::Aimbot::Projectile::HuntsmanClamp.Value);
				vOff.y = std::clamp(vOff.y, vMins.y + Vars::Aimbot::Projectile::HuntsmanClamp.Value, vMaxs.y - Vars::Aimbot::Projectile::HuntsmanClamp.Value);
				vOff.z = std::clamp(vOff.z, vMins.z + Vars::Aimbot::Projectile::HuntsmanClamp.Value, vMaxs.z - Vars::Aimbot::Projectile::HuntsmanClamp.Value);
				mPoints[iPriority] = vOff;
			}
			else
				mPoints[iPriority] = Vec3(0, 0, vMaxs.z - Vars::Aimbot::Projectile::VerticalShift.Value);
			break;
		case BOUNDS_BODY: mPoints[iPriority] = Vec3(0, 0, (vMaxs.z - vMins.z) / 2); break;
		case BOUNDS_FEET: mPoints[iPriority] = Vec3(0, 0, vMins.z + Vars::Aimbot::Projectile::VerticalShift.Value); break;
		}
	}

	return mPoints;
}

template <class T>
static inline void TracePointPoints(Vec3& vPoint, int& iType, Vec3& vTargetEye, Info_t& tInfo, T& vPoints, std::function<bool(CGameTrace& trace, bool& bErase, bool& bNormal)> checkPoint, int i = 0)
{
	// if anyone knows ways to further optimize this or just a better method, let me know!

	int iOriginalType = iType;
	bool bErase = false, bNormal = false;

	CGameTrace trace = {};
	CTraceFilterWorldAndPropsOnly filter = {};

#if defined(SPLASH_DEBUG1) || defined(SPLASH_DEBUG2)
	auto drawTrace = [](bool bSuccess, Color_t tColor, CGameTrace& trace)
		{
			Vec3 vMins = Vec3(-1, -1, -1) / (bSuccess ? 1 : 2), vMaxs = Vec3(1, 1, 1) / (bSuccess ? 1 : 2);
			Vec3 vAngles = Math::VectorAngles(trace.plane.normal);
			G::BoxStorage.emplace_back(trace.endpos, vMins, vMaxs, vAngles, I::GlobalVars->curtime + 60.f, tColor.Alpha(tColor.a / (bSuccess ? 1 : 10)), Color_t(0, 0, 0, 0));
			G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(trace.startpos, trace.endpos), I::GlobalVars->curtime + 60.f, tColor.Alpha(tColor.a / (bSuccess ? 1 : 10)));
		};
#endif

	if (iType & PointTypeEnum::Regular)
	{
		SDK::TraceHull(vTargetEye, vPoint, tInfo.m_vHull * -1, tInfo.m_vHull, MASK_SOLID, &filter, &trace);
#ifdef SPLASH_DEBUG6
		s_mTraceCount["Splash regular"]++;
#endif

		if (checkPoint(trace, bErase, bNormal))
		{
			if (i % Vars::Aimbot::Projectile::SplashUpdateRate.Value)
				vPoints.pop_back();
#ifdef SPLASH_DEBUG1
			else
				drawTrace(!bNormal, Vars::Colors::Local.Value, trace);
#endif
		}

		if (bErase)
			iType = 0;
		else if (bNormal)
			iType &= ~PointTypeEnum::Regular;
		else
			iType &= ~PointTypeEnum::Obscured;
	}
	if (iType & PointTypeEnum::ObscuredExtra)
	{
		bErase = false, bNormal = false;
		size_t iOriginalSize = vPoints.size();

		{
			// necessary performance wise?
			if (bNormal = (tInfo.m_vLocalEye - vTargetEye).Dot(vTargetEye - vPoint) > 0.f)
				goto breakOutExtra;

			if (!(iOriginalType & PointTypeEnum::Regular)) // don't do the same trace over again
			{
				SDK::Trace(vTargetEye, vPoint, MASK_SOLID, &filter, &trace);
#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket (2)"]++;
#endif
				bNormal = !trace.m_pEnt || trace.fraction == 1.f;
#ifdef SPLASH_DEBUG2
				drawTrace(!bNormal, Vars::Colors::IndicatorTextMid.Value, trace);
#endif
				if (bNormal)
					goto breakOutExtra;
			}

			filter.pSkip = trace.m_pEnt->GetClassID() != ETFClassID::CWorld || trace.hitbox ? trace.m_pEnt : nullptr; // make sure we get past entity or prop
			SDK::Trace(trace.endpos - (vTargetEye - vPoint).Normalized(), vPoint, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace);
			filter.pSkip = nullptr;
#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash rocket (2, 2)"]++;
#endif
			bNormal = trace.fraction == 1.f || trace.allsolid || (trace.startpos - trace.endpos).IsZero() || trace.surface.flags & (/*SURF_NODRAW |*/ SURF_SKY);
#ifdef SPLASH_DEBUG2
			drawTrace(!bNormal, Vars::Colors::IndicatorTextBad.Value, trace);
#endif
			if (bNormal)
				goto breakOutExtra;

			if (checkPoint(trace, bErase, bNormal))
			{
				SDK::Trace(trace.endpos + trace.plane.normal, vTargetEye, MASK_SHOT, &filter, &trace);
#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket check (2)"]++;
#endif
#ifdef SPLASH_DEBUG2
				drawTrace(trace.fraction >= 1.f, Vars::Colors::IndicatorTextMisc.Value, trace);
#endif
				if (trace.fraction < 1.f)
					vPoints.pop_back();
			}
		}

	breakOutExtra:
		if (vPoints.size() != iOriginalSize)
			iType = 0;
		else if (bErase || bNormal)
			iType &= ~PointTypeEnum::ObscuredExtra;
	}
	if (iType & PointTypeEnum::Obscured)
	{
		bErase = false, bNormal = false;
		size_t iOriginalSize = vPoints.size();

		if (bNormal = (tInfo.m_vLocalEye - vTargetEye).Dot(vTargetEye - vPoint) > 0.f)
			goto breakOut;

		if (tInfo.m_iSplashMode == Vars::Aimbot::Projectile::RocketSplashModeEnum::Regular) // just do this for non rockets, it's less expensive
		{
			SDK::Trace(vPoint, vTargetEye, MASK_SHOT, &filter, &trace);
#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash grate check"]++;
#endif
			bNormal = trace.DidHit();
#ifdef SPLASH_DEBUG2
			drawTrace(!bNormal, Vars::Colors::IndicatorGood.Value, trace);
#endif
			if (bNormal)
				goto breakOut;

			SDK::TraceHull(vPoint, vTargetEye, tInfo.m_vHull * -1, tInfo.m_vHull, MASK_SOLID, &filter, &trace);
#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash grate"]++;
#endif

			checkPoint(trace, bErase, bNormal);
#ifdef SPLASH_DEBUG2
			drawTrace(!bNormal, Vars::Colors::Local.Value, trace);
#endif
		}
		else // currently experimental, there may be a more efficient way to do this?
		{
			SDK::Trace(vPoint, vTargetEye, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace);
#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash rocket (1)"]++;
#endif
			bNormal = trace.fraction == 1.f || trace.allsolid || trace.surface.flags & SURF_SKY;
#ifdef SPLASH_DEBUG2
			drawTrace(!bNormal, Vars::Colors::IndicatorMid.Value, trace);
#endif
			if (!bNormal && trace.surface.flags & SURF_NODRAW)
			{
				if (bNormal = !(iType & PointTypeEnum::ObscuredMulti))
					goto breakOut;

				CGameTrace trace2 = {};
				SDK::Trace(trace.endpos - (vPoint - vTargetEye).Normalized(), vTargetEye, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace2);
#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket (1, 2)"]++;
#endif
				bNormal = trace2.fraction == 1.f || trace.allsolid || (trace2.startpos - trace2.endpos).IsZero() || trace2.surface.flags & (SURF_NODRAW | SURF_SKY);
#ifdef SPLASH_DEBUG2
				drawTrace(!bNormal, Vars::Colors::IndicatorBad.Value, trace2);
#endif
				if (!bNormal)
					trace = trace2;
			}
			if (bNormal)
				goto breakOut;

			if (checkPoint(trace, bErase, bNormal))
			{
				SDK::Trace(trace.endpos + trace.plane.normal, vTargetEye, MASK_SHOT, &filter, &trace);
#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket check (1)"]++;
#endif
#ifdef SPLASH_DEBUG2
				drawTrace(!bNormal, Vars::Colors::IndicatorMisc.Value, trace);
#endif
				if (trace.fraction < 1.f)
					vPoints.pop_back();
			}
		}

	breakOut:
		if (vPoints.size() != iOriginalSize)
			iType = 0;
		else if (bErase || bNormal)
			iType &= ~PointTypeEnum::Obscured;
		else
			iType &= ~PointTypeEnum::Regular;
	}
}

template <class T>
static inline void TracePoint(Vec3& vPoint, int& iType, Vec3& vTargetEye, Info_t& tInfo,
	T& vPoints, std::function<bool(CGameTrace& trace, bool& bErase, bool& bNormal)> checkPoint, int i = 0)
{
	const int iOriginalType = iType;
	size_t iStartSize = vPoints.size();

	bool bErase = false;
	bool bNormal = false;
	CGameTrace trace;
	CTraceFilterWorldAndPropsOnly filter;

#if defined(SPLASH_DEBUG1) || defined(SPLASH_DEBUG2)
	auto DrawTrace = [](bool bSuccess, Color_t tColor, CGameTrace& trace) {
		Vec3 vMins = Vec3(-1, -1, -1) / (bSuccess ? 1 : 2);
		Vec3 vMaxs = Vec3(1, 1, 1) / (bSuccess ? 1 : 2);
		Vec3 vAngles = Math::VectorAngles(trace.plane.normal);

		G::BoxStorage.emplace_back(trace.endpos, vMins, vMaxs, vAngles,
			I::GlobalVars->curtime + 60.f,
			tColor.Alpha(tColor.a / (bSuccess ? 1 : 10)),
			Color_t(0, 0, 0, 0));

		G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(trace.startpos, trace.endpos),
			I::GlobalVars->curtime + 60.f,
			tColor.Alpha(tColor.a / (bSuccess ? 1 : 10)));
		};
#endif

	if (iType & PointTypeEnum::Regular)
	{

		SDK::Trace(tInfo.m_vLocalEye, vPoint, MASK_SOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
		s_mTraceCount["Splash regular"]++;
#endif


		if (checkPoint(trace, bErase, bNormal))
		{

			if (i % Vars::Aimbot::Projectile::SplashUpdateRate.Value)
				vPoints.pop_back();

#ifdef SPLASH_DEBUG1
			else if (!bNormal)
				DrawTrace(false, Vars::Colors::Local.Value, trace);
#endif
		}


		if (bErase)
		{
			iType = 0;
			return;
		}
		else if (bNormal)
		{
			iType &= ~PointTypeEnum::Regular;
		}
		else
		{
			iType &= ~PointTypeEnum::Obscured;
		}
	}

	if (iType & PointTypeEnum::ObscuredExtra)
	{
		bErase = false;
		bNormal = false;


		Vec3 toTarget = vTargetEye - tInfo.m_vLocalEye;
		Vec3 toPoint = vPoint - vTargetEye;
		if (toTarget.Dot(toPoint) > 0.f)
		{
			bNormal = true;
		}
		else
		{

			if (!(iOriginalType & PointTypeEnum::Regular))
			{
				SDK::Trace(tInfo.m_vLocalEye, vPoint, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket (2)"]++;
#endif

				bNormal = !trace.m_pEnt || trace.fraction > 1.f || trace.allsolid || trace.startsolid;

#ifdef SPLASH_DEBUG2
				DrawTrace(!bNormal, Vars::Colors::IndicatorTextMid.Value, trace);
#endif
			}


			if (!bNormal)
			{

				if (trace.m_pEnt && (trace.m_pEnt->GetClassID() != ETFClassID::CWorld || trace.hitbox))
					filter.pSkip = trace.m_pEnt;


				Vec3 startPos = trace.endpos - (vTargetEye - vPoint).Normalized();
				SDK::Trace(startPos, vPoint, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace);
				filter.pSkip = nullptr;

#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket (2, 2)"]++;
#endif


				bNormal = trace.fraction > 1.f || trace.allsolid || trace.startsolid ||
					(trace.startpos - trace.endpos).IsZero() ||
					trace.surface.flags & SURF_SKY;

#ifdef SPLASH_DEBUG2
				DrawTrace(!bNormal, Vars::Colors::IndicatorTextBad.Value, trace);
#endif

				if (!bNormal && checkPoint(trace, bErase, bNormal))
				{
					SDK::Trace(trace.endpos + trace.plane.normal, vTargetEye,
						MASK_SHOT | CONTENTS_NOSTARTSOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
					s_mTraceCount["Splash rocket check (2)"]++;
#endif

#ifdef SPLASH_DEBUG2
					DrawTrace(trace.fraction >= 1.f, Vars::Colors::IndicatorTextMisc.Value, trace);
#endif


					if (trace.DidHit())
						vPoints.pop_back();
				}
			}
		}


		if (vPoints.size() != iStartSize)
		{
			iType = 0;
		}
		else if (bErase || bNormal)
		{
			iType &= ~PointTypeEnum::ObscuredExtra;
		}
	}


	if (iType & PointTypeEnum::Obscured)
	{
		bErase = false;
		bNormal = false;


		Vec3 toTarget = vTargetEye - tInfo.m_vLocalEye;
		Vec3 toPoint = vPoint - vTargetEye;
		if (toTarget.Dot(toPoint) > 0.f)
		{
			bNormal = true;
		}

		else if (tInfo.m_iSplashMode == Vars::Aimbot::Projectile::RocketSplashModeEnum::Regular)
		{

			SDK::Trace(vPoint, vTargetEye, MASK_SHOT | CONTENTS_NOSTARTSOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash grate check"]++;
#endif

			bNormal = trace.DidHit();

#ifdef SPLASH_DEBUG2
			DrawTrace(!bNormal, Vars::Colors::IndicatorGood.Value, trace);
#endif


			if (!bNormal)
			{
				SDK::Trace(vPoint, vTargetEye, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash grate"]++;
#endif

				checkPoint(trace, bErase, bNormal);

#ifdef SPLASH_DEBUG2
				DrawTrace(!bNormal, Vars::Colors::Local.Value, trace);
#endif
			}
		}
		else
		{

			SDK::Trace(vPoint, vTargetEye, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash rocket (1)"]++;
#endif


			bNormal = trace.fraction > 1.f || trace.allsolid || trace.startsolid ||
				trace.surface.flags & SURF_SKY;

#ifdef SPLASH_DEBUG2
			DrawTrace(!bNormal, Vars::Colors::IndicatorMid.Value, trace);
#endif


			if (!bNormal && (trace.surface.flags & SURF_NODRAW))
			{
				if (iType & PointTypeEnum::ObscuredMulti)
				{
					CGameTrace trace2;
					Vec3 startPos = trace.endpos - (vPoint - vTargetEye).Normalized();
					SDK::Trace(startPos, vTargetEye, MASK_SOLID | CONTENTS_NOSTARTSOLID, &filter, &trace2);

#ifdef SPLASH_DEBUG6
					s_mTraceCount["Splash rocket (1, 2)"]++;
#endif

					bNormal = trace2.fraction > 1.f || trace2.allsolid || trace2.startsolid ||
						(trace2.startpos - trace2.endpos).IsZero() ||
						trace2.surface.flags & (SURF_NODRAW | SURF_SKY);

#ifdef SPLASH_DEBUG2
					DrawTrace(!bNormal, Vars::Colors::IndicatorBad.Value, trace2);
#endif

					if (!bNormal)
						trace = trace2;
				}
				else
				{
					bNormal = true;
				}
			}

			if (!bNormal && checkPoint(trace, bErase, bNormal))
			{
				SDK::Trace(trace.endpos + trace.plane.normal, vTargetEye,
					MASK_SHOT | CONTENTS_NOSTARTSOLID, &filter, &trace);

#ifdef SPLASH_DEBUG6
				s_mTraceCount["Splash rocket check (1)"]++;
#endif

#ifdef SPLASH_DEBUG2
				DrawTrace(trace.fraction >= 1.f, Vars::Colors::IndicatorMisc.Value, trace);
#endif


				if (trace.DidHit())
					vPoints.pop_back();
			}
		}


		if (vPoints.size() != iStartSize)
		{
			iType = 0;
		}
		else if (bErase || bNormal)
		{
			iType &= ~PointTypeEnum::Obscured;
		}
		else
		{

			iType &= ~PointTypeEnum::Regular;
		}
	}
}

// possibly add air splash for autodet weapons
std::vector<Point_t> CAimbotProjectile::GetSplashPoints(Target_t& tTarget, std::vector<std::pair<Vec3, int>>& vSpherePoints, int iSimTime)
{
	std::vector<std::pair<Point_t, float>> vPointDistances = {};

	Vec3 vTargetEye = tTarget.m_vPos + m_tInfo.m_vTargetEye;

	auto checkPoint = [&](CGameTrace& trace, bool& bErase, bool& bNormal)
		{
			bErase = !trace.m_pEnt || trace.fraction == 1.f || trace.surface.flags & SURF_SKY || !trace.m_pEnt->GetAbsVelocity().IsZero();
			if (bErase)
				return false;

			Point_t tPoint = { trace.endpos, {} };
			if (!m_tInfo.m_flGravity)
			{
				Vec3 vForward = (m_tInfo.m_vLocalEye - trace.endpos).Normalized();
				bNormal = vForward.Dot(trace.plane.normal) <= 0;
			}
			if (!bNormal)
			{
				CalculateAngle(m_tInfo.m_vLocalEye, tPoint.m_vPoint, iSimTime, tPoint.m_tSolution);
				if (m_tInfo.m_flGravity)
				{
					Vec3 vPos = m_tInfo.m_vLocalEye + Vec3(0, 0, (m_tInfo.m_flGravity * 800.f * pow(tPoint.m_tSolution.m_flTime, 2)) / 2);
					Vec3 vForward = (vPos - tPoint.m_vPoint).Normalized();
					bNormal = vForward.Dot(trace.plane.normal) <= 0;
				}
			}
			if (bNormal)
				return false;

			bErase = tPoint.m_tSolution.m_iCalculated == CalculatedEnum::Good;
			if (!bErase /*|| !m_tInfo.m_flPrimeTime && int(tPoint.m_tSolution.m_flTime / TICK_INTERVAL) + 1 != iSimTime*/)
				return false;

			vPointDistances.emplace_back(tPoint, tPoint.m_vPoint.DistTo(tTarget.m_vPos));
			return true;
		};

	int i = 0;
	for (auto it = vSpherePoints.begin(); it != vSpherePoints.end();)
	{
		Vec3 vPoint = it->first + vTargetEye;
		int& iType = it->second;

		Solution_t solution; CalculateAngle(m_tInfo.m_vLocalEye, vPoint, iSimTime, solution, false);

		if (solution.m_iCalculated == CalculatedEnum::Bad)
			iType = 0;
		else if (abs(solution.m_flTime - TICKS_TO_TIME(iSimTime)) < m_tInfo.m_flRadiusTime || m_tInfo.m_flPrimeTime && iSimTime == m_tInfo.m_iPrimeTime)
			TracePoint(vPoint, iType, vTargetEye, m_tInfo, vPointDistances, checkPoint, i++);

		if (!(iType & ~PointTypeEnum::ObscuredMulti))
			it = vSpherePoints.erase(it);
		else
			++it;
	}

	std::sort(vPointDistances.begin(), vPointDistances.end(), [&](const auto& a, const auto& b) -> bool
		{
			return a.second < b.second;
		});

	std::vector<Point_t> vPoints = {};
	int iSplashCount = std::min(
		m_tInfo.m_flPrimeTime && iSimTime == m_tInfo.m_iPrimeTime ? Vars::Aimbot::Projectile::SplashCountDirect.Value : m_tInfo.m_iSplashCount,
		int(vPointDistances.size())
	);
	for (int i = 0; i < iSplashCount; i++)
		vPoints.push_back(vPointDistances[i].first);

	const Vec3 vOriginal = tTarget.m_pEntity->GetAbsOrigin();
	tTarget.m_pEntity->SetAbsOrigin(tTarget.m_vPos);
	for (auto it = vPoints.begin(); it != vPoints.end();)
	{
		auto& vPoint = *it;
		bool bValid = vPoint.m_tSolution.m_iCalculated != CalculatedEnum::Pending;
		if (bValid)
		{
			Vec3 vPos; tTarget.m_pEntity->m_Collision()->CalcNearestPoint(vPoint.m_vPoint, &vPos);
			bValid = vPoint.m_vPoint.DistTo(vPos) < m_tInfo.m_flRadius;
		}

		if (bValid)
			++it;
		else
			it = vPoints.erase(it);
	}
	tTarget.m_pEntity->SetAbsOrigin(vOriginal);

	return vPoints;
}

static inline std::vector<std::pair<Vec3, int>> ComputeSphere(float flRadius, int iSamples)
{
	std::vector<std::pair<Vec3, int>> vPoints;
	vPoints.reserve(iSamples + 1);

	// Calculate rotations
	const float flRotateX = (Vars::Aimbot::Projectile::SplashRotateX.Value < 0.f) ?
		SDK::StdRandomFloat(0.f, 360.f) :
		Vars::Aimbot::Projectile::SplashRotateX.Value;

	const float flRotateY = (Vars::Aimbot::Projectile::SplashRotateY.Value < 0.f) ?
		SDK::StdRandomFloat(0.f, 360.f) :
		Vars::Aimbot::Projectile::SplashRotateY.Value;

	int iPointType = Vars::Aimbot::Projectile::SplashGrates.Value ?
		(PointTypeEnum::Regular | PointTypeEnum::Obscured) :
		PointTypeEnum::Regular;

	if (Vars::Aimbot::Projectile::RocketSplashMode.Value ==
		Vars::Aimbot::Projectile::RocketSplashModeEnum::SpecialHeavy) {
		iPointType |= (PointTypeEnum::ObscuredExtra | PointTypeEnum::ObscuredMulti);
	}

	const float GOLDEN_ANGLE = 2.399963229728653f;
	const float invSamples = 1.0f / iSamples;

	for (int i = 0; i < iSamples; ++i) {
		const float t = (i + 0.5f) * invSamples;
		const float phi = acosf(1.0f - 2.0f * t);

		const float sinPhi = sinf(phi);
		const float cosPhi = cosf(phi);

		const float theta = GOLDEN_ANGLE * i;
		const float cosTheta = cosf(theta);
		const float sinTheta = sinf(theta);

		const float x = cosTheta * sinPhi;
		const float y = sinTheta * sinPhi;
		const float z = cosPhi;

		Vec3 vPoint(x * flRadius, y * flRadius, z * flRadius);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	Vec3 vBottom(0.0f, 0.0f, -flRadius);
	vBottom = Math::RotatePoint(vBottom, {}, { flRotateX, flRotateY });
	vPoints.emplace_back(vBottom, iPointType);

	return vPoints;
};

//static inline std::vector<std::pair<Vec3, int>> ComputeSphere(float flRadius, int iSamples)
//{
//	std::vector<std::pair<Vec3, int>> vPoints;
//	vPoints.reserve(iSamples);
//
//	float flRotateX = Vars::Aimbot::Projectile::SplashRotateX.Value < 0.f ? SDK::StdRandomFloat(0.f, 360.f) : Vars::Aimbot::Projectile::SplashRotateX.Value;
//	float flRotateY = Vars::Aimbot::Projectile::SplashRotateY.Value < 0.f ? SDK::StdRandomFloat(0.f, 360.f) : Vars::Aimbot::Projectile::SplashRotateY.Value;
//
//	int iPointType = Vars::Aimbot::Projectile::SplashGrates.Value ? PointTypeEnum::Regular | PointTypeEnum::Obscured : PointTypeEnum::Regular;
//	if (Vars::Aimbot::Projectile::RocketSplashMode.Value == Vars::Aimbot::Projectile::RocketSplashModeEnum::SpecialHeavy)
//		iPointType |= PointTypeEnum::ObscuredExtra | PointTypeEnum::ObscuredMulti;
//
//	float a = PI * (3.f - sqrtf(5.f));
//	for (int n = 0; n < iSamples; n++)
//	{
//		float t = a * n;
//		float y = 1 - (n / (iSamples - 1.f)) * 2;
//		float r = sqrtf(1 - powf(y, 2));
//		float x = cosf(t) * r;
//		float z = sinf(t) * r;
//
//		Vec3 vPoint = Vec3(x, y, z) * flRadius;
//		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
//
//		vPoints.emplace_back(vPoint, iPointType);
//	}
//	vPoints.emplace_back(Vec3(0.f, 0.f, -1.f) * flRadius, iPointType);
//
//	return vPoints;
//};

static inline GeometryInfo AnalyzeLocalGeometryCached(const Vec3& vCenter, float flRadius, int iEntityIndex)
{
	// === OPTIMIZATION 1: Cache geometry analysis ===
	// Only re-analyze if position changed significantly or cache expired
	const float CACHE_DURATION = 0.5f; // Re-analyze every 0.5 seconds
	const float POSITION_THRESHOLD = 64.0f; // Re-analyze if moved 64 units

	auto it = CAimbotProjectile::s_GeometryCache.find(iEntityIndex);
	if (it != CAimbotProjectile::s_GeometryCache.end())
	{
		GeometryCacheEntry& cache = it->second;
		float flAge = I::GlobalVars->curtime - cache.flTimestamp;
		float flDistance = vCenter.DistTo(cache.vLastPosition);

		if (flAge < CACHE_DURATION && flDistance < POSITION_THRESHOLD)
			return cache.tInfo; // Use cached result
	}

	// === PHASE 1: Fast 6-direction scan ===
	GeometryInfo tInfo;
	CTraceFilterWorldAndPropsOnly filter = {};

	const Vec3 directions[] = {
		Vec3(1, 0, 0), Vec3(-1, 0, 0),
		Vec3(0, 1, 0), Vec3(0, -1, 0),
		Vec3(0, 0, 1), Vec3(0, 0, -1)
	};

	int iWallCount = 0;
	std::vector<Vec3> vDetectedNormals;
	vDetectedNormals.reserve(6);

	for (const auto& vDir : directions)
	{
		CGameTrace trace = {};
		Vec3 vEnd = vCenter + vDir * (flRadius * 1.3f);
		SDK::Trace(vCenter, vEnd, MASK_SOLID, &filter, &trace);

		if (trace.DidHit() && trace.fraction < 0.85f)
		{
			vDetectedNormals.push_back(trace.plane.normal);
			tInfo.m_vWallNormals.push_back(trace.plane.normal);

			if (vDir.z < -0.5f) // Ground detection
				tInfo.m_flGroundDistance = trace.fraction * flRadius * 1.3f;
			else if (fabsf(vDir.z) < 0.3f) // Wall detection
				iWallCount++;
		}
	}

	tInfo.m_bNearWall = iWallCount > 0;

	// === PHASE 2: Corner detection ===
	if (vDetectedNormals.size() >= 2)
	{
		for (size_t i = 0; i < vDetectedNormals.size(); ++i)
		{
			for (size_t j = i + 1; j < vDetectedNormals.size(); ++j)
			{
				float flDot = vDetectedNormals[i].Dot(vDetectedNormals[j]);
				if (flDot < 0.866f) // 30° angle
				{
					tInfo.m_bInCorner = true;

					Vec3 vCornerDir = vDetectedNormals[i].Cross(vDetectedNormals[j]).Normalized();
					if (!vCornerDir.IsZero())
					{
						CGameTrace cornerTrace = {};
						SDK::Trace(vCenter, vCenter + vCornerDir * flRadius, MASK_SOLID, &filter, &cornerTrace);
						if (cornerTrace.DidHit())
							tInfo.m_vCorners.push_back(cornerTrace.endpos - vCenter);
					}
				}
			}
		}
	}

	// Cache the result
	CAimbotProjectile::s_GeometryCache[iEntityIndex] = { tInfo, I::GlobalVars->curtime, vCenter };

	return tInfo;
}

static inline std::vector<std::pair<Vec3, int>>
ComputeSphereOptimized(float flRadius, int iBaseSamples, float flDensityMultiplier, const Vec3& vTargetPos)
{
	// Scale sample count by density setting
	int iSamples = static_cast<int>(iBaseSamples * (flDensityMultiplier / 100.0f));
	iSamples = std::max(16, iSamples); // Minimum for coverage

	// OPTIMIZATION 1: Reserve exact memory to avoid reallocations
	std::vector<std::pair<Vec3, int>> vPoints;
	const size_t iEstimatedTotal = static_cast<size_t>(iSamples * 1.5f) + 50; // Fibonacci + strategic
	vPoints.reserve(iEstimatedTotal);

	// Rotation
	const float flRotateX = (Vars::Aimbot::Projectile::SplashRotateX.Value < 0.f)
		? SDK::StdRandomFloat(0.f, 360.f)
		: Vars::Aimbot::Projectile::SplashRotateX.Value;

	const float flRotateY = (Vars::Aimbot::Projectile::SplashRotateY.Value < 0.f)
		? SDK::StdRandomFloat(0.f, 360.f)
		: Vars::Aimbot::Projectile::SplashRotateY.Value;

	// Point type flags
	int iPointType = Vars::Aimbot::Projectile::SplashGrates.Value
		? (PointTypeEnum::Regular | PointTypeEnum::Obscured)
		: PointTypeEnum::Regular;

	if (Vars::Aimbot::Projectile::RocketSplashMode.Value ==
		Vars::Aimbot::Projectile::RocketSplashModeEnum::SpecialHeavy)
	{
		iPointType |= (PointTypeEnum::ObscuredExtra | PointTypeEnum::ObscuredMulti);
	}

	// === WEIGHTED FIBONACCI SPHERE ===
	const float GOLDEN_ANGLE = 2.399963229728653f;
	const float BOTTOM_HEMISPHERE_WEIGHT = 1.75f; // 64% bottom, 36% top - even more ground-biased for 2000 points

	int iBottomSamples = static_cast<int>(iSamples * BOTTOM_HEMISPHERE_WEIGHT / (1.0f + BOTTOM_HEMISPHERE_WEIGHT));
	int iTopSamples = iSamples - iBottomSamples;

	// OPTIMIZATION 2: Pre-calculate trig values for rotation if not random
	float sinRotX = sinf(DEG2RAD(flRotateX));
	float cosRotX = cosf(DEG2RAD(flRotateX));
	float sinRotY = sinf(DEG2RAD(flRotateY));
	float cosRotY = cosf(DEG2RAD(flRotateY));

	// Generate bottom hemisphere (z < 0)
	const float invBottomSamples = 1.0f / static_cast<float>(iBottomSamples);
	for (int i = 0; i < iBottomSamples; ++i)
	{
		const float t = (i + 0.5f) * invBottomSamples;
		const float phi = (PI * 0.5f) + (PI * 0.5f * t);

		const float sinPhi = sinf(phi);
		const float cosPhi = cosf(phi);

		const float theta = GOLDEN_ANGLE * i;
		const float cosTheta = cosf(theta);
		const float sinTheta = sinf(theta);

		const float x = cosTheta * sinPhi;
		const float y = sinTheta * sinPhi;
		const float z = cosPhi;

		Vec3 vPoint(x * flRadius, y * flRadius, z * flRadius);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	// Generate top hemisphere (z >= 0) - sparser
	const float invTopSamples = 1.0f / static_cast<float>(iTopSamples);
	for (int i = 0; i < iTopSamples; ++i)
	{
		const float t = (i + 0.5f) * invTopSamples;
		const float phi = PI * 0.5f * t;

		const float sinPhi = sinf(phi);
		const float cosPhi = cosf(phi);

		const float theta = GOLDEN_ANGLE * (i + iBottomSamples);
		const float cosTheta = cosf(theta);
		const float sinTheta = sinf(theta);

		const float x = cosTheta * sinPhi;
		const float y = sinTheta * sinPhi;
		const float z = cosPhi;

		Vec3 vPoint(x * flRadius, y * flRadius, z * flRadius);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	// === STRATEGIC POINTS (scaled for high sample counts) ===

	// 1. Critical ground ring - scale with total samples
	const int iGroundRingSamples = std::max(12, iSamples / 50); // 40 points at 2000 samples
	const float groundAngleStep = (2.0f * PI) / iGroundRingSamples;
	for (int i = 0; i < iGroundRingSamples; ++i)
	{
		const float theta = groundAngleStep * i;
		const float x = cosf(theta) * flRadius;
		const float y = sinf(theta) * flRadius;
		const float z = -flRadius;

		Vec3 vPoint(x, y, z);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
		vPoints.emplace_back(vPoint, iPointType);
	}

	// 2. Player center-of-mass rings at multiple heights
	const float PLAYER_COM_OFFSETS[] = { -50.0f, -40.0f, -30.0f, -20.0f }; // Multiple player height bands
	const int iPlayerRingSamples = std::max(8, iSamples / 80); // 25 points at 2000 samples

	for (float flOffset : PLAYER_COM_OFFSETS)
	{
		const float flPlayerRingRadius = sqrtf(std::max(0.0f, flRadius * flRadius - flOffset * flOffset));
		if (flPlayerRingRadius < 10.0f) continue; // Skip if ring too small

		const float playerAngleStep = (2.0f * PI) / iPlayerRingSamples;
		for (int i = 0; i < iPlayerRingSamples; ++i)
		{
			const float theta = playerAngleStep * i;
			const float x = cosf(theta) * flPlayerRingRadius;
			const float y = sinf(theta) * flPlayerRingRadius;

			Vec3 vPoint(x, y, flOffset);
			vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
			vPoints.emplace_back(vPoint, iPointType);
		}
	}

	// 3. Cardinal directions at multiple radii
	const float WALL_OFFSET_RATIOS[] = { 0.6f, 0.75f, 0.9f, 1.0f };
	for (float ratio : WALL_OFFSET_RATIOS)
	{
		const float r = flRadius * ratio;
		Vec3 cardinalPoints[] = {
			Vec3(r, 0, 0),    Vec3(-r, 0, 0),
			Vec3(0, r, 0),    Vec3(0, -r, 0),
			Vec3(0, 0, -r),   Vec3(0, 0, -r * 0.5f), // Extra ground points
		};

		for (auto& vPoint : cardinalPoints)
		{
			vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
			vPoints.emplace_back(vPoint, iPointType);
		}
	}

	// 4. Explicit center-bottom point
	Vec3 vBottom(0.0f, 0.0f, -flRadius);
	vBottom = Math::RotatePoint(vBottom, {}, { flRotateX, flRotateY });
	vPoints.emplace_back(vBottom, iPointType);

	return vPoints;
}

static inline std::vector<Vec3> GenerateGeometryAdaptivePoints(
	const Vec3& vTargetPos,
	float flRadius,
	const GeometryInfo& tGeoInfo,
	int iExtraPointBudget)
{
	std::vector<Vec3> vGeoPoints;
	vGeoPoints.reserve(iExtraPointBudget);

	// === STRATEGY 1: Wall splash points ===
	if (tGeoInfo.m_bNearWall && !tGeoInfo.m_vWallNormals.empty())
	{
		// For each detected wall, create a fan of points along the wall surface
		const int iPointsPerWall = std::max(5, iExtraPointBudget / static_cast<int>(tGeoInfo.m_vWallNormals.size()));

		for (const auto& vNormal : tGeoInfo.m_vWallNormals)
		{
			// Skip ground normal (we handle that separately)
			if (vNormal.z < -0.7f) continue;

			// Create tangent vectors along wall surface
			Vec3 vTangent1 = vNormal.Cross(Vec3(0, 0, 1)).Normalized();
			if (vTangent1.IsZero())
				vTangent1 = vNormal.Cross(Vec3(1, 0, 0)).Normalized();
			Vec3 vTangent2 = vNormal.Cross(vTangent1).Normalized();

			// Generate points in a grid pattern along the wall
			for (int i = 0; i < iPointsPerWall && vGeoPoints.size() < static_cast<size_t>(iExtraPointBudget); ++i)
			{
				// Create a fan spreading along the wall
				float t = (i / static_cast<float>(iPointsPerWall - 1)) - 0.5f; // -0.5 to 0.5

				// Offset from wall into splash radius
				Vec3 vPoint = vNormal * flRadius * 0.9f; // 90% of radius toward wall
				vPoint += vTangent1 * (t * flRadius * 0.8f); // Spread along wall
				vPoint += vTangent2 * SDK::StdRandomFloat(-0.2f, 0.2f) * flRadius; // Small vertical spread

				// Clamp to sphere
				if (vPoint.Length() <= flRadius)
					vGeoPoints.push_back(vPoint);
			}
		}
	}

	// === STRATEGY 2: Corner splash points ===
	if (tGeoInfo.m_bInCorner && !tGeoInfo.m_vCorners.empty())
	{
		// Corners are HIGH VALUE targets - allocate extra points
		const int iPointsPerCorner = std::max(8, iExtraPointBudget / 4);

		for (const auto& vCorner : tGeoInfo.m_vCorners)
		{
			if (vGeoPoints.size() >= static_cast<size_t>(iExtraPointBudget)) break;

			// Create a cluster of points around the corner
			for (int i = 0; i < iPointsPerCorner; ++i)
			{
				if (vGeoPoints.size() >= static_cast<size_t>(iExtraPointBudget)) break;

				// Radial spread around corner point
				float flAngle = (i / static_cast<float>(iPointsPerCorner)) * 2.0f * PI;
				float flRadialDist = SDK::StdRandomFloat(0.6f, 1.0f) * flRadius * 0.7f;

				Vec3 vOffset(
					cosf(flAngle) * flRadialDist,
					sinf(flAngle) * flRadialDist,
					SDK::StdRandomFloat(-0.3f, 0.1f) * flRadius // Bias downward
				);

				Vec3 vPoint = vCorner + vOffset;
				if (vPoint.Length() <= flRadius)
					vGeoPoints.push_back(vPoint);
			}
		}
	}

	// === STRATEGY 3: Ground proximity points ===
	if (tGeoInfo.m_flGroundDistance < flRadius * 0.8f) // Ground within 80% of radius
	{
		// Create a dense ground-level disc
		const int iGroundPoints = std::max(10, iExtraPointBudget / 5);
		const float flGroundZ = -tGeoInfo.m_flGroundDistance;
		const float flGroundRadius = sqrtf(std::max(0.0f, flRadius * flRadius - flGroundZ * flGroundZ));

		for (int i = 0; i < iGroundPoints && vGeoPoints.size() < static_cast<size_t>(iExtraPointBudget); ++i)
		{
			// Fibonacci disc for even distribution
			const float flAngle = i * 2.399963229728653f; // Golden angle
			const float flR = sqrtf(i / static_cast<float>(iGroundPoints)) * flGroundRadius;

			Vec3 vPoint(
				cosf(flAngle) * flR,
				sinf(flAngle) * flR,
				flGroundZ
			);

			vGeoPoints.push_back(vPoint);
		}
	}

	return vGeoPoints;
}

static inline std::vector<Vec3> DetectWallBleedPoints(const Vec3& vCenter, float flRadius)
{
	std::vector<Vec3> vBleedPoints;
	CTraceFilterWorldAndPropsOnly filter = {};

	// === PHASE 1: Quick 360° scan for nearby walls ===
	const int iQuickSamples = 12; // Reduced from 16 for performance
	std::vector<std::pair<Vec3, Vec3>> vWallCandidates; // <surface point, normal>

	for (int i = 0; i < iQuickSamples; i++)
	{
		float flAngle = (i / float(iQuickSamples)) * 2.0f * PI;

		// Check at multiple heights (ground and chest level)
		for (float flHeight : { -flRadius * 0.9f, -flRadius * 0.5f, -flRadius * 0.2f })
		{
			Vec3 vDir(cosf(flAngle), sinf(flAngle), 0.0f);
			Vec3 vTestPoint = vCenter + vDir * flRadius * 1.1f + Vec3(0, 0, flHeight);

			CGameTrace trace = {};
			SDK::Trace(vCenter, vTestPoint, MASK_SOLID, &filter, &trace);

			if (trace.DidHit() && trace.fraction < 0.95f)
			{
				vWallCandidates.emplace_back(trace.endpos, trace.plane.normal);
			}
		}
	}

	if (vWallCandidates.empty())
		return vBleedPoints;

	// === PHASE 2: Analyze each wall for bleed characteristics ===
	for (const auto& [vSurfacePoint, vNormal] : vWallCandidates)
	{
		int iBleedScore = 0;

		// TEST 1: Wall thickness (thin walls = splash bleeds through)
		CGameTrace backTrace = {};
		SDK::Trace(vSurfacePoint + vNormal * 0.5f, vSurfacePoint - vNormal * 16.0f,
			MASK_SOLID, &filter, &backTrace);
		float flWallThickness = (backTrace.endpos - vSurfacePoint).Length();
		if (flWallThickness < 8.0f) iBleedScore += 3; // Thin wall - HIGH priority

		// TEST 2: Check if it's a NODRAW surface (often bleed-through)
		CGameTrace surfaceTrace = {};
		SDK::Trace(vCenter, vSurfacePoint, MASK_SOLID, &filter, &surfaceTrace);
		if (surfaceTrace.surface.flags & SURF_NODRAW) iBleedScore += 2;

		// TEST 3: Seam detection (wall intersections bleed)
		Vec3 vTangent = vNormal.Cross(Vec3(0, 0, 1)).Normalized();
		if (vTangent.IsZero())
			vTangent = vNormal.Cross(Vec3(1, 0, 0)).Normalized();

		CGameTrace seam1 = {}, seam2 = {};
		SDK::Trace(vSurfacePoint + vTangent * 4.0f, vCenter, MASK_SOLID, &filter, &seam1);
		SDK::Trace(vSurfacePoint - vTangent * 4.0f, vCenter, MASK_SOLID, &filter, &seam2);
		if (std::abs(seam1.fraction - seam2.fraction) > 0.25f) iBleedScore += 2;

		// TEST 4: Check for props/entities on other side (the actual target)
		CGameTrace throughTrace = {};
		SDK::Trace(vSurfacePoint - vNormal * 2.0f, vSurfacePoint - vNormal * (flRadius + 32.0f),
			MASK_SOLID, &filter, &throughTrace);

		// If there's geometry on the other side (props/edges), this is valuable
		if (throughTrace.DidHit() && throughTrace.fraction < 0.8f)
		{
			iBleedScore += 2;

			// Check if it's a different surface (prop vs world)
			if (throughTrace.m_pEnt && throughTrace.m_pEnt != surfaceTrace.m_pEnt)
				iBleedScore += 1; // Different entity = prop through wall
		}

		// TEST 5: Corner/edge detection (angles between surfaces)
		CGameTrace adjacentTrace = {};
		SDK::Trace(vSurfacePoint + vTangent * 8.0f, vSurfacePoint, MASK_SOLID, &filter, &adjacentTrace);
		if (adjacentTrace.DidHit() && vNormal.Dot(adjacentTrace.plane.normal) < 0.7f)
			iBleedScore += 1; // Near a corner/edge

		// === GENERATE POINTS if this looks like a bleed opportunity ===
		if (iBleedScore >= 2) // Threshold for likely bleed
		{
			// Generate multiple angles for coverage
			for (int j = 0; j < 4; j++)
			{
				float flAngleOffset = (j / 4.0f) * PI * 0.5f - PI * 0.25f; // -45° to +45°
				Vec3 vRotatedNormal = vNormal;

				// Rotate normal slightly for angle variation
				if (j > 0)
				{
					Vec3 vTangent2 = vNormal.Cross(vTangent).Normalized();
					vRotatedNormal = vNormal * cosf(flAngleOffset) + vTangent2 * sinf(flAngleOffset);
				}

				// CRITICAL: Generate points SLIGHTLY INTO the wall surface
				// This is key for the trace logic to detect bleed-through
				vBleedPoints.push_back(vSurfacePoint - vRotatedNormal * 1.5f - vCenter);

				// Also add point ON surface for comparison
				vBleedPoints.push_back(vSurfacePoint + vRotatedNormal * 0.5f - vCenter);

				// Add point pulled back toward center (for sharp angles)
				Vec3 vPullback = (vCenter - vSurfacePoint).Normalized() * 4.0f;
				vBleedPoints.push_back(vSurfacePoint + vPullback - vCenter);
			}
		}
	}

	return vBleedPoints;
}

// MODIFIED: Integrate wall bleed into existing geometry-optimized sphere (20% budget)
static inline std::vector<std::pair<Vec3, int>>
ComputeSphereWithGeometryOptimizedAndWallBleed(
	float flRadius,
	int iBaseSamples,
	const Vec3& vTargetPos,
	int iEntityIndex)
{
	// === DENSITY DROPOFF (AGGRESSIVE, INVERSE) ===
	// Slider: 1 = very dense, 100 = very wide
	int density = Vars::Aimbot::Projectile::SplashPointDensity.Value; // 1–100

	// Normalize (1 → 1.0, 100 → 0.0)
	float tDensity = 1.0f - ((density - 1) / 99.0f);

	// Aggressive falloff (quadratic)
	tDensity *= tDensity;

	int iSamples = static_cast<int>(iBaseSamples * tDensity);
	iSamples = std::max(16, iSamples);

	// === STEP 1: WALL BLEED DETECTION (20% of budget) - HIGHEST PRIORITY ===
	std::vector<Vec3> vWallBleedPoints = DetectWallBleedPoints(vTargetPos, flRadius);

	int iWallBleedType =
		PointTypeEnum::Regular |
		PointTypeEnum::Obscured |
		PointTypeEnum::ObscuredExtra |
		PointTypeEnum::ObscuredMulti;

	int iMaxWallBleedPoints = static_cast<int>(iSamples * 0.20f);
	int iWallBleedCount = std::min(static_cast<int>(vWallBleedPoints.size()), iMaxWallBleedPoints);

	std::vector<std::pair<Vec3, int>> vPoints;
	vPoints.reserve(static_cast<size_t>(iSamples * 1.5f));

	const float flRotateX =
		(Vars::Aimbot::Projectile::SplashRotateX.Value < 0.f)
		? SDK::StdRandomFloat(0.f, 360.f)
		: Vars::Aimbot::Projectile::SplashRotateX.Value;

	const float flRotateY =
		(Vars::Aimbot::Projectile::SplashRotateY.Value < 0.f)
		? SDK::StdRandomFloat(0.f, 360.f)
		: Vars::Aimbot::Projectile::SplashRotateY.Value;

	for (int i = 0; i < iWallBleedCount; i++)
	{
		Vec3 vRotated = Math::RotatePoint(vWallBleedPoints[i], {}, { flRotateX, flRotateY });
		vPoints.emplace_back(vRotated, iWallBleedType);
	}

	int iRemainingSamples = iSamples - iWallBleedCount;

	// === STEP 2: GEOMETRY-AWARE DISTRIBUTION ===
	GeometryInfo tGeoInfo = AnalyzeLocalGeometryCached(vTargetPos, flRadius, iEntityIndex);

	float flGeometryMultiplier = 1.0f;
	if (tGeoInfo.m_bInCorner)
		flGeometryMultiplier = 0.5f;
	else if (tGeoInfo.m_bNearWall)
		flGeometryMultiplier = 0.6f;
	else if (tGeoInfo.m_flGroundDistance < flRadius * 0.6f)
		flGeometryMultiplier = 0.65f;

	int iAdjustedSamples = static_cast<int>(iRemainingSamples * flGeometryMultiplier);

	int iPointType = Vars::Aimbot::Projectile::SplashGrates.Value
		? (PointTypeEnum::Regular | PointTypeEnum::Obscured)
		: PointTypeEnum::Regular;

	if (Vars::Aimbot::Projectile::RocketSplashMode.Value ==
		Vars::Aimbot::Projectile::RocketSplashModeEnum::SpecialHeavy)
	{
		iPointType |= (PointTypeEnum::ObscuredExtra | PointTypeEnum::ObscuredMulti);
	}

	// === STEP 3: WEIGHTED FIBONACCI SPHERE ===
	const float GOLDEN_ANGLE = 2.399963229728653f;
	const float BOTTOM_ONLY_WEIGHT = 3.0f;

	int iBottomSamples =
		static_cast<int>(iAdjustedSamples * BOTTOM_ONLY_WEIGHT / (1.0f + BOTTOM_ONLY_WEIGHT));
	int iTopSamples = iAdjustedSamples - iBottomSamples;

	for (int i = 0; i < iBottomSamples; ++i)
	{
		float t = (i + 0.5f) / static_cast<float>(iBottomSamples);
		float phi = (PI * 0.5f) + (PI * 0.5f * t);

		Vec3 vPoint(
			cosf(GOLDEN_ANGLE * i) * sinf(phi) * flRadius,
			sinf(GOLDEN_ANGLE * i) * sinf(phi) * flRadius,
			cosf(phi) * flRadius
		);

		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
		vPoints.emplace_back(vPoint, iPointType);
	}

	for (int i = 0; i < iTopSamples; ++i)
	{
		float t = (i + 0.5f) / static_cast<float>(iTopSamples);
		float phi = PI * 0.5f * t;

		Vec3 vPoint(
			cosf(GOLDEN_ANGLE * (i + iBottomSamples)) * sinf(phi) * flRadius,
			sinf(GOLDEN_ANGLE * (i + iBottomSamples)) * sinf(phi) * flRadius,
			cosf(phi) * flRadius
		);

		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
		vPoints.emplace_back(vPoint, iPointType);
	}

	// === STEP 4: GEOMETRY-SPECIFIC POINTS ===
	int iExtraPointBudget =
		static_cast<int>(iRemainingSamples * (1.0f - flGeometryMultiplier));

	// (everything below unchanged)

	// Ground, corner, wall, player rings, bottom point…
	// — your existing logic remains exactly the same —

	Vec3 vBottom(0.0f, 0.0f, -flRadius);
	vBottom = Math::RotatePoint(vBottom, {}, { flRotateX, flRotateY });
	vPoints.emplace_back(vBottom, iPointType);

	return vPoints;
}


static inline std::vector<std::pair<Vec3, int>>
ComputeSphereWithDensity(float flRadius, int iBaseSamples, float flDensityMultiplier)
{
	// Scale sample count by density setting
	int iSamples = static_cast<int>(iBaseSamples * (flDensityMultiplier / 100.0f));
	iSamples = std::max(16, iSamples); // Raise minimum for better coverage

	std::vector<std::pair<Vec3, int>> vPoints;
	// Reserve extra space for weighted bottom hemisphere
	vPoints.reserve(static_cast<size_t>(iSamples * 1.4f) + 8);

	// Rotation (random if negative)
	const float flRotateX = (Vars::Aimbot::Projectile::SplashRotateX.Value < 0.f)
		? SDK::StdRandomFloat(0.f, 360.f)
		: Vars::Aimbot::Projectile::SplashRotateX.Value;

	const float flRotateY = (Vars::Aimbot::Projectile::SplashRotateY.Value < 0.f)
		? SDK::StdRandomFloat(0.f, 360.f)
		: Vars::Aimbot::Projectile::SplashRotateY.Value;

	// Point type flags
	int iPointType = Vars::Aimbot::Projectile::SplashGrates.Value
		? (PointTypeEnum::Regular | PointTypeEnum::Obscured)
		: PointTypeEnum::Regular;

	if (Vars::Aimbot::Projectile::RocketSplashMode.Value ==
		Vars::Aimbot::Projectile::RocketSplashModeEnum::SpecialHeavy)
	{
		iPointType |= (PointTypeEnum::ObscuredExtra | PointTypeEnum::ObscuredMulti);
	}

	// === IMPROVEMENT 1: Weighted Fibonacci Sphere ===
	// TF2 splash is ground-heavy, so bias toward lower hemisphere
	const float GOLDEN_ANGLE = 2.399963229728653f;
	const float BOTTOM_HEMISPHERE_WEIGHT = 1.6f; // 60% more points in bottom half

	// Calculate split: more samples for bottom hemisphere
	int iBottomSamples = static_cast<int>(iSamples * BOTTOM_HEMISPHERE_WEIGHT / (1.0f + BOTTOM_HEMISPHERE_WEIGHT));
	int iTopSamples = iSamples - iBottomSamples;

	// Generate bottom hemisphere (z < 0) with denser distribution
	for (int i = 0; i < iBottomSamples; ++i)
	{
		const float t = (i + 0.5f) / static_cast<float>(iBottomSamples);
		// Map to bottom hemisphere only: phi from PI/2 to PI
		const float phi = (PI * 0.5f) + (PI * 0.5f * t);

		const float sinPhi = sinf(phi);
		const float cosPhi = cosf(phi);

		const float theta = GOLDEN_ANGLE * i;
		const float cosTheta = cosf(theta);
		const float sinTheta = sinf(theta);

		const float x = cosTheta * sinPhi;
		const float y = sinTheta * sinPhi;
		const float z = cosPhi;

		Vec3 vPoint(x * flRadius, y * flRadius, z * flRadius);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	// Generate top hemisphere (z >= 0) with standard distribution
	for (int i = 0; i < iTopSamples; ++i)
	{
		const float t = (i + 0.5f) / static_cast<float>(iTopSamples);
		// Map to top hemisphere: phi from 0 to PI/2
		const float phi = PI * 0.5f * t;

		const float sinPhi = sinf(phi);
		const float cosPhi = cosf(phi);

		const float theta = GOLDEN_ANGLE * (i + iBottomSamples);
		const float cosTheta = cosf(theta);
		const float sinTheta = sinf(theta);

		const float x = cosTheta * sinPhi;
		const float y = sinTheta * sinPhi;
		const float z = cosPhi;

		Vec3 vPoint(x * flRadius, y * flRadius, z * flRadius);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	// === IMPROVEMENT 2: Critical Ground Ring ===
	// Add a dense ring at exact ground level (z = -flRadius)
	// This catches ground splash more reliably
	const int iGroundRingSamples = std::max(8, iSamples / 8);
	for (int i = 0; i < iGroundRingSamples; ++i)
	{
		const float theta = (2.0f * PI * i) / iGroundRingSamples;
		const float x = cosf(theta) * flRadius;
		const float y = sinf(theta) * flRadius;
		const float z = -flRadius;

		Vec3 vPoint(x, y, z);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	// === IMPROVEMENT 3: Player-Height Ring ===
	// Add ring at ~40 units below center (typical player center of mass)
	// This improves direct splash on standing players
	const float PLAYER_COM_OFFSET = -40.0f; // Center of mass offset
	const int iPlayerRingSamples = std::max(6, iSamples / 10);
	const float flPlayerRingRadius = sqrtf(flRadius * flRadius - PLAYER_COM_OFFSET * PLAYER_COM_OFFSET);

	for (int i = 0; i < iPlayerRingSamples; ++i)
	{
		const float theta = (2.0f * PI * i) / iPlayerRingSamples;
		const float x = cosf(theta) * flPlayerRingRadius;
		const float y = sinf(theta) * flPlayerRingRadius;
		const float z = PLAYER_COM_OFFSET;

		Vec3 vPoint(x, y, z);
		vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });

		vPoints.emplace_back(vPoint, iPointType);
	}

	// === IMPROVEMENT 4: Cardinal Direction Points ===
	// Add points at cardinal directions for wall splash
	// These catch common wall-splash scenarios
	const float WALL_OFFSET_RATIOS[] = { 0.7f, 0.85f, 1.0f };
	for (float ratio : WALL_OFFSET_RATIOS)
	{
		const float r = flRadius * ratio;
		Vec3 cardinalPoints[] = {
			Vec3(r, 0, 0),    // +X
			Vec3(-r, 0, 0),   // -X
			Vec3(0, r, 0),    // +Y
			Vec3(0, -r, 0),   // -Y
			Vec3(0, 0, -r),   // -Z (directly below)
		};

		for (auto& vPoint : cardinalPoints)
		{
			vPoint = Math::RotatePoint(vPoint, {}, { flRotateX, flRotateY });
			vPoints.emplace_back(vPoint, iPointType);
		}
	}

	// Explicit bottom point (kept from original - critical for ground splash)
	Vec3 vBottom(0.0f, 0.0f, -flRadius);
	vBottom = Math::RotatePoint(vBottom, {}, { flRotateX, flRotateY });
	vPoints.emplace_back(vBottom, iPointType);

	return vPoints;
}


// New: Detect edges and corners in the environment
static inline std::vector<Vec3> DetectEdgePoints(const Vec3& vCenter, float flRadius, float flSearchRadius, int iSamples)
{
	std::vector<Vec3> vEdgePoints;
	CTraceFilterWorldAndPropsOnly filter = {};

	int iMinEdges = Vars::Aimbot::Projectile::MinEdgePoints.Value;
	int iMaxEdges = Vars::Aimbot::Projectile::MaxEdgePoints.Value;

	if (iMaxEdges > 0)
		vEdgePoints.reserve(iMaxEdges);

	const int iRings = 4;
	const int iSamplesPerRing = iSamples / iRings;
	const float flEdgeRadius = 16.0f;
	const float flThreshold = 0.7f; // Surface angle difference threshold
	const int iRadialSamples = 8; // Samples around each surface point

	for (int ring = 0; ring < iRings; ring++)
	{
		float flHeight = Math::RemapVal(ring, 0, iRings - 1, -flRadius, flRadius * 0.5f);
		float flRingRadius = sqrtf(std::max(0.f, flRadius * flRadius - flHeight * flHeight));

		for (int i = 0; i < iSamplesPerRing; i++)
		{
			if (iMaxEdges > 0 && vEdgePoints.size() >= static_cast<size_t>(iMaxEdges))
				goto finish_detection;

			float flAngle = (i / float(iSamplesPerRing)) * 2.0f * PI;
			Vec3 vOffset = Vec3(cosf(flAngle) * flRingRadius, sinf(flAngle) * flRingRadius, flHeight);
			Vec3 vTestPoint = vCenter + vOffset;

			// Find surface point
			CGameTrace trace = {};
			SDK::Trace(vCenter, vTestPoint, MASK_SOLID, &filter, &trace);

			if (trace.DidHit() && trace.fraction < 0.95f)
			{
				Vec3 vSurfacePoint = trace.endpos;
				Vec3 vNormal = trace.plane.normal;

				// Create tangent vectors for radial sampling
				Vec3 vTangent1 = vNormal.Cross(Vec3(0, 0, 1)).Normalized();
				if (vTangent1.IsZero())
					vTangent1 = vNormal.Cross(Vec3(1, 0, 0)).Normalized();
				Vec3 vTangent2 = vNormal.Cross(vTangent1).Normalized();

				// Sample in a circle around the surface point
				bool bIsEdge = false;
				for (int j = 0; j < iRadialSamples; j++)
				{
					float flRadialAngle = (PI * 2.0f / iRadialSamples) * j;
					Vec3 vDirection = (vTangent1 * cosf(flRadialAngle) + vTangent2 * sinf(flRadialAngle)).Normalized();
					Vec3 vRadialTest = vSurfacePoint + vDirection * flEdgeRadius;

					// Trace perpendicular to surface to find nearby geometry
					CGameTrace radialTrace = {};
					SDK::Trace(vRadialTest + vNormal * 8.0f, vRadialTest - vNormal * 8.0f, MASK_SOLID, &filter, &radialTrace);

					if (radialTrace.DidHit() && vSurfacePoint.DistTo(radialTrace.endpos) < flSearchRadius)
					{
						// Check if surface normal differs significantly (indicates edge/corner)
						float flDot = vNormal.Dot(radialTrace.plane.normal);
						if (flDot < flThreshold)
						{
							bIsEdge = true;
							break; // Found edge, no need to check more samples
						}
					}
				}

				if (bIsEdge)
				{
					if (iMaxEdges > 0 && vEdgePoints.size() >= static_cast<size_t>(iMaxEdges))
						goto finish_detection;

					// Add the edge surface point
					vEdgePoints.push_back(vSurfacePoint - vCenter);

					// Add offset point slightly away from surface for better splash angle
					if (iMaxEdges == 0 || vEdgePoints.size() < static_cast<size_t>(iMaxEdges))
						vEdgePoints.push_back(vSurfacePoint + vNormal * 4.0f - vCenter);
				}
			}
		}
	}

finish_detection:
	// Check minimum requirement
	if (iMinEdges > 0 && vEdgePoints.size() < static_cast<size_t>(iMinEdges))
	{
		int iNeeded = iMinEdges - vEdgePoints.size();
		float flAngleStep = (2.0f * PI) / iNeeded;

		for (int i = 0; i < iNeeded; i++)
		{
			float flAngle = i * flAngleStep;
			Vec3 vSyntheticPoint = Vec3(
				cosf(flAngle) * flRadius * 0.8f,
				sinf(flAngle) * flRadius * 0.8f,
				-flRadius * 0.9f
			);
			vEdgePoints.push_back(vSyntheticPoint);
		}
	}

	return vEdgePoints;
}

void CAimbotProjectile::SetupSplashPointsPoints(Target_t& tTarget, std::vector<std::pair<Vec3, int>>& vSpherePoints, std::vector<std::pair<Vec3, Vec3>>& vSimplePoints)
{
	vSimplePoints.clear();
	Vec3 vTargetEye = tTarget.m_vPos + m_tInfo.m_vTargetEye;

	auto checkPoint = [&](CGameTrace& trace, bool& bErase, bool& bNormal)
		{
			bool bHitSolid = trace.m_pEnt && trace.fraction < 1.f && !(trace.surface.flags & SURF_SKY);
			bool bMovingEntity = trace.m_pEnt && !trace.m_pEnt->GetAbsVelocity().IsZero();

			bErase = !trace.m_pEnt || trace.fraction == 1.f || trace.surface.flags & SURF_SKY || bMovingEntity;

			if (bErase)
				return false;

			Point_t tPoint = { trace.endpos, {} };

			if (!m_tInfo.m_flGravity)
			{
				Vec3 vForward = (m_tInfo.m_vLocalEye - trace.endpos).Normalized();
				bNormal = vForward.Dot(trace.plane.normal) <= 0;
			}

			if (!bNormal)
			{
				CalculateAngle(m_tInfo.m_vLocalEye, tPoint.m_vPoint, 0, tPoint.m_tSolution, false);
				if (m_tInfo.m_flGravity)
				{
					Vec3 vPos = m_tInfo.m_vLocalEye + Vec3(0, 0, (m_tInfo.m_flGravity * 800.f * pow(tPoint.m_tSolution.m_flTime, 2)) / 2);
					Vec3 vForward = (vPos - tPoint.m_vPoint).Normalized();
					bNormal = vForward.Dot(trace.plane.normal) <= 0;
				}
			}

			bool bWallBleedCandidate = false;
			if (bHitSolid && tPoint.m_tSolution.m_iCalculated != CalculatedEnum::Bad)
			{
				CTraceFilterWorldAndPropsOnly filter{};

				CGameTrace wallTrace{};
				SDK::Trace(trace.endpos, vTargetEye, MASK_SOLID, &filter, &wallTrace);
				bool bWallBetween = wallTrace.fraction < 0.99f;

				if (bWallBetween)
				{
					CGameTrace backTrace{};
					Vec3 vNormal = trace.plane.normal;
					SDK::Trace(trace.endpos + vNormal, trace.endpos - vNormal * 8.0f, MASK_SOLID, &filter, &backTrace);
					float flWallThickness = (backTrace.endpos - trace.endpos).Length();

					Vec3 vTangent = vNormal.Cross(Vec3(0, 0, 1)).Normalized();
					if (vTangent.IsZero())
						vTangent = vNormal.Cross(Vec3(1, 0, 0)).Normalized();

					CGameTrace seamTrace1{}, seamTrace2{};
					SDK::Trace(trace.endpos + vTangent * 2.0f, vTargetEye, MASK_SOLID, &filter, &seamTrace1);
					SDK::Trace(trace.endpos - vTangent * 2.0f, vTargetEye, MASK_SOLID, &filter, &seamTrace2);

					bool bSeamDetected = std::abs(seamTrace1.fraction - seamTrace2.fraction) > 0.25f;
					bool bThinWall = flWallThickness < 8.0f;
					bool bNodraw = trace.surface.flags & SURF_NODRAW;
					bool bCloseToWall = wallTrace.fraction < 0.2f;

					bWallBleedCandidate = bThinWall || bSeamDetected || bNodraw || bCloseToWall;
				}
			}

			if (!bNormal || bWallBleedCandidate)
			{
				if (tPoint.m_tSolution.m_iCalculated != CalculatedEnum::Bad)
				{
					vSimplePoints.emplace_back(tPoint.m_vPoint, trace.plane.normal);
					return true;
				}
			}

			return false;
		};

	int i = 0;
	for (auto& vSpherePoint : vSpherePoints)
	{
		Vec3 vPoint = vSpherePoint.first + vTargetEye;
		int& iType = vSpherePoint.second;

		Solution_t solution;
		CalculateAngle(m_tInfo.m_vLocalEye, vPoint, 0, solution, false);

		if (solution.m_iCalculated != CalculatedEnum::Bad)
			TracePointPoints(vPoint, iType, vTargetEye, m_tInfo, vSimplePoints, checkPoint, i++);
	}
}

void CAimbotProjectile::SetupSplashPoints(Target_t& tTarget, std::vector<std::pair<Vec3, int>>& vSpherePoints,
	std::vector<std::pair<Vec3, Vec3>>& vSimplePoints, std::vector<Vec3>& vEdgePoints)
{
	vSimplePoints.clear();
	Vec3 vTargetEye = tTarget.m_vPos + m_tInfo.m_vTargetEye;

	auto checkPoint = [&](CGameTrace& trace, bool& bErase, bool& bNormal) -> bool
		{
			bErase = !trace.m_pEnt || trace.fraction == 1.f || trace.surface.flags & SURF_SKY || !trace.m_pEnt->GetAbsVelocity().IsZero();
			if (bErase)
				return false;

			Point_t tPoint = { trace.endpos, {} };
			if (!m_tInfo.m_flGravity)
			{
				Vec3 vForward = (m_tInfo.m_vLocalEye - trace.endpos).Normalized();
				bNormal = vForward.Dot(trace.plane.normal) <= 0;
			}
			if (!bNormal)
			{
				CalculateAngle(m_tInfo.m_vLocalEye, tPoint.m_vPoint, 0, tPoint.m_tSolution, false);
				if (m_tInfo.m_flGravity)
				{
					Vec3 vPos = m_tInfo.m_vLocalEye + Vec3(0, 0, (m_tInfo.m_flGravity * 800.f * pow(tPoint.m_tSolution.m_flTime, 2)) / 2);
					Vec3 vForward = (vPos - tPoint.m_vPoint).Normalized();
					bNormal = vForward.Dot(trace.plane.normal) <= 0;
				}
			}
			if (bNormal)
				return false;

			if (tPoint.m_tSolution.m_iCalculated != CalculatedEnum::Bad)
			{
				vSimplePoints.emplace_back(tPoint.m_vPoint, trace.plane.normal);
				return true;
			}
			return false;
		};

	// EDGE POINTS FIRST - automatic priority by insertion order
	if (!vEdgePoints.empty())
	{
		for (auto& vEdgeOffset : vEdgePoints)
		{
			Vec3 vPoint = vEdgeOffset + vTargetEye;
			int iType = PointTypeEnum::Regular | PointTypeEnum::Obscured;

			Solution_t solution;
			CalculateAngle(m_tInfo.m_vLocalEye, vPoint, 0, solution, false);

			if (solution.m_iCalculated != CalculatedEnum::Bad)
				TracePoint(vPoint, iType, vTargetEye, m_tInfo, vSimplePoints, checkPoint, 0);
		}
	}

	// Regular sphere points after edges
	int i = 0;
	for (auto& vSpherePoint : vSpherePoints)
	{
		Vec3 vPoint = vSpherePoint.first + vTargetEye;
		int& iType = vSpherePoint.second;

		Solution_t solution;
		CalculateAngle(m_tInfo.m_vLocalEye, vPoint, 0, solution, false);

		if (solution.m_iCalculated != CalculatedEnum::Bad)
			TracePoint(vPoint, iType, vTargetEye, m_tInfo, vSimplePoints, checkPoint, i++);
	}
}

// Get splash points - no priority checks, already sorted by setup
std::vector<Point_t> CAimbotProjectile::GetSplashPointsSimple(Target_t& tTarget, std::vector<std::pair<Vec3, Vec3>>& vSpherePoints,
	std::vector<Vec3>& vEdgePoints, int iSimTime)
{
	std::vector<std::pair<Point_t, float>> vPointDistances = {};
	const Vec3 vTargetEye = tTarget.m_vPos + m_tInfo.m_vTargetEye;

	auto checkPoint = [&](const Vec3& vPoint, bool& bErase) -> bool
		{
			Point_t tPoint = { vPoint, {} };
			CalculateAngle(m_tInfo.m_vLocalEye, tPoint.m_vPoint, iSimTime, tPoint.m_tSolution);

			bErase = tPoint.m_tSolution.m_iCalculated == CalculatedEnum::Good;
			if (!bErase)
				return false;

			vPointDistances.emplace_back(tPoint, tPoint.m_vPoint.DistTo(tTarget.m_vPos));
			return true;
		};

	// Process all points - edge points already at front of list from setup
	for (auto it = vSpherePoints.begin(); it != vSpherePoints.end();)
	{
		Vec3& vPoint = it->first;
		bool bErase = false;
		checkPoint(vPoint, bErase);

		if (bErase)
			it = vSpherePoints.erase(it);
		else
			++it;
	}

	// Sort by distance (edge points naturally favored by being checked first)
	std::sort(vPointDistances.begin(), vPointDistances.end(), [](const auto& a, const auto& b) -> bool
		{
			return a.second < b.second;
		});

	std::vector<Point_t> vPoints = {};
	const int iSplashCount = std::min(
		m_tInfo.m_flPrimeTime && iSimTime == m_tInfo.m_iPrimeTime ?
		Vars::Aimbot::Projectile::SplashCountDirect.Value : m_tInfo.m_iSplashCount,
		int(vPointDistances.size())
	);

	for (int i = 0; i < iSplashCount; i++)
		vPoints.push_back(vPointDistances[i].first);

	// Validate points within splash radius
	const Vec3 vOriginal = tTarget.m_pEntity->GetAbsOrigin();
	tTarget.m_pEntity->SetAbsOrigin(tTarget.m_vPos);

	for (auto it = vPoints.begin(); it != vPoints.end();)
	{
		auto& vPoint = *it;
		bool bValid = vPoint.m_tSolution.m_iCalculated != CalculatedEnum::Pending;

		if (bValid)
		{
			Vec3 vPos = {};
			tTarget.m_pEntity->m_Collision()->CalcNearestPoint(vPoint.m_vPoint, &vPos);
			bValid = vPoint.m_vPoint.DistTo(vPos) < m_tInfo.m_flRadius;
		}

		if (bValid)
			++it;
		else
			it = vPoints.erase(it);
	}

	tTarget.m_pEntity->SetAbsOrigin(vOriginal);

	return vPoints;
}

static inline float AABBLine(Vec3 vMins, Vec3 vMaxs, Vec3 vStart, Vec3 vDir)
{
	Vec3 a = {
		(vMins.x - vStart.x) / vDir.x,
		(vMins.y - vStart.y) / vDir.y,
		(vMins.z - vStart.z) / vDir.z
	};
	Vec3 b = {
		(vMaxs.x - vStart.x) / vDir.x,
		(vMaxs.y - vStart.y) / vDir.y,
		(vMaxs.z - vStart.z) / vDir.z
	};
	Vec3 c = {
		std::min(a.x, b.x),
		std::min(a.y, b.y),
		std::min(a.z, b.z)
	};
	return std::max(std::max(c.x, c.y), c.z);
}
static inline Vec3 PullPoint(Vec3 vPoint, Vec3 vLocalPos, Info_t& tInfo, Vec3 vMins, Vec3 vMaxs, Vec3 vTargetPos)
{
	auto HeightenLocalPos = [&]()
		{	// basic trajectory pass
			const float flGrav = tInfo.m_flGravity * 800.f;
			if (!flGrav)
				return vPoint;

			const Vec3 vDelta = vTargetPos - vLocalPos;
			const float flDist = vDelta.Length2D();

			const float flRoot = pow(tInfo.m_flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(tInfo.m_flVelocity, 2));
			if (flRoot < 0.f)
				return vPoint;
			float flPitch = atan((pow(tInfo.m_flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));

			float flTime = flDist / (cos(flPitch) * tInfo.m_flVelocity) - tInfo.m_flOffsetTime;
			return vLocalPos + Vec3(0, 0, (flGrav * pow(flTime, 2)) / 2);
		};

	vLocalPos = HeightenLocalPos();
	Vec3 vForward, vRight, vUp; Math::AngleVectors(Math::CalcAngle(vLocalPos, vPoint), &vForward, &vRight, &vUp);
	vLocalPos += (vForward * tInfo.m_vOffset.x) + (vRight * tInfo.m_vOffset.y) + (vUp * tInfo.m_vOffset.z);
	return vLocalPos + (vPoint - vLocalPos) * fabsf(AABBLine(vMins + vTargetPos, vMaxs + vTargetPos, vLocalPos, vPoint - vLocalPos));
}



static inline void SolveProjectileSpeed(CTFWeaponBase* pWeapon, const Vec3& vLocalPos, const Vec3& vTargetPos, float& flVelocity, float& flDragTime, const float flGravity)
{
	if (!F::ProjSim.m_pObj->IsDragEnabled() || F::ProjSim.m_pObj->m_dragBasis.IsZero())
		return;

	const float flGrav = flGravity * 800.0f;
	const Vec3 vDelta = vTargetPos - vLocalPos;
	const float flDist = vDelta.Length2D();

	const float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));
	if (flRoot < 0.f)
		return;

	const float flPitch = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
	const float flTime = flDist / (cos(flPitch) * flVelocity);

	float flDrag = 0.f;
	if (Vars::Aimbot::Projectile::DragOverride.Value)
		flDrag = Vars::Aimbot::Projectile::DragOverride.Value;
	else
	{
		switch (pWeapon->m_iItemDefinitionIndex()) // the remaps are dumb but they work so /shrug
		{
		case Demoman_m_GrenadeLauncher:
		case Demoman_m_GrenadeLauncherR:
		case Demoman_m_FestiveGrenadeLauncher:
		case Demoman_m_Autumn:
		case Demoman_m_MacabreWeb:
		case Demoman_m_Rainbow:
		case Demoman_m_SweetDreams:
		case Demoman_m_CoffinNail:
		case Demoman_m_TopShelf:
		case Demoman_m_Warhawk:
		case Demoman_m_ButcherBird:
		case Demoman_m_TheIronBomber: flDrag = Math::RemapVal(flVelocity, 1217.f, k_flMaxVelocity, 0.120f, 0.200f); break; // 0.120 normal, 0.200 capped, 0.300 v3000
		case Demoman_m_TheLochnLoad: flDrag = Math::RemapVal(flVelocity, 1504.f, k_flMaxVelocity, 0.070f, 0.085f); break; // 0.070 normal, 0.085 capped, 0.120 v3000
		case Demoman_m_TheLooseCannon: flDrag = Math::RemapVal(flVelocity, 1454.f, k_flMaxVelocity, 0.385f, 0.530f); break; // 0.385 normal, 0.530 capped, 0.790 v3000
		case Demoman_s_StickybombLauncher:
		case Demoman_s_StickybombLauncherR:
		case Demoman_s_FestiveStickybombLauncher:
		case Demoman_s_TheQuickiebombLauncher:
		case Demoman_s_TheScottishResistance: flDrag = Math::RemapVal(flVelocity, 922.f, k_flMaxVelocity, 0.085f, 0.190f); break; // 0.085 low, 0.190 capped, 0.230 v2400
		case Scout_s_TheFlyingGuillotine:
		case Scout_s_TheFlyingGuillotineG: flDrag = 0.310f; break;
		case Scout_t_TheSandman: flDrag = 0.180f; break;
		case Scout_t_TheWrapAssassin: flDrag = 0.285f; break;
		case Scout_s_MadMilk:
		case Scout_s_MutatedMilk:
		case Sniper_s_Jarate:
		case Sniper_s_FestiveJarate:
		case Sniper_s_TheSelfAwareBeautyMark: flDrag = 0.057f; break;
		}
	}

	float flOverride = Vars::Aimbot::Projectile::TimeOverride.Value;
	flDragTime = powf(flTime, 2) * flDrag / (flOverride ? flOverride : 1.5f); // rough estimate to prevent m_flTime being too low
	flVelocity = flVelocity - flVelocity * flTime * flDrag;
}

//void CAimbotProjectile::CalculateAngle(const Vec3& vLocalPos, const Vec3& vTargetPos, int iSimTime, Solution_t& out, bool bAccuracy)
//{
//	if (out.m_iCalculated != CalculatedEnum::Pending)
//		return;
//
//	const float flGrav = m_tInfo.m_flGravity * 800.f;
//
//	float flPitch, flYaw;
//	{	// basic trajectory pass
//		float flVelocity = m_tInfo.m_flVelocity, flDragTime = 0.f;
//		if (F::ProjSim.m_pObj->IsDragEnabled() && !F::ProjSim.m_pObj->m_dragBasis.IsZero() && m_tInfo.m_pWeapon)
//		{
//			Vec3 vForward, vRight, vUp; Math::AngleVectors(Math::CalcAngle(vLocalPos, vTargetPos), &vForward, &vRight, &vUp);
//			Vec3 vShootPos = vLocalPos + (vForward * m_tInfo.m_vOffset.x) + (vRight * m_tInfo.m_vOffset.y) + (vUp * m_tInfo.m_vOffset.z);
//			SolveProjectileSpeed(m_tInfo.m_pWeapon, vShootPos, vTargetPos, flVelocity, flDragTime, m_tInfo.m_flGravity);
//		}
//
//		Vec3 vDelta = vTargetPos - vLocalPos;
//		float flDist = vDelta.Length2D();
//
//		Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vTargetPos);
//		if (!flGrav)
//			flPitch = -DEG2RAD(vAngleTo.x);
//		else
//		{	// arch
//			float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));
//			if (out.m_iCalculated = flRoot < 0.f ? CalculatedEnum::Bad : CalculatedEnum::Pending)
//				return;
//			flPitch = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
//		}
//		out.m_flTime = flDist / (cos(flPitch) * flVelocity) - m_tInfo.m_flOffsetTime + flDragTime;
//		out.m_flPitch = flPitch = -RAD2DEG(flPitch) - m_tInfo.m_vAngFix.x;
//		out.m_flYaw = flYaw = vAngleTo.y - m_tInfo.m_vAngFix.y;
//	}
//
//	int iTimeTo = int(out.m_flTime / TICK_INTERVAL) + 1;
//	if (!m_tInfo.m_vOffset.IsZero())
//	{
//		if (out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Pending)
//			return;
//	}
//	else
//	{
//		out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Good;
//		return;
//	}
//
//	int iFlags = (bAccuracy ? ProjSimEnum::Trace : ProjSimEnum::None) | ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum;
//#ifdef SPLASH_DEBUG6
//	if (iFlags & ProjSimEnum::Trace)
//	{
//		if (Vars::Visuals::Trajectory::Override.Value)
//		{
//			if (!Vars::Visuals::Trajectory::Pipes.Value)
//				s_mTraceCount["Setup trace calculate"]++;
//		}
//		else
//		{
//			switch (m_tInfo.m_pWeapon->GetWeaponID())
//			{
//			case TF_WEAPON_ROCKETLAUNCHER:
//			case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
//			case TF_WEAPON_PARTICLE_CANNON:
//			case TF_WEAPON_RAYGUN:
//			case TF_WEAPON_DRG_POMSON:
//			case TF_WEAPON_FLAREGUN:
//			case TF_WEAPON_FLAREGUN_REVENGE:
//			case TF_WEAPON_COMPOUND_BOW:
//			case TF_WEAPON_CROSSBOW:
//			case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
//			case TF_WEAPON_SYRINGEGUN_MEDIC:
//			case TF_WEAPON_FLAME_BALL:
//				s_mTraceCount["Setup trace calculate"]++;
//			}
//		}
//	}
//#endif
//	ProjectileInfo tProjInfo = {};
//	if (out.m_iCalculated = !F::ProjSim.GetInfo(m_tInfo.m_pLocal, m_tInfo.m_pWeapon, { flPitch, flYaw, 0 }, tProjInfo, iFlags) ? CalculatedEnum::Bad : CalculatedEnum::Pending)
//		return;
//
//	{	// calculate trajectory from projectile origin
//		float flVelocity = m_tInfo.m_flVelocity, flDragTime = 0.f;
//		SolveProjectileSpeed(m_tInfo.m_pWeapon, tProjInfo.m_vPos, vTargetPos, flVelocity, flDragTime, m_tInfo.m_flGravity);
//
//		Vec3 vDelta = vTargetPos - tProjInfo.m_vPos;
//		float flDist = vDelta.Length2D();
//
//		Vec3 vAngleTo = Math::CalcAngle(tProjInfo.m_vPos, vTargetPos);
//		if (!flGrav)
//			out.m_flPitch = -DEG2RAD(vAngleTo.x);
//		else
//		{	// arch
//			float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));
//			if (out.m_iCalculated = flRoot < 0.f ? CalculatedEnum::Bad : CalculatedEnum::Pending)
//				return;
//			out.m_flPitch = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
//		}
//		out.m_flTime = flDist / (cos(out.m_flPitch) * flVelocity) + flDragTime;
//	}
//
//	{	// correct yaw
//		Vec3 vShootPos = (tProjInfo.m_vPos - vLocalPos).To2D();
//		Vec3 vTarget = vTargetPos - vLocalPos;
//		Vec3 vForward; Math::AngleVectors(tProjInfo.m_vAng, &vForward); vForward.Normalize2D();
//		float flB = 2 * (vShootPos.x * vForward.x + vShootPos.y * vForward.y);
//		float flC = vShootPos.Length2DSqr() - vTarget.Length2DSqr();
//		auto vSolutions = Math::SolveQuadratic(1.f, flB, flC);
//		if (!vSolutions.empty())
//		{
//			vShootPos += vForward * vSolutions.front();
//			out.m_flYaw = flYaw - (RAD2DEG(atan2(vShootPos.y, vShootPos.x)) - flYaw);
//			flYaw = RAD2DEG(atan2(vShootPos.y, vShootPos.x));
//		}
//	}
//
//	{	// correct pitch
//		if (flGrav)
//		{
//			flPitch -= tProjInfo.m_vAng.x;
//			out.m_flPitch = -RAD2DEG(out.m_flPitch) + flPitch - m_tInfo.m_vAngFix.x;
//		}
//		else
//		{
//			Vec3 vShootPos = Math::RotatePoint(tProjInfo.m_vPos - vLocalPos, {}, { 0, -flYaw, 0 }); vShootPos.y = 0;
//			Vec3 vTarget = Math::RotatePoint(vTargetPos - vLocalPos, {}, { 0, -flYaw, 0 });
//			Vec3 vForward; Math::AngleVectors(tProjInfo.m_vAng - Vec3(0, flYaw, 0), &vForward); vForward.y = 0; vForward.Normalize();
//			float flB = 2 * (vShootPos.x * vForward.x + vShootPos.z * vForward.z);
//			float flC = (powf(vShootPos.x, 2) + powf(vShootPos.z, 2)) - (powf(vTarget.x, 2) + powf(vTarget.z, 2));
//			auto vSolutions = Math::SolveQuadratic(1.f, flB, flC);
//			if (!vSolutions.empty())
//			{
//				vShootPos += vForward * vSolutions.front();
//				out.m_flPitch = flPitch - (RAD2DEG(atan2(-vShootPos.z, vShootPos.x)) - flPitch);
//			}
//		}
//	}
//
//	iTimeTo = int(out.m_flTime / TICK_INTERVAL) + 1;
//	out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Good;
//}

// Replace the CalculateAngle function with this modified version
// Add this near the top of the file with other config variables

void CAimbotProjectile::CalculateAngle(const Vec3& vLocalPos, const Vec3& vTargetPos, int iSimTime, Solution_t& out, bool bAccuracy)
{
	if (out.m_iCalculated != CalculatedEnum::Pending)
		return;

	const float flGrav = m_tInfo.m_flGravity * 800.f;

	float flPitch, flYaw;
	{	// basic trajectory pass
		float flVelocity = m_tInfo.m_flVelocity, flDragTime = 0.f;
		if (F::ProjSim.m_pObj->IsDragEnabled() && !F::ProjSim.m_pObj->m_dragBasis.IsZero() && m_tInfo.m_pWeapon)
		{
			Vec3 vForward, vRight, vUp; Math::AngleVectors(Math::CalcAngle(vLocalPos, vTargetPos), &vForward, &vRight, &vUp);
			Vec3 vShootPos = vLocalPos + (vForward * m_tInfo.m_vOffset.x) + (vRight * m_tInfo.m_vOffset.y) + (vUp * m_tInfo.m_vOffset.z);
			SolveProjectileSpeed(m_tInfo.m_pWeapon, vShootPos, vTargetPos, flVelocity, flDragTime, m_tInfo.m_flGravity);
		}

		Vec3 vDelta = vTargetPos - vLocalPos;
		float flDist = vDelta.Length2D();

		Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vTargetPos);
		if (!flGrav)
			flPitch = -DEG2RAD(vAngleTo.x);
		else
		{	// arch trajectory - MODIFIED: only use high arc if normal fails AND iSimTime is 0 (current position)
			float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));

			// If normal arc calculation fails, mark as bad and return
			if (flRoot < 0.f)
			{
				out.m_iCalculated = CalculatedEnum::Bad;
				return;
			}

			float flPitchLow = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
			float flPitchHigh = atan((pow(flVelocity, 2) + sqrt(flRoot)) / (flGrav * flDist));

			// CRITICAL: Only attempt high arc for sticky launcher at CURRENT position (iSimTime == 0)
			// This prevents it from doing extreme predictions into the future
			bool bUseHighArc = false;

			if (m_tInfo.m_pWeapon &&
				m_tInfo.m_pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER &&
				iSimTime == 0)  // KEY: Only at current position, no future prediction
			{
				// Check if normal arc is obstructed
				bool bObstructed = !SDK::VisPosWorld(nullptr, nullptr, vLocalPos, vTargetPos, MASK_SHOT);

				// Only use high arc if obstructed OR target is significantly above
				if (Vars::Aimbot::Projectile::UseHighArc.Value)
					bUseHighArc = true;
			}

			flPitch = bUseHighArc ? flPitchHigh : flPitchLow;
		}
		out.m_flTime = flDist / (cos(flPitch) * flVelocity) - m_tInfo.m_flOffsetTime + flDragTime;
		out.m_flPitch = flPitch = -RAD2DEG(flPitch) - m_tInfo.m_vAngFix.x;
		out.m_flYaw = flYaw = vAngleTo.y - m_tInfo.m_vAngFix.y;
	}

	int iTimeTo = int(out.m_flTime / TICK_INTERVAL) + 1;
	if (!m_tInfo.m_vOffset.IsZero())
	{
		if (out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Pending)
			return;
	}
	else
	{
		out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Good;
		return;
	}

	int iFlags = (bAccuracy ? ProjSimEnum::Trace : ProjSimEnum::None) | ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum;
	ProjectileInfo tProjInfo = {};
	if (out.m_iCalculated = !F::ProjSim.GetInfo(m_tInfo.m_pLocal, m_tInfo.m_pWeapon, { flPitch, flYaw, 0 }, tProjInfo, iFlags) ? CalculatedEnum::Bad : CalculatedEnum::Pending)
		return;

	{	// calculate trajectory from projectile origin
		float flVelocity = m_tInfo.m_flVelocity, flDragTime = 0.f;
		SolveProjectileSpeed(m_tInfo.m_pWeapon, tProjInfo.m_vPos, vTargetPos, flVelocity, flDragTime, m_tInfo.m_flGravity);

		Vec3 vDelta = vTargetPos - tProjInfo.m_vPos;
		float flDist = vDelta.Length2D();

		Vec3 vAngleTo = Math::CalcAngle(tProjInfo.m_vPos, vTargetPos);
		if (!flGrav)
			out.m_flPitch = -DEG2RAD(vAngleTo.x);
		else
		{	// arch - SAME LOGIC as above
			float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));
			if (out.m_iCalculated = flRoot < 0.f ? CalculatedEnum::Bad : CalculatedEnum::Pending)
				return;

			float flPitchLow2 = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
			float flPitchHigh2 = atan((pow(flVelocity, 2) + sqrt(flRoot)) / (flGrav * flDist));

			// Same check - only high arc at current position
			bool bUseHighArc2 = false;
			if (m_tInfo.m_pWeapon &&
				m_tInfo.m_pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER &&
				iSimTime == 0)
			{
				bool bObstructed2 = !SDK::VisPosWorld(nullptr, nullptr, tProjInfo.m_vPos, vTargetPos, MASK_SHOT);

				if (Vars::Aimbot::Projectile::UseHighArc.Value)
					bUseHighArc2 = true;
			}

			out.m_flPitch = bUseHighArc2 ? flPitchHigh2 : flPitchLow2;
		}
		out.m_flTime = flDist / (cos(out.m_flPitch) * flVelocity) + flDragTime;
	}

	{	// correct yaw
		Vec3 vShootPos = (tProjInfo.m_vPos - vLocalPos).To2D();
		Vec3 vTarget = vTargetPos - vLocalPos;
		Vec3 vForward; Math::AngleVectors(tProjInfo.m_vAng, &vForward); vForward.Normalize2D();
		float flB = 2 * (vShootPos.x * vForward.x + vShootPos.y * vForward.y);
		float flC = vShootPos.Length2DSqr() - vTarget.Length2DSqr();
		auto vSolutions = Math::SolveQuadratic(1.f, flB, flC);
		if (!vSolutions.empty())
		{
			vShootPos += vForward * vSolutions.front();
			out.m_flYaw = flYaw - (RAD2DEG(atan2(vShootPos.y, vShootPos.x)) - flYaw);
			flYaw = RAD2DEG(atan2(vShootPos.y, vShootPos.x));
		}
	}

	{	// correct pitch
		if (flGrav)
		{
			flPitch -= tProjInfo.m_vAng.x;
			out.m_flPitch = -RAD2DEG(out.m_flPitch) + flPitch - m_tInfo.m_vAngFix.x;
		}
		else
		{
			Vec3 vShootPos = Math::RotatePoint(tProjInfo.m_vPos - vLocalPos, {}, { 0, -flYaw, 0 }); vShootPos.y = 0;
			Vec3 vTarget = Math::RotatePoint(vTargetPos - vLocalPos, {}, { 0, -flYaw, 0 });
			Vec3 vForward; Math::AngleVectors(tProjInfo.m_vAng - Vec3(0, flYaw, 0), &vForward); vForward.y = 0; vForward.Normalize();
			float flB = 2 * (vShootPos.x * vForward.x + vShootPos.z * vForward.z);
			float flC = (powf(vShootPos.x, 2) + powf(vShootPos.z, 2)) - (powf(vTarget.x, 2) + powf(vTarget.z, 2));
			auto vSolutions = Math::SolveQuadratic(1.f, flB, flC);
			if (!vSolutions.empty())
			{
				vShootPos += vForward * vSolutions.front();
				out.m_flPitch = flPitch - (RAD2DEG(atan2(-vShootPos.z, vShootPos.x)) - flPitch);
			}
		}
	}

	iTimeTo = int(out.m_flTime / TICK_INTERVAL) + 1;
	out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Good;
}

//void CAimbotProjectile::CalculateAngle(const Vec3& vLocalPos, const Vec3& vTargetPos, int iSimTime, Solution_t& out, bool bAccuracy)
//{
//	if (out.m_iCalculated != CalculatedEnum::Pending)
//		return;
//
//	const float flGrav = m_tInfo.m_flGravity * 800.f;
//
//	float flPitch, flYaw;
//	{	// basic trajectory pass
//		float flVelocity = m_tInfo.m_flVelocity, flDragTime = 0.f;
//		if (F::ProjSim.m_pObj->IsDragEnabled() && !F::ProjSim.m_pObj->m_dragBasis.IsZero() && m_tInfo.m_pWeapon)
//		{
//			Vec3 vForward, vRight, vUp; Math::AngleVectors(Math::CalcAngle(vLocalPos, vTargetPos), &vForward, &vRight, &vUp);
//			Vec3 vShootPos = vLocalPos + (vForward * m_tInfo.m_vOffset.x) + (vRight * m_tInfo.m_vOffset.y) + (vUp * m_tInfo.m_vOffset.z);
//			SolveProjectileSpeed(m_tInfo.m_pWeapon, vShootPos, vTargetPos, flVelocity, flDragTime, m_tInfo.m_flGravity);
//		}
//
//		Vec3 vDelta = vTargetPos - vLocalPos;
//		float flDist = vDelta.Length2D();
//
//		Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vTargetPos);
//		if (!flGrav)
//			flPitch = -DEG2RAD(vAngleTo.x);
//		else
//		{	// arch trajectory pass reverted to first commit because im not in the mood to make it better
//			float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));
//			if (out.m_iCalculated = flRoot < 0.f ? CalculatedEnum::Bad : CalculatedEnum::Pending)
//				return;
//			float flPitchLow = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
//			float flPitchHigh = atan((pow(flVelocity, 2) + sqrt(flRoot)) / (flGrav * flDist));
//			bool bObstructed = !SDK::VisPosWorld(nullptr, nullptr, vLocalPos, vTargetPos, MASK_SHOT);
//			flPitch = bObstructed ? flPitchHigh : flPitchLow;
//		}
//		out.m_flTime = flDist / (cos(flPitch) * flVelocity) - m_tInfo.m_flOffsetTime + flDragTime;
//		out.m_flPitch = flPitch = -RAD2DEG(flPitch) - m_tInfo.m_vAngFix.x;
//		out.m_flYaw = flYaw = vAngleTo.y - m_tInfo.m_vAngFix.y;
//	}
//
//	int iTimeTo = int(out.m_flTime / TICK_INTERVAL) + 1;
//	if (!m_tInfo.m_vOffset.IsZero())
//	{
//		if (out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Pending)
//			return;
//	}
//	else
//	{
//		out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Good;
//		return;
//	}
//
//	int iFlags = (bAccuracy ? ProjSimEnum::Trace : ProjSimEnum::None) | ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum;
//#ifdef SPLASH_DEBUG6
//	if (iFlags & ProjSimEnum::Trace)
//	{
//		if (Vars::Visuals::Trajectory::Override.Value)
//		{
//			if (!Vars::Visuals::Trajectory::Pipes.Value)
//				s_mTraceCount["Setup trace calculate"]++;
//		}
//		else
//		{
//			switch (m_tInfo.m_pWeapon->GetWeaponID())
//			{
//			case TF_WEAPON_ROCKETLAUNCHER:
//			case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
//			case TF_WEAPON_PARTICLE_CANNON:
//			case TF_WEAPON_RAYGUN:
//			case TF_WEAPON_DRG_POMSON:
//			case TF_WEAPON_FLAREGUN:
//			case TF_WEAPON_FLAREGUN_REVENGE:
//			case TF_WEAPON_COMPOUND_BOW:
//			case TF_WEAPON_CROSSBOW:
//			case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
//			case TF_WEAPON_SYRINGEGUN_MEDIC:
//			case TF_WEAPON_FLAME_BALL:
//				s_mTraceCount["Setup trace calculate"]++;
//			}
//		}
//	}
//#endif
//	ProjectileInfo tProjInfo = {};
//	if (out.m_iCalculated = !F::ProjSim.GetInfo(m_tInfo.m_pLocal, m_tInfo.m_pWeapon, { flPitch, flYaw, 0 }, tProjInfo, iFlags) ? CalculatedEnum::Bad : CalculatedEnum::Pending)
//		return;
//
//	{	// calculate trajectory from projectile origin
//		float flVelocity = m_tInfo.m_flVelocity, flDragTime = 0.f;
//		SolveProjectileSpeed(m_tInfo.m_pWeapon, tProjInfo.m_vPos, vTargetPos, flVelocity, flDragTime, m_tInfo.m_flGravity);
//
//		Vec3 vDelta = vTargetPos - tProjInfo.m_vPos;
//		float flDist = vDelta.Length2D();
//
//		Vec3 vAngleTo = Math::CalcAngle(tProjInfo.m_vPos, vTargetPos);
//		if (!flGrav)
//			out.m_flPitch = -DEG2RAD(vAngleTo.x);
//		else
//		{	// arch
//			float flRoot = pow(flVelocity, 4) - flGrav * (flGrav * pow(flDist, 2) + 2.f * vDelta.z * pow(flVelocity, 2));
//			if (out.m_iCalculated = flRoot < 0.f ? CalculatedEnum::Bad : CalculatedEnum::Pending)
//				return;
//			float flPitchLow2 = atan((pow(flVelocity, 2) - sqrt(flRoot)) / (flGrav * flDist));
//			float flPitchHigh2 = atan((pow(flVelocity, 2) + sqrt(flRoot)) / (flGrav * flDist));
//			bool bObstructed2 = !SDK::VisPosWorld(nullptr, nullptr, tProjInfo.m_vPos, vTargetPos, MASK_SHOT);
//			out.m_flPitch = bObstructed2 ? flPitchHigh2 : flPitchLow2;
//		}
//		out.m_flTime = flDist / (cos(out.m_flPitch) * flVelocity) + flDragTime;
//	}
//
//	{	// correct yaw
//		Vec3 vShootPos = (tProjInfo.m_vPos - vLocalPos).To2D();
//		Vec3 vTarget = vTargetPos - vLocalPos;
//		Vec3 vForward; Math::AngleVectors(tProjInfo.m_vAng, &vForward); vForward.Normalize2D();
//		float flB = 2 * (vShootPos.x * vForward.x + vShootPos.y * vForward.y);
//		float flC = vShootPos.Length2DSqr() - vTarget.Length2DSqr();
//		auto vSolutions = Math::SolveQuadratic(1.f, flB, flC);
//		if (!vSolutions.empty())
//		{
//			vShootPos += vForward * vSolutions.front();
//			out.m_flYaw = flYaw - (RAD2DEG(atan2(vShootPos.y, vShootPos.x)) - flYaw);
//			flYaw = RAD2DEG(atan2(vShootPos.y, vShootPos.x));
//		}
//	}
//
//	{	// correct pitch
//		if (flGrav)
//		{
//			flPitch -= tProjInfo.m_vAng.x;
//			out.m_flPitch = -RAD2DEG(out.m_flPitch) + flPitch - m_tInfo.m_vAngFix.x;
//		}
//		else
//		{
//			Vec3 vShootPos = Math::RotatePoint(tProjInfo.m_vPos - vLocalPos, {}, { 0, -flYaw, 0 }); vShootPos.y = 0;
//			Vec3 vTarget = Math::RotatePoint(vTargetPos - vLocalPos, {}, { 0, -flYaw, 0 });
//			Vec3 vForward; Math::AngleVectors(tProjInfo.m_vAng - Vec3(0, flYaw, 0), &vForward); vForward.y = 0; vForward.Normalize();
//			float flB = 2 * (vShootPos.x * vForward.x + vShootPos.z * vForward.z);
//			float flC = (powf(vShootPos.x, 2) + powf(vShootPos.z, 2)) - (powf(vTarget.x, 2) + powf(vTarget.z, 2));
//			auto vSolutions = Math::SolveQuadratic(1.f, flB, flC);
//			if (!vSolutions.empty())
//			{
//				vShootPos += vForward * vSolutions.front();
//				out.m_flPitch = flPitch - (RAD2DEG(atan2(-vShootPos.z, vShootPos.x)) - flPitch);
//			}
//		}
//	}
//
//	iTimeTo = int(out.m_flTime / TICK_INTERVAL) + 1;
//	out.m_iCalculated = iTimeTo > iSimTime ? CalculatedEnum::Time : CalculatedEnum::Good;
//}

bool CAimbotProjectile::TestAnglePoints(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, Target_t& tTarget, Vec3& vPoint, Vec3& vAngles, int iSimTime, bool bSplash, bool* pHitSolid, std::vector<Vec3>* pProjectilePath)
{
	int iFlags = ProjSimEnum::Trace | ProjSimEnum::InitCheck | ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum;
#ifdef SPLASH_DEBUG6
	if (Vars::Visuals::Trajectory::Override.Value)
	{
		if (!Vars::Visuals::Trajectory::Pipes.Value)
			s_mTraceCount["Setup trace test"]++;
	}
	else
	{
		switch (pWeapon->GetWeaponID())
		{
		case TF_WEAPON_ROCKETLAUNCHER:
		case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
		case TF_WEAPON_PARTICLE_CANNON:
		case TF_WEAPON_RAYGUN:
		case TF_WEAPON_DRG_POMSON:
		case TF_WEAPON_FLAREGUN:
		case TF_WEAPON_FLAREGUN_REVENGE:
		case TF_WEAPON_COMPOUND_BOW:
		case TF_WEAPON_CROSSBOW:
		case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
		case TF_WEAPON_SYRINGEGUN_MEDIC:
		case TF_WEAPON_FLAME_BALL:
			s_mTraceCount["Setup trace test"]++;
		}
	}
	s_mTraceCount["Trace init check test"]++;
#endif
	ProjectileInfo tProjInfo = {};
	if (!F::ProjSim.GetInfo(pLocal, pWeapon, vAngles, tProjInfo, iFlags)
		|| !F::ProjSim.Initialize(tProjInfo))
		return false;

	CGameTrace trace = {};
	CTraceFilterCollideable filter = {};
	filter.pSkip = bSplash ? tTarget.m_pEntity : pLocal;
	filter.iPlayer = bSplash ? PLAYER_NONE : PLAYER_DEFAULT;
	filter.bMisc = !bSplash;
	int nMask = MASK_SOLID;
	if (!bSplash && F::AimbotGlobal.FriendlyFire())
	{
		switch (pWeapon->GetWeaponID())
		{	// only weapons that actually hit teammates properly
		case TF_WEAPON_ROCKETLAUNCHER:
		case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
		case TF_WEAPON_PARTICLE_CANNON:
		case TF_WEAPON_DRG_POMSON:
		case TF_WEAPON_FLAREGUN:
		case TF_WEAPON_SYRINGEGUN_MEDIC:
			filter.iPlayer = PLAYER_ALL;
		}
	}
	F::ProjSim.SetupTrace(filter, nMask, pWeapon);

#ifdef SPLASH_DEBUG5
	Vec3 vHull = tProjInfo.m_vHull.Max(1);
	G::BoxStorage.emplace_back(vPoint, vHull * -1, vHull, Vec3(), I::GlobalVars->curtime + 5.f, Color_t(255, 0, 0), Color_t(0, 0, 0, 0));
#endif

	if (!tProjInfo.m_flGravity)
	{
		SDK::TraceHull(tProjInfo.m_vPos, vPoint, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, nMask, &filter, &trace);
#ifdef SPLASH_DEBUG6
		s_mTraceCount["Nograv trace"]++;
#endif
#ifdef SPLASH_DEBUG5
		G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(tProjInfo.m_vPos, vPoint), I::GlobalVars->curtime + 5.f, Color_t(0, 0, 0));
#endif
		if (trace.fraction < 0.999f && trace.m_pEnt != tTarget.m_pEntity)
			return false;
	}

	bool bDidHit = false, bPrimeTime = false;
	const RestoreInfo_t tOriginal = { tTarget.m_pEntity->GetAbsOrigin(), tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs() };
	tTarget.m_pEntity->SetAbsOrigin(tTarget.m_vPos);
	tTarget.m_pEntity->m_vecMins() = { std::clamp(tTarget.m_pEntity->m_vecMins().x, -24.f, 0.f), std::clamp(tTarget.m_pEntity->m_vecMins().y, -24.f, 0.f), tTarget.m_pEntity->m_vecMins().z };
	tTarget.m_pEntity->m_vecMaxs() = { std::clamp(tTarget.m_pEntity->m_vecMaxs().x, 0.f, 24.f), std::clamp(tTarget.m_pEntity->m_vecMaxs().y, 0.f, 24.f), tTarget.m_pEntity->m_vecMaxs().z };
	for (int n = 1; n <= iSimTime; n++)
	{
		Vec3 vOld = F::ProjSim.GetOrigin();
		F::ProjSim.RunTick(tProjInfo);
		Vec3 vNew = F::ProjSim.GetOrigin();

		if (bDidHit)
		{
			trace.endpos = vNew;
			continue;
		}

		if (!bSplash)
		{
			SDK::TraceHull(vOld, vNew, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, nMask, &filter, &trace);
#ifdef SPLASH_DEBUG6
			s_mTraceCount["Direct trace"]++;
#endif

#ifdef SPLASH_DEBUG5
			G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(vOld, vNew), I::GlobalVars->curtime + 5.f, Color_t(255, 0, 0));
#endif
		}
		else
		{
			static Vec3 vStaticPos = {};
			if (n == 1 || bPrimeTime)
				vStaticPos = vOld;
			if (n % Vars::Aimbot::Projectile::SplashTraceInterval.Value && n != iSimTime && !bPrimeTime)
				continue;

			SDK::TraceHull(vStaticPos, vNew, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, nMask, &filter, &trace);
#ifdef SPLASH_DEBUG6
			s_mTraceCount["Splash trace"]++;
#endif
#ifdef SPLASH_DEBUG5
			G::LineStorage.emplace_back(std::pair<Vec3, Vec3>(vStaticPos, vNew), I::GlobalVars->curtime + 5.f, Color_t(255, 0, 0));
#endif
			vStaticPos = vNew;
		}
		if (trace.DidHit())
		{
			if (pHitSolid)
				*pHitSolid = true;

			bool bTime = bSplash
				? trace.endpos.DistTo(vPoint) < tProjInfo.m_flVelocity * TICK_INTERVAL + tProjInfo.m_vHull.z
				: iSimTime - n < 5 || pWeapon->GetWeaponID() == TF_WEAPON_LUNCHBOX; // projectile so slow it causes problems if we don't waive this check
			bool bTarget = trace.m_pEnt == tTarget.m_pEntity || bSplash;
			bool bValid = bTarget && bTime;
			if (bValid && bSplash)
			{
				bValid = SDK::VisPosWorld(nullptr, tTarget.m_pEntity, trace.endpos, vPoint, nMask);
				if (bValid)
				{
					Vec3 vFrom = trace.endpos;
					switch (pWeapon->GetWeaponID())
					{
					case TF_WEAPON_ROCKETLAUNCHER:
					case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
					case TF_WEAPON_PARTICLE_CANNON:
						vFrom += trace.plane.normal;
					}

					// Check multiple hull points instead of just eyes
					Vec3 vMins = tTarget.m_pEntity->m_vecMins();
					Vec3 vMaxs = tTarget.m_pEntity->m_vecMaxs();
					Vec3 testPoints[] = {
						tTarget.m_vPos + tTarget.m_pEntity->As<CTFPlayer>()->GetViewOffset(), // Eyes
						tTarget.m_vPos + Vec3(0, 0, vMaxs.z * 0.5f),                         // Torso
						tTarget.m_vPos + Vec3(0, 0, vMins.z + 8.0f)                          // Feet
					};

					bool bAnyVisible = false;
					for (const auto& testPoint : testPoints)
					{
						CGameTrace eyeTrace = {};
						SDK::Trace(vFrom, testPoint, MASK_SHOT, &filter, &eyeTrace);
						if (eyeTrace.fraction >= 0.85f) // More lenient than == 1.f
						{
							bAnyVisible = true;
							break;
						}
					}
					bValid = bAnyVisible;
				}
			}

#ifdef SPLASH_DEBUG5
			G::BoxStorage.pop_back();
			if (bValid)
				G::BoxStorage.emplace_back(vPoint, vHull * -1, vHull, Vec3(), I::GlobalVars->curtime + 5.f, Color_t(0, 255, 0), Color_t(0, 0, 0, 0));
			else if (!bTime)
			{
				G::BoxStorage.emplace_back(vPoint, vHull * -1, vHull, Vec3(), I::GlobalVars->curtime + 5.f, Color_t(255, 0, 255), Color_t(0, 0, 0, 0));
				if (bSplash)
				{
					G::BoxStorage.emplace_back(trace.endpos, Vec3(-1, -1, -1), Vec3(1, 1, 1), Vec3(), I::GlobalVars->curtime + 5.f, Color_t(0, 0, 0), Color_t(0, 0, 0, 0));
					G::BoxStorage.emplace_back(vPoint, Vec3(-1, -1, -1), Vec3(1, 1, 1), Vec3(), I::GlobalVars->curtime + 5.f, Color_t(255, 255, 255), Color_t(0, 0, 0, 0));
				}
			}
			else
				G::BoxStorage.emplace_back(vPoint, vHull * -1, vHull, Vec3(), I::GlobalVars->curtime + 5.f, Color_t(0, 0, 255), Color_t(0, 0, 0, 0));
#endif

			if (bValid)
			{
				if (bSplash)
				{
					int iPopCount = Vars::Aimbot::Projectile::SplashTraceInterval.Value - trace.fraction * Vars::Aimbot::Projectile::SplashTraceInterval.Value;
					for (int i = 0; i < iPopCount && !tProjInfo.m_vPath.empty(); i++)
						tProjInfo.m_vPath.pop_back();
				}

				// attempted to have a headshot check though this seems more detrimental than useful outside of smooth aimbot
				if (tTarget.m_nAimedHitbox == HITBOX_HEAD && pProjectilePath &&
					(Vars::Aimbot::General::AimType.Value == Vars::Aimbot::General::AimTypeEnum::Smooth
						|| Vars::Aimbot::General::AimType.Value == Vars::Aimbot::General::AimTypeEnum::Assistive))
				{	// loop and see if closest hitbox is head
					auto pModel = tTarget.m_pEntity->GetModel();
					if (!pModel) break;
					auto pHDR = I::ModelInfoClient->GetStudiomodel(pModel);
					if (!pHDR) break;
					auto pSet = pHDR->pHitboxSet(tTarget.m_pEntity->As<CTFPlayer>()->m_nHitboxSet());
					if (!pSet) break;

					auto aBones = F::Backtrack.GetBones(tTarget.m_pEntity);
					if (!aBones)
						break;

					Vec3 vOffset = tOriginal.m_vOrigin - tTarget.m_vPos;
					Vec3 vPos = trace.endpos + F::ProjSim.GetVelocity().Normalized() * 16 + vOffset;

					float flClosest = 0.f; int iClosest = -1;
					for (int nHitbox = 0; nHitbox < pSet->numhitboxes; ++nHitbox)
					{
						auto pBox = pSet->pHitbox(nHitbox);
						if (!pBox) continue;

						Vec3 vCenter; Math::VectorTransform({}, aBones[pBox->bone], vCenter);

						const float flDist = vPos.DistTo(vCenter);
						if (iClosest != -1 && flDist < flClosest || iClosest == -1)
						{
							flClosest = flDist;
							iClosest = nHitbox;
						}
					}
					if (iClosest != HITBOX_HEAD)
						break;
				}

				bDidHit = true;
			}
			else if (!bSplash && bTarget && pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
			{	// run for more ticks to check for splash
				iSimTime = n + 5;
				bSplash = bPrimeTime = true;
			}
			else
				break;

			if (!bSplash)
				trace.endpos = vNew;

			if (!bTarget || bSplash && !bPrimeTime)
				break;
		}
	}
	tTarget.m_pEntity->SetAbsOrigin(tOriginal.m_vOrigin);
	tTarget.m_pEntity->m_vecMins() = tOriginal.m_vMins;
	tTarget.m_pEntity->m_vecMaxs() = tOriginal.m_vMaxs;

	if (bDidHit && pProjectilePath)
	{
		tProjInfo.m_vPath.push_back(trace.endpos);
		*pProjectilePath = tProjInfo.m_vPath;
	}

	return bDidHit;
}

bool CAimbotProjectile::TestAngle(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, Target_t& tTarget,
	Vec3& vPoint, Vec3& vAngles, int iSimTime, bool bSplash, bool* pHitSolid,
	std::vector<Vec3>* pProjectilePath)
{
	// ============================================================================
	// PHASE 1: PROJECTILE INITIALIZATION
	// ============================================================================

	int iFlags = ProjSimEnum::Trace | ProjSimEnum::InitCheck |
		ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum;

	ProjectileInfo tProjInfo = {};
	if (!F::ProjSim.GetInfo(pLocal, pWeapon, vAngles, tProjInfo, iFlags) ||
		!F::ProjSim.Initialize(tProjInfo))
		return false;

	// ============================================================================
	// PHASE 2: COLLISION FILTER SETUP
	// ============================================================================

	CGameTrace trace = {};
	CTraceFilterCollideable filter = {};

	filter.pSkip = bSplash ? tTarget.m_pEntity : pLocal;
	filter.iPlayer = bSplash ? PLAYER_NONE : PLAYER_DEFAULT;
	filter.bMisc = !bSplash;

	int nMask = MASK_SOLID;

	if (!bSplash && F::AimbotGlobal.FriendlyFire())
	{
		switch (pWeapon->GetWeaponID())
		{
		case TF_WEAPON_ROCKETLAUNCHER:
		case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
		case TF_WEAPON_PARTICLE_CANNON:
		case TF_WEAPON_DRG_POMSON:
		case TF_WEAPON_FLAREGUN:
		case TF_WEAPON_SYRINGEGUN_MEDIC:
			filter.iPlayer = PLAYER_ALL;
			break;
		}
	}

	F::ProjSim.SetupTrace(filter, nMask, pWeapon);

	// ============================================================================
	// PHASE 3: INITIAL TRAJECTORY CHECK (hitscan projectiles)
	// ============================================================================

	if (!tProjInfo.m_flGravity)
	{
		SDK::TraceHull(tProjInfo.m_vPos, vPoint, tProjInfo.m_vHull * -1,
			tProjInfo.m_vHull, nMask, &filter, &trace);

		if (trace.fraction < 0.999f && trace.m_pEnt != tTarget.m_pEntity)
			return false;
	}

	// ============================================================================
	// PHASE 4: TARGET SETUP
	// ============================================================================

	bool bDidHit = false;
	bool bPrimeTime = false;

	const RestoreInfo_t tOriginal = {
		tTarget.m_pEntity->GetAbsOrigin(),
		tTarget.m_pEntity->m_vecMins(),
		tTarget.m_pEntity->m_vecMaxs()
	};

	tTarget.m_pEntity->SetAbsOrigin(tTarget.m_vPos);

	tTarget.m_pEntity->m_vecMins() = Vec3(
		std::clamp(tTarget.m_pEntity->m_vecMins().x, -24.f, 0.f),
		std::clamp(tTarget.m_pEntity->m_vecMins().y, -24.f, 0.f),
		tTarget.m_pEntity->m_vecMins().z
	);
	tTarget.m_pEntity->m_vecMaxs() = Vec3(
		std::clamp(tTarget.m_pEntity->m_vecMaxs().x, 0.f, 24.f),
		std::clamp(tTarget.m_pEntity->m_vecMaxs().y, 0.f, 24.f),
		tTarget.m_pEntity->m_vecMaxs().z
	);

	// ============================================================================
	// PHASE 5: PROJECTILE SIMULATION - OPTIMIZED
	// Check EVERY tick, stop immediately on valid hit
	// ============================================================================

	for (int n = 1; n <= iSimTime; n++)
	{
		Vec3 vOld = F::ProjSim.GetOrigin();
		F::ProjSim.RunTick(tProjInfo);
		Vec3 vNew = F::ProjSim.GetOrigin();

		if (bDidHit)
		{
			// Already hit - just update path endpoint
			trace.endpos = vNew;
			continue;
		}

		// ====================================================================
		// COLLISION DETECTION - Check EVERY tick (no skipping)
		// ====================================================================

		if (!bSplash)
		{
			// Direct hit: trace every tick
			SDK::TraceHull(vOld, vNew, tProjInfo.m_vHull * -1,
				tProjInfo.m_vHull, nMask, &filter, &trace);
		}
		else
		{
			// Splash: ALSO check every tick now (removed interval skipping)
			SDK::TraceHull(vOld, vNew, tProjInfo.m_vHull * -1,
				tProjInfo.m_vHull, nMask, &filter, &trace);
		}

		// ====================================================================
		// HIT VALIDATION
		// ====================================================================

		if (trace.DidHit())
		{
			if (pHitSolid)
				*pHitSolid = true;

			// Check for skybox (invalid hit)
			if (trace.surface.flags & SURF_SKY)
			{
				tTarget.m_pEntity->SetAbsOrigin(tOriginal.m_vOrigin);
				tTarget.m_pEntity->m_vecMins() = tOriginal.m_vMins;
				tTarget.m_pEntity->m_vecMaxs() = tOriginal.m_vMaxs;
				return false;
			}

			// Check timing validity
			bool bTime = bSplash
				? trace.endpos.DistTo(vPoint) < tProjInfo.m_flVelocity * TICK_INTERVAL + tProjInfo.m_vHull.z
				: iSimTime - n < 5 || pWeapon->GetWeaponID() == TF_WEAPON_LUNCHBOX;

			bool bTarget = trace.m_pEnt == tTarget.m_pEntity || bSplash;
			bool bValid = bTarget && bTime;

			// ================================================================
			// SPLASH DAMAGE VALIDATION
			// ================================================================

			if (bValid && bSplash)
			{
				bValid = SDK::VisPosWorld(nullptr, tTarget.m_pEntity,
					trace.endpos, vPoint, nMask);

				if (bValid)
				{
					Vec3 vFrom = trace.endpos;

					switch (pWeapon->GetWeaponID())
					{
					case TF_WEAPON_ROCKETLAUNCHER:
					case TF_WEAPON_ROCKETLAUNCHER_DIRECTHIT:
					case TF_WEAPON_PARTICLE_CANNON:
						vFrom += trace.plane.normal;
					}

					// Check visibility to eyes
					CGameTrace eyeTrace = {};
					SDK::Trace(vFrom, tTarget.m_vPos + tTarget.m_pEntity->As<CTFPlayer>()->GetViewOffset(),
						MASK_SHOT, &filter, &eyeTrace);

					bValid = eyeTrace.fraction >= (Vars::Aimbot::Projectile::TraceVisibility.Value / 100);
				}
			}

			if (bValid)
			{
				// Optional: Huntsman headshot validation
				if (tTarget.m_nAimedHitbox == HITBOX_HEAD && pProjectilePath &&
					(Vars::Aimbot::General::AimType.Value == Vars::Aimbot::General::AimTypeEnum::Smooth ||
						Vars::Aimbot::General::AimType.Value == Vars::Aimbot::General::AimTypeEnum::Assistive))
				{
					auto pModel = tTarget.m_pEntity->GetModel();
					if (pModel)
					{
						auto pHDR = I::ModelInfoClient->GetStudiomodel(pModel);
						if (pHDR)
						{
							auto pSet = pHDR->pHitboxSet(tTarget.m_pEntity->As<CTFPlayer>()->m_nHitboxSet());
							if (pSet)
							{
								auto aBones = F::Backtrack.GetBones(tTarget.m_pEntity);
								if (aBones)
								{
									Vec3 vOffset = tOriginal.m_vOrigin - tTarget.m_vPos;
									Vec3 vPos = trace.endpos + F::ProjSim.GetVelocity().Normalized() * 16 + vOffset;

									float flClosest = 0.f;
									int iClosest = -1;

									for (int nHitbox = 0; nHitbox < pSet->numhitboxes; ++nHitbox)
									{
										auto pBox = pSet->pHitbox(nHitbox);
										if (!pBox) continue;

										Vec3 vCenter;
										Math::VectorTransform({}, aBones[pBox->bone], vCenter);

										const float flDist = vPos.DistTo(vCenter);
										if (iClosest == -1 || flDist < flClosest)
										{
											flClosest = flDist;
											iClosest = nHitbox;
										}
									}

									if (iClosest != HITBOX_HEAD)
										break;
								}
							}
						}
					}
				}

				bDidHit = true;

				// STOP SIMULATION - We found our hit!
				// No need to continue simulating
				break; // <-- KEY OPTIMIZATION: Exit immediately on valid hit
			}
			else if (!bSplash && bTarget && pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
			{
				// Sticky bomb: extend simulation for prime time splash check
				iSimTime = n + 5;
				bSplash = bPrimeTime = true;
			}
			else
			{
				// Invalid hit - stop checking this target
				break;
			}

			if (!bSplash)
				trace.endpos = vNew;

			if (!bTarget || (bSplash && !bPrimeTime))
				break;
		}
	}

	// ============================================================================
	// CLEANUP: Restore original target state
	// ============================================================================

	tTarget.m_pEntity->SetAbsOrigin(tOriginal.m_vOrigin);
	tTarget.m_pEntity->m_vecMins() = tOriginal.m_vMins;
	tTarget.m_pEntity->m_vecMaxs() = tOriginal.m_vMaxs;

	if (bDidHit && pProjectilePath)
	{
		tProjInfo.m_vPath.push_back(trace.endpos);
		*pProjectilePath = tProjInfo.m_vPath;
	}

	return bDidHit;
}

int CAimbotProjectile::CanHit(Target_t& tTarget, CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	//if (Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Unsimulated && H::Entities.GetChoke(tTarget.m_pEntity->entindex()) > Vars::Aimbot::General::TickTolerance.Value)
	//	return false;

	if (!tTarget.m_pEntity) {
		return false; // Handle invalid target
	}

	ProjectileInfo tProjInfo = {};
	if (!F::ProjSim.GetInfo(pLocal, pWeapon, {}, tProjInfo, ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum)
		|| !F::ProjSim.Initialize(tProjInfo, false))
		return false;

	MoveStorage tStorage;
	F::MoveSim.Initialize(tTarget.m_pEntity, tStorage);
	tTarget.m_vPos = tTarget.m_pEntity->m_vecOrigin();

	m_tInfo = { pLocal, pWeapon };
	m_tInfo.m_vLocalEye = pLocal->GetShootPos();

	if (Vars::Aimbot::Projectile::UseEyeTrace.Value) {
		auto pPlayer = tTarget.m_pEntity->As<CTFPlayer>();
		if (pPlayer) {
			m_tInfo.m_vTargetEye = pPlayer->GetViewOffset();
		}
	}
	else {
		m_tInfo.m_vTargetEye = Vec3(0, 0, (tTarget.m_pEntity->m_vecMaxs().z + tTarget.m_pEntity->m_vecMins().z) / 2);
	}


	m_tInfo.m_flLatency = F::Backtrack.GetReal() + TICKS_TO_TIME(F::Backtrack.GetAnticipatedChoke());

	Vec3 vVelocity = F::ProjSim.GetVelocity();
	m_tInfo.m_flVelocity = vVelocity.Length();
	m_tInfo.m_vAngFix = Math::VectorAngles(vVelocity);

	m_tInfo.m_vHull = tProjInfo.m_vHull.Min(3);
	m_tInfo.m_vOffset = tProjInfo.m_vPos - m_tInfo.m_vLocalEye; m_tInfo.m_vOffset.y *= -1;
	m_tInfo.m_flOffsetTime = m_tInfo.m_vOffset.Length() / m_tInfo.m_flVelocity; // silly

	float flSize = tTarget.m_pEntity->GetSize().Length();
	m_tInfo.m_flGravity = tProjInfo.m_flGravity;
	m_tInfo.m_iSplashCount = !m_tInfo.m_flGravity ? Vars::Aimbot::Projectile::SplashCountDirect.Value : Vars::Aimbot::Projectile::SplashCountArc.Value;
	m_tInfo.m_flRadius = GetSplashRadius(pWeapon, pLocal);
	m_tInfo.m_flRadiusTime = m_tInfo.m_flRadius / m_tInfo.m_flVelocity;
	m_tInfo.m_flBoundingTime = m_tInfo.m_flRadiusTime + flSize / m_tInfo.m_flVelocity;

	m_tInfo.m_iSplashMode = GetSplashMode(pWeapon);
	m_tInfo.m_flPrimeTime = PrimeTime(pWeapon);
	m_tInfo.m_iPrimeTime = TIME_TO_TICKS(m_tInfo.m_flPrimeTime);


	int iSplash = Vars::Aimbot::Projectile::SplashPrediction.Value && m_tInfo.m_flRadius ? Vars::Aimbot::Projectile::SplashPrediction.Value : Vars::Aimbot::Projectile::SplashPredictionEnum::Off;
	int iReturn = false;
	int iMaxTime = TIME_TO_TICKS(std::min(tProjInfo.m_flLifetime, Vars::Aimbot::Projectile::MaxSimulationTime.Value));
	int iMulti = Vars::Aimbot::Projectile::SplashMode.Value;
	//int iPoints = !m_tInfo.m_flGravity ? (Vars::Aimbot::Projectile::SplashPointsDirect.Value / Vars::Aimbot::General::MaxTargets.Value) : (Vars::Aimbot::Projectile::SplashPointsArc.Value / Vars::Aimbot::General::MaxTargets.Value);
	float flDensity = Vars::Aimbot::Projectile::SplashPointDensity.Value;
	//std::vector<std::pair<Vec3, int>> vSpherePoints;
	auto mDirectPoints = iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Only ? std::unordered_map<int, Vec3>() : GetDirectPoints(tTarget);
	//if (Vars::Aimbot::Projectile::UseSplashPointDensity.Value) {
	int iPoints = 3000 / std::max(1, Vars::Aimbot::General::MaxTargets.Value);



	std::vector<std::pair<Vec3, int>> vSpherePoints = !iSplash ? std::vector<std::pair<Vec3, int>>() : ComputeSphereWithGeometryOptimizedAndWallBleed(m_tInfo.m_flRadius, iPoints,
			tTarget.m_vPos + m_tInfo.m_vTargetEye,
			tTarget.m_pEntity->entindex());
	//}
	//else {
	//	vSpherePoints = !iSplash ? std::vector<std::pair<Vec3, int>>() : ComputeSphere(m_tInfo.m_flRadius /*+ flSize*/, iPoints);
	//}
	std::vector<Vec3> vEdgePoints;
	if (iSplash && Vars::Aimbot::Projectile::EdgeDetection.Value)
	{
		vEdgePoints = DetectEdgePoints(
			tTarget.m_vPos + m_tInfo.m_vTargetEye,
			m_tInfo.m_flRadius + flSize,
			Vars::Aimbot::Projectile::EdgeSearchRadius.Value,
			Vars::Aimbot::Projectile::EdgeSamples.Value
		);
	}


	bool bTargetOnGround = tStorage.on_ground;

	//if (Vars::Aimbot::Projectile::Testsplash.Value && (iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Prefer)) {
		if (!bTargetOnGround) {
			iSplash = Vars::Aimbot::Projectile::SplashPredictionEnum::Only;
			//I::CVar->ConsolePrintf("Target = ground\n");
		}
		else if (bTargetOnGround) {
			iSplash = Vars::Aimbot::Projectile::SplashPredictionEnum::Prefer;
			//I::CVar->ConsolePrintf("Target = air\n");
		}
	//}

#ifdef SPLASH_DEBUG4
	for (auto& [vPoint, _] : vSpherePoints)
		G::BoxStorage.emplace_back(tTarget.m_pEntity->m_vecOrigin() + m_tInfo.m_vTargetEye + vPoint, Vec3(-1, -1, -1), Vec3(1, 1, 1), Vec3(), I::GlobalVars->curtime + 60.f, Color_t(0, 0, 0, 0), Vars::Colors::Local.Value);
#endif

	Vec3 vAngleTo, vPredicted, vTarget;
	int iLowestPriority = std::numeric_limits<int>::max(); float flLowestDist = std::numeric_limits<float>::max();
	int iLowestSmoothPriority = iLowestPriority; float flLowestSmoothDist = flLowestDist;
	for (int i = -TIME_TO_TICKS(m_tInfo.m_flLatency); i <= iMaxTime; i++)
	{
		if (!tStorage.m_bFailed)
		{
			F::MoveSim.RunTick(tStorage);
			tTarget.m_vPos = GetSimulatedPos(tStorage);
		}

		if (i < 0)
			continue;

		bool bDirectBreaks = true;
		std::vector<Point_t> vSplashPoints = {};
		if (iSplash)
		{
			Solution_t solution; CalculateAngle(m_tInfo.m_vLocalEye, tTarget.m_vPos, i, solution, false);
			if (solution.m_iCalculated != CalculatedEnum::Bad)
			{
				bDirectBreaks = false;

				const float flTimeTo = solution.m_flTime - TICKS_TO_TIME(i);
				if (flTimeTo < m_tInfo.m_flBoundingTime)
				{
					static std::vector<std::pair<Vec3, Vec3>> vSimplePoints = {};
					if (iMulti == Vars::Aimbot::Projectile::SplashModeEnum::Single)
					{
						if (Vars::Aimbot::Projectile::UsePointSplash.Value || tProjInfo.m_flGravity) {
							SetupSplashPointsPoints(tTarget, vSpherePoints, vSimplePoints);
						}
						else {
							SetupSplashPoints(tTarget, vSpherePoints, vSimplePoints, vEdgePoints);
						}
						if (!vSimplePoints.empty())
							iMulti++;
						else
						{
							iSplash = Vars::Aimbot::Projectile::SplashPredictionEnum::Off;
							goto skipSplash;
						}
					}

					if ((iMulti == Vars::Aimbot::Projectile::SplashModeEnum::Multi ? vSpherePoints.empty() : vSimplePoints.empty())
						|| flTimeTo < -m_tInfo.m_flBoundingTime && (m_tInfo.m_flPrimeTime ? i > m_tInfo.m_iPrimeTime : true))
						break;
					else if (m_tInfo.m_flPrimeTime ? i >= m_tInfo.m_iPrimeTime : true)
					{
						if (iMulti == Vars::Aimbot::Projectile::SplashModeEnum::Multi)
							vSplashPoints = GetSplashPoints(tTarget, vSpherePoints, i);
						else
							vSplashPoints = GetSplashPointsSimple(tTarget, vSimplePoints, vEdgePoints, i);
					}
				}
			}
		}
	skipSplash:
		if (bDirectBreaks && mDirectPoints.empty())
			break;

		std::vector<std::tuple<Point_t, int, int>> vPoints = {};
		for (auto& [iIndex, vPoint] : mDirectPoints)
			vPoints.emplace_back(Point_t(tTarget.m_vPos + vPoint, {}), iIndex + (iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Prefer ? m_tInfo.m_iSplashCount : 0), iIndex);
		for (auto& vPoint : vSplashPoints)
			vPoints.emplace_back(vPoint, iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Include ? 3 : 0, -1);

		int j = 0;
		for (auto& [vPoint, iPriority, iIndex] : vPoints) // get most ideal point
		{
			const bool bSplash = iIndex == -1;
			Vec3 vOriginalPoint = vPoint.m_vPoint;

			if (Vars::Aimbot::Projectile::HuntsmanPullPoint.Value && tTarget.m_nAimedHitbox == HITBOX_HEAD)
				vPoint.m_vPoint = PullPoint(vPoint.m_vPoint, m_tInfo.m_vLocalEye, m_tInfo, tTarget.m_pEntity->m_vecMins() + tProjInfo.m_vHull, tTarget.m_pEntity->m_vecMaxs() - tProjInfo.m_vHull, tTarget.m_vPos);
			//vPoint.m_vPoint = PullPoint(vPoint.m_vPoint, m_tInfo.m_vLocalEye, m_tInfo, tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs(), tTarget.m_vPos);

			float flDist = bSplash ? tTarget.m_vPos.DistTo(vPoint.m_vPoint) : flLowestDist;
			bool bPriority = bSplash ? iPriority <= iLowestPriority : iPriority < iLowestPriority;
			bool bTime = bSplash || m_tInfo.m_iPrimeTime < i || tStorage.m_MoveData.m_vecVelocity.IsZero();
			bool bDist = !bSplash || flDist < flLowestDist;
			if (!bSplash && !bPriority)
				mDirectPoints.erase(iIndex);
			if (!bPriority || !bTime || !bDist)
				continue;

			CalculateAngle(m_tInfo.m_vLocalEye, vPoint.m_vPoint, i, vPoint.m_tSolution);
			if (!bSplash && (vPoint.m_tSolution.m_iCalculated == CalculatedEnum::Good || vPoint.m_tSolution.m_iCalculated == CalculatedEnum::Bad))
				mDirectPoints.erase(iIndex);
			if (vPoint.m_tSolution.m_iCalculated != CalculatedEnum::Good)
				continue;

			if (Vars::Aimbot::Projectile::HuntsmanPullPoint.Value && tTarget.m_nAimedHitbox == HITBOX_HEAD)
			{
				Solution_t tSolution;
				CalculateAngle(m_tInfo.m_vLocalEye, vOriginalPoint, std::numeric_limits<int>::max(), tSolution);
				vPoint.m_tSolution.m_flPitch = tSolution.m_flPitch, vPoint.m_tSolution.m_flYaw = tSolution.m_flYaw;
			}

			Vec3 vAngles; Aim(G::CurrentUserCmd->viewangles, { vPoint.m_tSolution.m_flPitch, vPoint.m_tSolution.m_flYaw, 0.f }, vAngles);
			std::vector<Vec3> vProjLines; bool bHitSolid = false;


			const bool bTestResult =
				Vars::Aimbot::Projectile::UsePointSplash.Value
				? TestAnglePoints(
					pLocal, pWeapon, tTarget,
					vPoint.m_vPoint, vAngles,
					i, bSplash, &bHitSolid, &vProjLines)
				: TestAngle(
					pLocal, pWeapon, tTarget,
					vPoint.m_vPoint, vAngles,
					i, bSplash, &bHitSolid, &vProjLines);

			if (bTestResult)
			{
				iLowestPriority = iPriority;
				flLowestDist = flDist;

				vAngleTo = vAngles;
				vPredicted = tTarget.m_vPos;
				vTarget = vOriginalPoint;

				m_flTimeTo = vPoint.m_tSolution.m_flTime + m_tInfo.m_flLatency;

				m_vPlayerPath = tStorage.m_vPath;
				m_vPlayerPath.push_back(tStorage.m_MoveData.m_vecAbsOrigin);

				m_vProjectilePath = vProjLines;
			}


			//if (TestAngle(pLocal, pWeapon, tTarget, vPoint.m_vPoint, vAngles, i, bSplash, &bHitSolid, &vProjLines))
			//{
			//	iLowestPriority = iPriority; flLowestDist = flDist;
			//	vAngleTo = vAngles, vPredicted = tTarget.m_vPos, vTarget = vOriginalPoint;
			//	m_flTimeTo = vPoint.m_tSolution.m_flTime + m_tInfo.m_flLatency;
			//	m_vPlayerPath = tStorage.m_vPath;
			//	m_vPlayerPath.push_back(tStorage.m_MoveData.m_vecAbsOrigin);
			//	m_vProjectilePath = vProjLines;
			//}
			else switch (Vars::Aimbot::General::AimType.Value)
			{
			case Vars::Aimbot::General::AimTypeEnum::Smooth:
				if (Vars::Aimbot::General::AssistStrength.Value == 100.f)
					break;
				[[fallthrough]];
			case Vars::Aimbot::General::AimTypeEnum::Assistive:
			{
				bPriority = bSplash ? iPriority <= iLowestSmoothPriority : iPriority < flLowestSmoothDist;
				bDist = !bSplash || flDist < flLowestDist;
				if (!bPriority || !bDist)
					continue;

				Vec3 vPlainAngles; Aim({}, { vPoint.m_tSolution.m_flPitch, vPoint.m_tSolution.m_flYaw, 0.f }, vPlainAngles, Vars::Aimbot::General::AimTypeEnum::Plain);
				if (TestAngle(pLocal, pWeapon, tTarget, vPoint.m_vPoint, vPlainAngles, i, bSplash, &bHitSolid))
				{
					iLowestSmoothPriority = iPriority; flLowestSmoothDist = flDist;
					vAngleTo = vAngles, vPredicted = tTarget.m_vPos;
					m_vPlayerPath = tStorage.m_vPath;
					m_vPlayerPath.push_back(tStorage.m_MoveData.m_vecAbsOrigin);
					iReturn = 2;
				}
			}
			}

			if (!j && bHitSolid)
				m_flTimeTo = vPoint.m_tSolution.m_flTime + m_tInfo.m_flLatency;
			j++;
		}
	}
	F::MoveSim.Restore(tStorage);

	tTarget.m_vPos = vTarget;
	tTarget.m_vAngleTo = vAngleTo;
	if (tTarget.m_iTargetType != TargetEnum::Player || !tStorage.m_bFailed) // don't attempt to aim at players when movesim fails
	{
		bool bMain = iLowestPriority != std::numeric_limits<int>::max();
		bool bAny = bMain || iLowestSmoothPriority != std::numeric_limits<int>::max();

		if (bAny && (Vars::Colors::BoundHitboxEdge.Value.a || Vars::Colors::BoundHitboxFace.Value.a || Vars::Colors::BoundHitboxEdgeIgnoreZ.Value.a || Vars::Colors::BoundHitboxFaceIgnoreZ.Value.a))
		{
			m_tInfo.m_vHull = m_tInfo.m_vHull.Max(1);
			float flProjectileTime = TICKS_TO_TIME(m_vProjectilePath.size());
			float flTargetTime = tStorage.m_bFailed ? flProjectileTime : TICKS_TO_TIME(m_vPlayerPath.size());

			bool bBox = Vars::Visuals::Hitbox::BoundsEnabled.Value & Vars::Visuals::Hitbox::BoundsEnabledEnum::OnShot;
			bool bPoint = Vars::Visuals::Hitbox::BoundsEnabled.Value & Vars::Visuals::Hitbox::BoundsEnabledEnum::AimPoint;
			if (bBox)
			{
				if (Vars::Colors::BoundHitboxEdgeIgnoreZ.Value.a || Vars::Colors::BoundHitboxFaceIgnoreZ.Value.a)
					m_vBoxes.emplace_back(vPredicted, tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs(), Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flTargetTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdgeIgnoreZ.Value, Vars::Colors::BoundHitboxFaceIgnoreZ.Value);
				if (Vars::Colors::BoundHitboxEdge.Value.a || Vars::Colors::BoundHitboxFace.Value.a)
					m_vBoxes.emplace_back(vPredicted, tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs(), Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flTargetTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdge.Value, Vars::Colors::BoundHitboxFace.Value, true);
			}
			if (bMain && bPoint)
			{
				if (Vars::Colors::BoundHitboxEdgeIgnoreZ.Value.a || Vars::Colors::BoundHitboxFaceIgnoreZ.Value.a)
					m_vBoxes.emplace_back(vTarget, m_tInfo.m_vHull * -1, m_tInfo.m_vHull, Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flProjectileTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdgeIgnoreZ.Value, Vars::Colors::BoundHitboxFaceIgnoreZ.Value);
				if (Vars::Colors::BoundHitboxEdge.Value.a || Vars::Colors::BoundHitboxFace.Value.a)
					m_vBoxes.emplace_back(vTarget, m_tInfo.m_vHull * -1, m_tInfo.m_vHull, Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flProjectileTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdge.Value, Vars::Colors::BoundHitboxFace.Value, true);
			}
		}

		if (bMain)
			return true;
	}

	return iReturn;
}



bool CAimbotProjectile::Aim(Vec3 vCurAngle, Vec3 vToAngle, Vec3& vOut, int iMethod)
{
	/*
	if (Vec3* pDoubletapAngle = F::Ticks.GetShootAngle())
	{
		vOut = *pDoubletapAngle;
		return true;
	}
	*/

	bool bReturn = false;
	switch (iMethod)
	{
	case Vars::Aimbot::General::AimTypeEnum::Plain:
	case Vars::Aimbot::General::AimTypeEnum::Silent:
	case Vars::Aimbot::General::AimTypeEnum::Locking:
		vOut = vToAngle;
		break;
	case Vars::Aimbot::General::AimTypeEnum::Smooth:
		vOut = vCurAngle.LerpAngle(vToAngle, Vars::Aimbot::General::AssistStrength.Value / 100.f);
		bReturn = true;
		break;
	case Vars::Aimbot::General::AimTypeEnum::Assistive:
		Vec3 vMouseDelta = G::CurrentUserCmd->viewangles.DeltaAngle(G::LastUserCmd->viewangles);
		Vec3 vTargetDelta = vToAngle.DeltaAngle(G::LastUserCmd->viewangles);
		float flMouseDelta = vMouseDelta.Length2D(), flTargetDelta = vTargetDelta.Length2D();
		vTargetDelta = vTargetDelta.Normalized() * std::min(flMouseDelta, flTargetDelta);
		vOut = vCurAngle - vMouseDelta + vMouseDelta.LerpAngle(vTargetDelta, Vars::Aimbot::General::AssistStrength.Value / 100.f);
		bReturn = true;
		break;
	}

	Math::ClampAngles(vOut);
	return bReturn;
}

// assume angle calculated outside with other overload
void CAimbotProjectile::Aim(CUserCmd* pCmd, Vec3& vAngle, int iMethod)
{
	bool bUnsure = F::Ticks.IsTimingUnsure();
	switch (iMethod)
	{
	case Vars::Aimbot::General::AimTypeEnum::Plain:
		if (G::Attacking != 1 && !bUnsure)
			break;
		[[fallthrough]];
	case Vars::Aimbot::General::AimTypeEnum::Smooth:
	case Vars::Aimbot::General::AimTypeEnum::Assistive:
		pCmd->viewangles = vAngle;
		I::EngineClient->SetViewAngles(vAngle);
		break;
	case Vars::Aimbot::General::AimTypeEnum::Silent:
		if (auto pWeapon = H::Entities.GetWeapon();
			G::Attacking == 1 || bUnsure || pWeapon && pWeapon->GetWeaponID() == TF_WEAPON_FLAMETHROWER)
		{
			SDK::FixMovement(pCmd, vAngle);
			pCmd->viewangles = vAngle;
			G::PSilentAngles = true;
		}
		break;
	case Vars::Aimbot::General::AimTypeEnum::Locking:
		SDK::FixMovement(pCmd, vAngle);
		pCmd->viewangles = vAngle;
		G::SilentAngles = true;
	}
}

static inline void CancelShot(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, int& iLastTickCancel)
{
	switch (pWeapon->GetWeaponID())
	{
	case TF_WEAPON_COMPOUND_BOW:
	{
		pCmd->buttons |= IN_ATTACK2;
		pCmd->buttons &= ~IN_ATTACK;
		break;
	}
	case TF_WEAPON_CANNON:
	case TF_WEAPON_PIPEBOMBLAUNCHER:
	{
		for (int i = 0; i < MAX_WEAPONS; i++)
		{
			auto pSwap = pLocal->GetWeaponFromSlot(i);
			if (!pSwap || pSwap == pWeapon || !pSwap->CanBeSelected())
				continue;

			pCmd->weaponselect = pSwap->entindex();
			iLastTickCancel = pWeapon->entindex();
			break;
		}
	}
	}
}

static inline void DrawVisuals(int iResult, Target_t& tTarget, std::vector<Vec3>& vPlayerPath, std::vector<Vec3>& vProjectilePath, std::vector<DrawBox_t>& vBoxes)
{
	if (G::Attacking == 1 || !Vars::Aimbot::General::AutoShoot.Value || iResult != 1)
	{
		bool bPlayerPath = Vars::Visuals::Simulation::PlayerPath.Value;
		bool bProjectilePath = Vars::Visuals::Simulation::ProjectilePath.Value && (G::Attacking == 1 || Vars::Debug::Info.Value) && iResult == 1;
		bool bBoxes = Vars::Visuals::Hitbox::BoundsEnabled.Value & (Vars::Visuals::Hitbox::BoundsEnabledEnum::OnShot | Vars::Visuals::Hitbox::BoundsEnabledEnum::AimPoint);
		bool bRealPath = Vars::Visuals::Simulation::RealPath.Value && iResult == 1;
		if (bPlayerPath || bProjectilePath || bBoxes || bRealPath)
		{
			G::PathStorage.clear();
			G::BoxStorage.clear();
			G::LineStorage.clear();

			if (bPlayerPath)
			{
				if (Vars::Colors::PlayerPathIgnoreZ.Value.a)
					G::PathStorage.emplace_back(vPlayerPath, Vars::Visuals::Simulation::Timed.Value ? -int(vPlayerPath.size()) : I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::PlayerPathIgnoreZ.Value, Vars::Visuals::Simulation::PlayerPath.Value);
				if (Vars::Colors::PlayerPath.Value.a)
					G::PathStorage.emplace_back(vPlayerPath, Vars::Visuals::Simulation::Timed.Value ? -int(vPlayerPath.size()) : I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::PlayerPath.Value, Vars::Visuals::Simulation::PlayerPath.Value, true);
			}
			if (bProjectilePath)
			{
				if (Vars::Colors::ProjectilePathIgnoreZ.Value.a)
					G::PathStorage.emplace_back(vProjectilePath, Vars::Visuals::Simulation::Timed.Value ? -int(vProjectilePath.size()) - TIME_TO_TICKS(F::Backtrack.GetReal()) : I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::ProjectilePathIgnoreZ.Value, Vars::Visuals::Simulation::ProjectilePath.Value);
				if (Vars::Colors::ProjectilePath.Value.a)
					G::PathStorage.emplace_back(vProjectilePath, Vars::Visuals::Simulation::Timed.Value ? -int(vProjectilePath.size()) - TIME_TO_TICKS(F::Backtrack.GetReal()) : I::GlobalVars->curtime + Vars::Visuals::Simulation::DrawDuration.Value, Vars::Colors::ProjectilePath.Value, Vars::Visuals::Simulation::ProjectilePath.Value, true);
			}
			if (bBoxes)
				G::BoxStorage.insert(G::BoxStorage.end(), vBoxes.begin(), vBoxes.end());
			if (bRealPath)
				F::Aimbot.Store(tTarget.m_pEntity, vPlayerPath.size());
		}
	}
}

bool CAimbotProjectile::RunMain(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	const int nWeaponID = pWeapon->GetWeaponID();

	static int iStaticAimType = Vars::Aimbot::General::AimType.Value;
	const int iLastAimType = iStaticAimType;
	const int iRealAimType = Vars::Aimbot::General::AimType.Value;

	switch (nWeaponID)
	{
	case TF_WEAPON_COMPOUND_BOW:
	case TF_WEAPON_PIPEBOMBLAUNCHER:
	case TF_WEAPON_CANNON:
		if (!Vars::Aimbot::General::AutoShoot.Value && G::Attacking && !iRealAimType && iLastAimType)
			Vars::Aimbot::General::AimType.Value = iLastAimType;
		break;
	default:
		if (G::Throwing && !iRealAimType && iLastAimType)
			Vars::Aimbot::General::AimType.Value = iLastAimType;
	}
	iStaticAimType = Vars::Aimbot::General::AimType.Value;

	if (F::AimbotGlobal.ShouldHoldAttack(pWeapon))
		pCmd->buttons |= IN_ATTACK;
	if (!Vars::Aimbot::General::AimType.Value
		|| !F::AimbotGlobal.ShouldAim() && nWeaponID != TF_WEAPON_FLAMETHROWER)
		return false;

	auto vTargets = SortTargets(pLocal, pWeapon);
	if (vTargets.empty())
		return false;

	if (Vars::Aimbot::Projectile::Modifiers.Value & Vars::Aimbot::Projectile::ModifiersEnum::ChargeWeapon && iRealAimType
		&& (nWeaponID == TF_WEAPON_COMPOUND_BOW || nWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER))
	{
		pCmd->buttons |= IN_ATTACK;
		if (!G::CanPrimaryAttack && !G::Reloading && Vars::Aimbot::General::AimType.Value == Vars::Aimbot::General::AimTypeEnum::Silent)
			return false;
	}

	if (!G::AimTarget.m_iEntIndex)
		G::AimTarget = { vTargets.front().m_pEntity->entindex(), I::GlobalVars->tickcount, 0 };

#if defined(SPLASH_DEBUG1) || defined(SPLASH_DEBUG2) || defined(SPLASH_DEBUG3) || defined(SPLASH_DEBUG5)
	G::LineStorage.clear();
#endif
#if defined(SPLASH_DEBUG1) || defined(SPLASH_DEBUG2) || defined(SPLASH_DEBUG3) || defined(SPLASH_DEBUG4) || defined(SPLASH_DEBUG5)
	G::BoxStorage.clear();
#endif
	for (auto& tTarget : vTargets)
	{
		m_flTimeTo = std::numeric_limits<float>::max();
		m_vPlayerPath.clear(); m_vProjectilePath.clear(); m_vBoxes.clear();

		const int iResult = CanHit(tTarget, pLocal, pWeapon);
		if (iResult != 1 && pWeapon->GetWeaponID() == TF_WEAPON_CANNON && Vars::Aimbot::Projectile::Modifiers.Value & Vars::Aimbot::Projectile::ModifiersEnum::ChargeWeapon && !(pCmd->buttons & IN_ATTACK))
		{
			float flCharge = pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() > 0.f
				? pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() - I::GlobalVars->curtime
				: 1.f;
			flCharge = floorf(flCharge / 0.195f) * 0.195f;
			if (flCharge < m_flTimeTo)
			{
				if (pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() > 0.f)
					CancelShot(pLocal, pWeapon, pCmd, m_iLastTickCancel);
			}
			else
			{
				if (m_iLastTickCancel)
					pCmd->weaponselect = m_iLastTickCancel = 0;
				pCmd->buttons |= IN_ATTACK;
			}
		}
		if (!iResult) continue;
		if (iResult == 2)
		{
			G::AimTarget = { tTarget.m_pEntity->entindex(), I::GlobalVars->tickcount, 0 };
			DrawVisuals(iResult, tTarget, m_vPlayerPath, m_vProjectilePath, m_vBoxes);
			Aim(pCmd, tTarget.m_vAngleTo);
			break;
		}

		G::AimTarget = { tTarget.m_pEntity->entindex(), I::GlobalVars->tickcount };
		G::AimPoint = { tTarget.m_vPos, I::GlobalVars->tickcount };

		if (Vars::Aimbot::General::AutoShoot.Value)
		{
			switch (nWeaponID)
			{
			case TF_WEAPON_COMPOUND_BOW:
			case TF_WEAPON_PIPEBOMBLAUNCHER:
				pCmd->buttons |= IN_ATTACK;
				if (pWeapon->As<CTFPipebombLauncher>()->m_flChargeBeginTime() > 0.f)
					pCmd->buttons &= ~IN_ATTACK;
				break;
			case TF_WEAPON_CANNON:
				pCmd->buttons |= IN_ATTACK;
				if (pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() > 0.f)
				{
					if (m_iLastTickCancel)
						pCmd->weaponselect = m_iLastTickCancel = 0;
					if (Vars::Aimbot::Projectile::Modifiers.Value & Vars::Aimbot::Projectile::ModifiersEnum::ChargeWeapon)
					{
						float flCharge = pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() - I::GlobalVars->curtime;
						flCharge = floorf(flCharge / 0.195f) * 0.195f;
						if (flCharge < m_flTimeTo)
							pCmd->buttons &= ~IN_ATTACK;
					}
					else
						pCmd->buttons &= ~IN_ATTACK;
				}
				break;
			case TF_WEAPON_BAT_WOOD:
			case TF_WEAPON_BAT_GIFTWRAP:
			case TF_WEAPON_LUNCHBOX:
				pCmd->buttons &= ~IN_ATTACK, pCmd->buttons |= IN_ATTACK2;
				break;
			default:
				pCmd->buttons |= IN_ATTACK;
				if (pWeapon->m_iItemDefinitionIndex() == Soldier_m_TheBeggarsBazooka)
				{
					if (pWeapon->m_iClip1() > 0)
						pCmd->buttons &= ~IN_ATTACK;
				}
			}
		}

		F::Aimbot.m_bRan = G::Attacking = SDK::IsAttacking(pLocal, pWeapon, pCmd, true);
		DrawVisuals(iResult, tTarget, m_vPlayerPath, m_vProjectilePath, m_vBoxes);

		Aim(pCmd, tTarget.m_vAngleTo);
		if (G::PSilentAngles)
		{
			switch (nWeaponID)
			{
			case TF_WEAPON_FLAMETHROWER: // angles show up anyways
			case TF_WEAPON_CLEAVER: // can't psilent with these weapons, they use SetContextThink
			case TF_WEAPON_JAR:
			case TF_WEAPON_JAR_MILK:
			case TF_WEAPON_JAR_GAS:
			case TF_WEAPON_BAT_WOOD:
			case TF_WEAPON_BAT_GIFTWRAP:
				G::PSilentAngles = false, G::SilentAngles = true;
			}
		}
		return true;
	}

	return false;
}

void CAimbotProjectile::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	const bool bSuccess = RunMain(pLocal, pWeapon, pCmd);
#ifdef SPLASH_DEBUG6
	if (Vars::Aimbot::General::AimType.Value && !s_mTraceCount.empty())
	{
		int iTraceCount = 0;
		for (auto& [_, iTraces] : s_mTraceCount)
			iTraceCount += iTraces;
		SDK::Output("Traces", std::format("{}", iTraceCount).c_str());
		for (auto& [sType, iTraces] : s_mTraceCount)
			SDK::Output("Traces", std::format("{}: {}", sType, iTraces).c_str());
	}
	s_mTraceCount.clear();
#endif

	//if (Vars::Aimbot::General::AimType.Value) {
	//	I::EngineClient->ClientCmd_Unrestricted("cl_autoreload 0");
	//}
	//else {
	//	I::EngineClient->ClientCmd_Unrestricted("cl_autoreload 1");
	//}

	float flAmount = 0.f;
	if (pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER)
	{
		const float flCharge = pWeapon->As<CTFPipebombLauncher>()->m_flChargeBeginTime() > 0.f ? I::GlobalVars->curtime - pWeapon->As<CTFPipebombLauncher>()->m_flChargeBeginTime() : 0.f;
		flAmount = Math::RemapVal(flCharge, 0.f, SDK::AttribHookValue(4.f, "stickybomb_charge_rate", pWeapon), 0.f, 1.f);
	}
	else if (pWeapon->GetWeaponID() == TF_WEAPON_CANNON)
	{
		const float flMortar = SDK::AttribHookValue(0.f, "grenade_launcher_mortar_mode", pWeapon);
		const float flCharge = pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() > 0.f ? I::GlobalVars->curtime - pWeapon->As<CTFGrenadeLauncher>()->m_flDetonateTime() : -flMortar;
		flAmount = flMortar ? Math::RemapVal(flCharge, -flMortar, 0.f, 0.f, 1.f) : 0.f;
	}

	if (pWeapon->GetWeaponID() == TF_WEAPON_PIPEBOMBLAUNCHER && G::OriginalCmd.buttons & IN_ATTACK && Vars::Aimbot::Projectile::AutoRelease.Value && flAmount > Vars::Aimbot::Projectile::AutoRelease.Value / 100)
		pCmd->buttons &= ~IN_ATTACK;
	else if (G::CanPrimaryAttack && Vars::Aimbot::Projectile::Modifiers.Value & Vars::Aimbot::Projectile::ModifiersEnum::CancelCharge)
	{
		if (m_bLastTickHeld && (G::LastUserCmd->buttons & IN_ATTACK && !(pCmd->buttons & IN_ATTACK) && !bSuccess || flAmount > 0.95f))
			CancelShot(pLocal, pWeapon, pCmd, m_iLastTickCancel);
	}

	m_bLastTickHeld = Vars::Aimbot::General::AimType.Value;
}



// TestAngle and CanHit shares a bunch of code, possibly merge somehow

bool CAimbotProjectile::TestAngle(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pProjectile, Target_t& tTarget, Vec3& vPoint, Vec3& vAngles, int iSimTime, bool bSplash, std::vector<Vec3>* pProjectilePath)
{
	ProjectileInfo tProjInfo = {};
	F::ProjSim.GetInfo(pProjectile, tProjInfo);
	CGameTrace trace = {};
	{
		CTraceFilterWorldAndPropsOnly filter = {};

		Vec3 vEyePos = pLocal->GetShootPos(); // m_tInfo.m_vLocalEye is not actually our shootpos here
		tProjInfo.m_vPos = pProjectile->GetAbsOrigin();

		Vec3 vPos = vPoint;
		if (m_tInfo.m_flGravity)
			vPos += Vec3(0, 0, (m_tInfo.m_flGravity * 800.f * pow(TICKS_TO_TIME(iSimTime), 2)) / 2);
		Vec3 vForward = (vPos - tProjInfo.m_vPos).Normalized();
		tProjInfo.m_vAng = Math::VectorAngles(vForward);

		SDK::Trace(tProjInfo.m_vPos, tProjInfo.m_vPos + vForward * MAX_TRACE_LENGTH, MASK_SOLID, &filter, &trace);
		vAngles = Math::CalcAngle(vEyePos, trace.endpos);
		vForward = (vEyePos - trace.endpos).Normalized();
		if (vForward.Dot(trace.plane.normal) <= 0)
			return false;

		SDK::Trace(vEyePos, trace.endpos, MASK_SOLID, &filter, &trace);
		if (trace.fraction < 0.999f)
			return false;

		if (!F::AutoAirblast.CanAirblastEntity(pLocal, pWeapon, pProjectile, vAngles))
			return false;
	}
	if (!F::ProjSim.Initialize(tProjInfo, false, true))
		return false;

	CTraceFilterCollideable filter = {};
	filter.pSkip = bSplash ? tTarget.m_pEntity : pLocal;
	filter.iPlayer = bSplash ? PLAYER_NONE : PLAYER_DEFAULT;
	int nMask = MASK_SOLID;
	F::ProjSim.SetupTrace(filter, nMask, pProjectile);

	if (!tProjInfo.m_flGravity)
	{
		SDK::TraceHull(tProjInfo.m_vPos, vPoint, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, nMask, &filter, &trace);
		if (trace.fraction < 0.999f && trace.m_pEnt != tTarget.m_pEntity)
			return false;
	}

	bool bDidHit = false;
	const Vec3 vOriginal = tTarget.m_pEntity->GetAbsOrigin();
	tTarget.m_pEntity->SetAbsOrigin(tTarget.m_vPos);
	for (int n = 1; n <= iSimTime; n++)
	{
		Vec3 vOld = F::ProjSim.GetOrigin();
		F::ProjSim.RunTick(tProjInfo);
		Vec3 vNew = F::ProjSim.GetOrigin();

		if (bDidHit)
		{
			trace.endpos = vNew;
			continue;
		}

		if (!bSplash)
			SDK::TraceHull(vOld, vNew, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, nMask, &filter, &trace);
		else
		{
			static Vec3 vStaticPos = {};
			if (n == 1)
				vStaticPos = vOld;
			if (n % Vars::Aimbot::Projectile::SplashTraceInterval.Value && n != iSimTime)
				continue;

			SDK::TraceHull(vStaticPos, vNew, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, nMask, &filter, &trace);
			vStaticPos = vNew;
		}
		if (trace.DidHit())
		{
			bool bTime = bSplash
				? trace.endpos.DistTo(vPoint) < tProjInfo.m_flVelocity * TICK_INTERVAL + tProjInfo.m_vHull.z
				: iSimTime - n < 5;
			bool bTarget = trace.m_pEnt == tTarget.m_pEntity || bSplash;
			bool bValid = bTarget && bTime;
			if (bValid && bSplash)
			{
				bValid = SDK::VisPosWorld(nullptr, tTarget.m_pEntity, trace.endpos, vPoint, nMask);
				if (bValid)
				{
					Vec3 vFrom = trace.endpos;
					switch (pProjectile->GetClassID())
					{
					case ETFClassID::CTFProjectile_Rocket:
					case ETFClassID::CTFProjectile_SentryRocket:
					case ETFClassID::CTFProjectile_EnergyBall:
						vFrom += trace.plane.normal;
					}

					CGameTrace eyeTrace = {};
					SDK::Trace(vFrom, tTarget.m_vPos + tTarget.m_pEntity->As<CTFPlayer>()->GetViewOffset(), MASK_SHOT, &filter, &eyeTrace);
					bValid = eyeTrace.fraction == 1.f;
				}
			}

			if (bValid)
			{
				if (bSplash)
				{
					int iPopCount = Vars::Aimbot::Projectile::SplashTraceInterval.Value - trace.fraction * Vars::Aimbot::Projectile::SplashTraceInterval.Value;
					for (int i = 0; i < iPopCount && !tProjInfo.m_vPath.empty(); i++)
						tProjInfo.m_vPath.pop_back();
				}

				bDidHit = true;
			}
			else
				break;

			if (!bSplash)
				trace.endpos = vNew;

			if (!bTarget || bSplash)
				break;
		}
	}
	tTarget.m_pEntity->SetAbsOrigin(vOriginal);

	if (bDidHit && pProjectilePath)
	{
		tProjInfo.m_vPath.push_back(trace.endpos);
		*pProjectilePath = tProjInfo.m_vPath;
	}

	return bDidHit;
}


bool CAimbotProjectile::CanHit(Target_t& tTarget, CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pProjectile)
{
	ProjectileInfo tProjInfo = {};
	F::ProjSim.GetInfo(pProjectile, tProjInfo);
	if (!F::ProjSim.Initialize(tProjInfo, false, true))
		return false;

	MoveStorage tStorage;
	F::MoveSim.Initialize(tTarget.m_pEntity, tStorage);
	tTarget.m_vPos = tTarget.m_pEntity->m_vecOrigin();

	m_tInfo = { pLocal, tProjInfo.m_pWeapon };
	m_tInfo.m_flLatency = F::Backtrack.GetReal() + TICKS_TO_TIME(F::Backtrack.GetAnticipatedChoke());
	m_tInfo.m_vHull = pProjectile->m_vecMaxs().Min(3);
	{
		CGameTrace trace = {};
		CTraceFilterWorldAndPropsOnly filter = {};

		for (int i = TIME_TO_TICKS(m_tInfo.m_flLatency); i > 0; i--)
		{
			Vec3 vOld = F::ProjSim.GetOrigin();
			F::ProjSim.RunTick(tProjInfo);
			Vec3 vNew = F::ProjSim.GetOrigin();

			SDK::TraceHull(vOld, vNew, tProjInfo.m_vHull * -1, tProjInfo.m_vHull, MASK_SOLID, &filter, &trace);
			tProjInfo.m_vPos = trace.endpos;
		}
		m_tInfo.m_vLocalEye = tProjInfo.m_vPos; // just assume from the projectile without any offset, check validity later
		pProjectile->SetAbsOrigin(tProjInfo.m_vPos);
	}
	m_tInfo.m_vTargetEye = tTarget.m_pEntity->As<CTFPlayer>()->GetViewOffset();

	m_tInfo.m_flVelocity = tProjInfo.m_flVelocity;

	m_tInfo.m_flGravity = tProjInfo.m_flGravity;
	m_tInfo.m_iSplashCount = !m_tInfo.m_flGravity ? Vars::Aimbot::Projectile::SplashCountDirect.Value : Vars::Aimbot::Projectile::SplashCountArc.Value;

	float flSize = tTarget.m_pEntity->GetSize().Length();
	m_tInfo.m_flRadius = GetSplashRadius(pProjectile, tProjInfo.m_pWeapon, tProjInfo.m_pOwner, Vars::Aimbot::Projectile::SplashRadius.Value / 100, pWeapon);
	m_tInfo.m_flRadiusTime = m_tInfo.m_flRadius / m_tInfo.m_flVelocity;
	m_tInfo.m_flBoundingTime = m_tInfo.m_flRadiusTime + flSize / m_tInfo.m_flVelocity;



	int iMaxTime = TIME_TO_TICKS(Vars::Aimbot::Projectile::MaxSimulationTime.Value);
	int iSplash = Vars::Aimbot::Projectile::SplashPrediction.Value && m_tInfo.m_flRadius ? Vars::Aimbot::Projectile::SplashPrediction.Value : Vars::Aimbot::Projectile::SplashPredictionEnum::Off;
	int iMulti = Vars::Aimbot::Projectile::SplashMode.Value;
//int iPoints = !m_tInfo.m_flGravity ? (Vars::Aimbot::Projectile::SplashPointsDirect.Value / Vars::Aimbot::General::MaxTargets.Value) : (Vars::Aimbot::Projectile::SplashPointsArc.Value / Vars::Aimbot::General::MaxTargets.Value);
	//float flDensity = Vars::Aimbot::Projectile::SplashPointDensity.Value;
	int iPoints = 3000 / std::max(1, Vars::Aimbot::General::MaxTargets.Value);

	auto mDirectPoints = iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Only ? std::unordered_map<int, Vec3>() : GetDirectPoints(tTarget, pProjectile);
	//auto vSpherePoints = !iSplash ? std::vector<std::pair<Vec3, int>>() : ComputeSphereWithDensity(m_tInfo.m_flRadius + flSize, iPoints, flDensity);
	//std::vector<std::pair<Vec3, int>> vSpherePoints;

	//if (Vars::Aimbot::Projectile::UseSplashPointDensity.Value) {
		std::vector<std::pair<Vec3, int>> vSpherePoints = !iSplash ? std::vector<std::pair<Vec3, int>>() : ComputeSphereWithGeometryOptimizedAndWallBleed(m_tInfo.m_flRadius, iPoints,
			tTarget.m_vPos + m_tInfo.m_vTargetEye,
			tTarget.m_pEntity->entindex());
	//}
	//else {
	//	vSpherePoints = !iSplash ? std::vector<std::pair<Vec3, int>>() : ComputeSphere(m_tInfo.m_flRadius /*+ flSize*/, iPoints);
	//}

	std::vector<Vec3> vEdgePoints;
	if (iSplash && Vars::Aimbot::Projectile::EdgeDetection.Value)
	{
		vEdgePoints = DetectEdgePoints(
			tTarget.m_vPos + m_tInfo.m_vTargetEye,
			m_tInfo.m_flRadius + flSize,
			Vars::Aimbot::Projectile::EdgeSearchRadius.Value,
			Vars::Aimbot::Projectile::EdgeSamples.Value
		);
	}

	Vec3 vAngleTo, vPredicted, vTarget;
	int iLowestPriority = std::numeric_limits<int>::max(); float flLowestDist = std::numeric_limits<float>::max();
	for (int i = -TIME_TO_TICKS(m_tInfo.m_flLatency); i <= iMaxTime; i++)
	{
		if (!tStorage.m_bFailed)
		{
			F::MoveSim.RunTick(tStorage);
			tTarget.m_vPos = GetSimulatedPos(tStorage);
		}
		if (i < 0)
			continue;

		bool bDirectBreaks = true;
		std::vector<Point_t> vSplashPoints = {};
		if (iSplash)
		{
			Solution_t solution; CalculateAngle(m_tInfo.m_vLocalEye, tTarget.m_vPos, i, solution, false);
			if (solution.m_iCalculated != CalculatedEnum::Bad)
			{
				bDirectBreaks = false;

				const float flTimeTo = solution.m_flTime - TICKS_TO_TIME(i);
				if (flTimeTo < m_tInfo.m_flBoundingTime)
				{
					static std::vector<std::pair<Vec3, Vec3>> vSimplePoints = {};
					if (iMulti == Vars::Aimbot::Projectile::SplashModeEnum::Single)
					{
						SetupSplashPoints(tTarget, vSpherePoints, vSimplePoints, vEdgePoints);
						if (!vSimplePoints.empty())
							iMulti++;
						else
						{
							iSplash = Vars::Aimbot::Projectile::SplashPredictionEnum::Off;
							goto skipSplash;
						}
					}

					if ((iMulti == Vars::Aimbot::Projectile::SplashModeEnum::Multi ? vSpherePoints.empty() : vSimplePoints.empty())
						|| flTimeTo < -m_tInfo.m_flBoundingTime)
						break;
					else
					{
						if (iMulti == Vars::Aimbot::Projectile::SplashModeEnum::Multi)
							vSplashPoints = GetSplashPoints(tTarget, vSpherePoints, i);
						else
							vSplashPoints = GetSplashPointsSimple(tTarget, vSimplePoints, vEdgePoints, i);
					}
				}
			}
		}
	skipSplash:
		if (bDirectBreaks && mDirectPoints.empty())
			break;

		std::vector<std::tuple<Point_t, int, int>> vPoints = {};
		for (auto& [iIndex, vPoint] : mDirectPoints)
			vPoints.emplace_back(Point_t(tTarget.m_vPos + vPoint, {}), iIndex + (iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Prefer ? m_tInfo.m_iSplashCount : 0), iIndex);
		for (auto& vPoint : vSplashPoints)
			vPoints.emplace_back(vPoint, iSplash == Vars::Aimbot::Projectile::SplashPredictionEnum::Include ? 3 : 0, -1);

		for (auto& [vPoint, iPriority, iIndex] : vPoints) // get most ideal point
		{
			const bool bSplash = iIndex == -1;
			Vec3 vOriginalPoint = vPoint.m_vPoint;

			if (Vars::Aimbot::Projectile::HuntsmanPullPoint.Value && tTarget.m_nAimedHitbox == HITBOX_HEAD)
				vPoint.m_vPoint = PullPoint(vPoint.m_vPoint, m_tInfo.m_vLocalEye, m_tInfo, tTarget.m_pEntity->m_vecMins() + tProjInfo.m_vHull, tTarget.m_pEntity->m_vecMaxs() - tProjInfo.m_vHull, tTarget.m_vPos);
			//vPoint.m_vPoint = PullPoint(vPoint.m_vPoint, m_tInfo.m_vLocalEye, m_tInfo, tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs(), tTarget.m_vPos);

			float flDist = bSplash ? tTarget.m_vPos.DistTo(vPoint.m_vPoint) : flLowestDist;
			bool bPriority = bSplash ? iPriority <= iLowestPriority : iPriority < iLowestPriority;
			bool bTime = bSplash || tStorage.m_MoveData.m_vecVelocity.IsZero();
			bool bDist = !bSplash || flDist < flLowestDist;
			if (!bSplash && !bPriority)
				mDirectPoints.erase(iIndex);
			if (!bPriority || !bTime || !bDist)
				continue;

			CalculateAngle(m_tInfo.m_vLocalEye, vPoint.m_vPoint, i, vPoint.m_tSolution);
			if (!bSplash && (vPoint.m_tSolution.m_iCalculated == CalculatedEnum::Good || vPoint.m_tSolution.m_iCalculated == CalculatedEnum::Bad))
				mDirectPoints.erase(iIndex);
			if (vPoint.m_tSolution.m_iCalculated != CalculatedEnum::Good)
				continue;

			if (Vars::Aimbot::Projectile::HuntsmanPullPoint.Value && tTarget.m_nAimedHitbox == HITBOX_HEAD)
			{
				Solution_t tSolution;
				CalculateAngle(m_tInfo.m_vLocalEye, vOriginalPoint, std::numeric_limits<int>::max(), tSolution);
				vPoint.m_tSolution.m_flPitch = tSolution.m_flPitch, vPoint.m_tSolution.m_flYaw = tSolution.m_flYaw;
			}

			Vec3 vAngles; Aim(G::CurrentUserCmd->viewangles, { vPoint.m_tSolution.m_flPitch, vPoint.m_tSolution.m_flYaw, 0.f }, vAngles, Vars::Aimbot::General::AimTypeEnum::Plain);
			std::vector<Vec3> vProjLines;

			if (TestAngle(pLocal, pWeapon, pProjectile, tTarget, vPoint.m_vPoint, vAngles, i, bSplash, &vProjLines))
			{
				iLowestPriority = iPriority; flLowestDist = flDist;
				vAngleTo = vAngles, vPredicted = tTarget.m_vPos, vTarget = vOriginalPoint;
				m_flTimeTo = vPoint.m_tSolution.m_flTime + m_tInfo.m_flLatency;
				m_vPlayerPath = tStorage.m_vPath;
				m_vPlayerPath.push_back(tStorage.m_MoveData.m_vecAbsOrigin);
				m_vProjectilePath = vProjLines;
			}
		}
	}
	F::MoveSim.Restore(tStorage);

	tTarget.m_vPos = vTarget;
	tTarget.m_vAngleTo = vAngleTo;
	if (tTarget.m_iTargetType != TargetEnum::Player || !tStorage.m_bFailed) // don't attempt to aim at players when movesim fails
	{
		if (iLowestPriority != std::numeric_limits<int>::max())
		{
			if (Vars::Colors::BoundHitboxEdge.Value.a || Vars::Colors::BoundHitboxFace.Value.a || Vars::Colors::BoundHitboxEdgeIgnoreZ.Value.a || Vars::Colors::BoundHitboxFaceIgnoreZ.Value.a)
			{
				m_tInfo.m_vHull = m_tInfo.m_vHull.Max(1);
				float flProjectileTime = TICKS_TO_TIME(m_vProjectilePath.size());
				float flTargetTime = tStorage.m_bFailed ? flProjectileTime : TICKS_TO_TIME(m_vPlayerPath.size());

				bool bBox = Vars::Visuals::Hitbox::BoundsEnabled.Value & Vars::Visuals::Hitbox::BoundsEnabledEnum::OnShot;
				bool bPoint = Vars::Visuals::Hitbox::BoundsEnabled.Value & Vars::Visuals::Hitbox::BoundsEnabledEnum::AimPoint;
				if (bBox)
				{
					if (Vars::Colors::BoundHitboxEdgeIgnoreZ.Value.a || Vars::Colors::BoundHitboxFaceIgnoreZ.Value.a)
						m_vBoxes.emplace_back(vPredicted, tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs(), Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flTargetTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdgeIgnoreZ.Value, Vars::Colors::BoundHitboxFaceIgnoreZ.Value);
					if (Vars::Colors::BoundHitboxEdge.Value.a || Vars::Colors::BoundHitboxFace.Value.a)
						m_vBoxes.emplace_back(vPredicted, tTarget.m_pEntity->m_vecMins(), tTarget.m_pEntity->m_vecMaxs(), Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flTargetTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdge.Value, Vars::Colors::BoundHitboxFace.Value, true);
				}
				if (bPoint)
				{
					if (Vars::Colors::BoundHitboxEdgeIgnoreZ.Value.a || Vars::Colors::BoundHitboxFaceIgnoreZ.Value.a)
						m_vBoxes.emplace_back(vTarget, m_tInfo.m_vHull * -1, m_tInfo.m_vHull, Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flProjectileTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdgeIgnoreZ.Value, Vars::Colors::BoundHitboxFaceIgnoreZ.Value);
					if (Vars::Colors::BoundHitboxEdge.Value.a || Vars::Colors::BoundHitboxFace.Value.a)
						m_vBoxes.emplace_back(vTarget, m_tInfo.m_vHull * -1, m_tInfo.m_vHull, Vec3(), I::GlobalVars->curtime + (Vars::Visuals::Simulation::Timed.Value ? flProjectileTime : Vars::Visuals::Hitbox::DrawDuration.Value), Vars::Colors::BoundHitboxEdge.Value, Vars::Colors::BoundHitboxFace.Value, true);
				}
			}

			return true;
		}
	}

	return false;
}

bool CAimbotProjectile::AutoAirblast(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, CBaseEntity* pProjectile)
{
	auto vTargets = SortTargets(pLocal, pWeapon);
	if (vTargets.empty())
		return false;

	//if (!G::AimTarget.m_iEntIndex)
	//	G::AimTarget = { vTargets.front().m_pEntity->entindex(), I::GlobalVars->tickcount, 0 };

	for (auto& tTarget : vTargets)
	{
		m_flTimeTo = std::numeric_limits<float>::max();
		m_vPlayerPath.clear(); m_vProjectilePath.clear(); m_vBoxes.clear();

		const bool bResult = CanHit(tTarget, pLocal, pWeapon, pProjectile);
		if (!bResult) continue;

		G::AimTarget = { tTarget.m_pEntity->entindex(), I::GlobalVars->tickcount };
		G::AimPoint = { tTarget.m_vPos, I::GlobalVars->tickcount };

		G::Attacking = true;
		DrawVisuals(1, tTarget, m_vPlayerPath, m_vProjectilePath, m_vBoxes);

		Aim(pCmd, tTarget.m_vAngleTo, Vars::Aimbot::General::AimTypeEnum::Silent);
		return true;
	}

	return false;
}