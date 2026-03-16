#include "Events.h"

#include "../../Core/Core.h"
#include "../../Features/Aimbot/AutoHeal/AutoHeal.h"
#include "../../Features/Backtrack/Backtrack.h"
#include "../../Features/CheaterDetection/CheaterDetection.h"
#include "../../Features/CritHack/CritHack.h"
#include "../../Features/Misc/Misc.h"
#include "../../Features/PacketManip/AntiAim/AntiAim.h"
#include "../../Features/Output/Output.h"
#include "../../Features/Resolver/Resolver.h"
#include "../../Features/Visuals/Visuals.h"
#include "../../Features/BanChecker/BanChecker.h"

bool CEventListener::Initialize()
{
	g_BanChecker.Initialize("28020647bf76ae3e23354bfc1ea39fc7");


	std::vector<const char*> vEvents = {
		"client_beginconnect", "client_connected", "client_disconnect", "game_newmap", "teamplay_round_start", "scorestats_accumulated_update", "mvm_reset_stats", "player_connect_client", "player_spawn", "player_changeclass", "player_hurt", "vote_cast", "item_pickup", "revive_player_notify"
	};

	for (auto szEvent : vEvents)
	{
		I::GameEventManager->AddListener(this, szEvent, false);

		if (!I::GameEventManager->FindListener(this, szEvent))
		{
			U::Core.AppendFailText(std::format("Failed to add listener: {}", szEvent).c_str());
			m_bFailed = true;
		}
	}


	return !m_bFailed;
}

void CEventListener::Unload()
{
	g_BanChecker.Shutdown();

	I::GameEventManager->RemoveListener(this);
}

//#include "../src/Features/Aimbot/AimbotProjectile/BSPParser.h"

void CEventListener::FireGameEvent(IGameEvent* pEvent)
{
	if (!pEvent)
		return;

	auto pLocal = H::Entities.GetLocal();
	auto uHash = FNV1A::Hash32(pEvent->GetName());

	//static bool bTestedAPI = false;
	//if (!bTestedAPI && uHash == FNV1A::Hash32Const("player_spawn"))
	//{
	//	bTestedAPI = true;
	//	g_BanChecker.TestAPI();
	//}

	//if (uHash == FNV1A::Hash32Const("game_newmap"))
	//{
	//	g_BSPParser.OnLevelShutdown();

	//	const char* full = I::EngineClient->GetLevelName();
	//	I::CVar->ConsolePrintf("game_newmap fired — raw name: %s\n", full);

	//	if (full && full[0])
	//	{
	//		// strip "maps/"
	//		const char* name = full;
	//		if (std::strstr(full, "maps/") == full)
	//			name = full + 5;

	//		std::string mapName = name;

	//		// strip ".bsp"
	//		if (mapName.size() > 4 && mapName.ends_with(".bsp"))
	//			mapName = mapName.substr(0, mapName.size() - 4);

	//		I::CVar->ConsolePrintf("BSP Init called with cleaned map name: %s\n", mapName.c_str());

	//		g_BSPParser.OnLevelInit(mapName.c_str());
	//	}
	//}



	F::Output.Event(pEvent, uHash, pLocal);
	if (I::EngineClient->IsPlayingDemo())
		return;

	F::CritHack.Event(pEvent, uHash, pLocal);
	F::AutoHeal.Event(pEvent, uHash);
	F::Misc.Event(pEvent, uHash);
	F::Visuals.Event(pEvent, uHash);
	switch (uHash)
	{
	case FNV1A::Hash32Const("player_hurt"):
		F::Resolver.PlayerHurt(pEvent);
		F::CheaterDetection.ReportDamage(pEvent);
		break;
	case FNV1A::Hash32Const("player_spawn"):
		F::Backtrack.SetLerp(pEvent);
		break;
	case FNV1A::Hash32Const("revive_player_notify"):
	{
		if (!Vars::Misc::MannVsMachine::InstantRevive.Value || pEvent->GetInt("entindex") != I::EngineClient->GetLocalPlayer())
			break;

		KeyValues* kv = new KeyValues("MVM_Revive_Response");
		kv->SetBool("accepted", true);
		I::EngineClient->ServerCmdKeyValues(kv);
	}
	}
}