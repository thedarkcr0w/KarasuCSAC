#include "hooks.h"

#include "cs2ac.h"
#include "movement_analysis/player_context.h"
#include "movement_analysis/events/movement_events.h"
#include "utils/ctimer.h"
#include "utils/gameconfig.h"
#include "utils/interfaces.h"
#include "utils/utils.h"

#include "igameevents.h"
#include "iserver.h"

SH_DECL_MANUALHOOK3_void(Teleport, 0, 0, 0, const Vector *, const QAngle *, const Vector *);
SH_DECL_HOOK3_void(ISource2Server, GameFrame, SH_NOATTRIB, false, bool, bool, bool);
SH_DECL_HOOK1_void(ISource2GameClients, ClientFullyConnect, SH_NOATTRIB, false, CPlayerSlot);
SH_DECL_HOOK1_void(ISource2GameClients, ClientSettingsChanged, SH_NOATTRIB, false, CPlayerSlot);
SH_DECL_HOOK4_void(ISource2GameClients, ClientActive, SH_NOATTRIB, false, CPlayerSlot, bool, const char *, uint64);
SH_DECL_HOOK5_void(ISource2GameClients, ClientDisconnect, SH_NOATTRIB, false, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64,
				   const char *);
SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, false, bool, IGameEvent *, bool);

namespace
{
	struct HookEntry
	{
		i32 id;
		const char *name;
	};

	CUtlVector<HookEntry> hookIds;
	i32 gameFrameHookId {};
	i32 teleportHooks[MAXPLAYERS] {};

	struct PendingGameEvent
	{
		IGameEvent *event;
		MovementPlayer *player;
	};

	std::vector<PendingGameEvent> pendingGameEvents;
	constexpr unsigned watermarkRoundInterval = 5;
	unsigned completedRounds {};
	bool AddTeleportHook(MovementPlayer *player);

	bool IsConsumedEvent(IGameEvent *event)
	{
		if (!event)
		{
			return false;
		}
		const char *name = event->GetName();
		return CS2AC_STREQ(name, "weapon_fire") || CS2AC_STREQ(name, "bullet_impact") || CS2AC_STREQ(name, "player_hurt")
			   || CS2AC_STREQ(name, "player_death") || CS2AC_STREQ(name, "player_spawn") || CS2AC_STREQ(name, "round_end");
	}

	MovementPlayer *ResolveEventPlayer(IGameEvent *event)
	{
		if (!event)
		{
			return nullptr;
		}
		if (CS2AC_STREQ(event->GetName(), "bullet_impact"))
		{
			const int userID = event->GetInt("userid", -1);
			return userID < 0 ? nullptr : g_CS2AC.ResolveImpactShooter(userID);
		}

		auto *player = g_pCS2ACPlayerManager->ToPlayer(static_cast<CBasePlayerController *>(event->GetPlayerController("userid")));
		if (player)
		{
			return player;
		}

		int userID = event->GetInt("userid", -1);
		return userID < 0 ? nullptr : g_pCS2ACPlayerManager->ToPlayer(CPlayerUserId(userID));
	}

	bool HookFireEventBefore(IGameEvent *event, bool)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META_VALUE(MRES_IGNORED, true);
		}
		PendingGameEvent pending {};
		if (IsConsumedEvent(event))
		{
			pending = {interfaces::pGameEventManager->DuplicateEvent(event), ResolveEventPlayer(event)};
		}
		pendingGameEvents.push_back(pending);
		RETURN_META_VALUE(MRES_IGNORED, true);
	}

	bool HookFireEventAfter(IGameEvent *, bool)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META_VALUE(MRES_IGNORED, true);
		}
		PendingGameEvent pending {};
		if (!pendingGameEvents.empty())
		{
			pending = pendingGameEvents.back();
			pendingGameEvents.pop_back();
		}
		if (pending.event)
		{
			if (CS2AC_STREQ(pending.event->GetName(), "round_end") && ++completedRounds % watermarkRoundInterval == 0)
			{
				utils::AnnounceWatermark();
			}
			g_CS2AC.OnGameEvent(pending.event, pending.player);
			if (CS2AC_STREQ(pending.event->GetName(), "player_spawn") && pending.player)
			{
				// A spawn can replace the pawn object after ClientActive, so bind the per-pawn hook again.
				pending.player->OnTeleport(nullptr, nullptr, nullptr);
				AddTeleportHook(pending.player);
			}
			interfaces::pGameEventManager->FreeEvent(pending.event);
		}
		RETURN_META_VALUE(MRES_IGNORED, true);
	}

	void HookTeleport(const Vector *origin, const QAngle *angles, const Vector *velocity)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		auto *pawn = META_IFACEPTR(CCSPlayerPawn);
		auto *current = g_pCS2ACPlayerManager->ToPlayer(static_cast<CBasePlayerPawn *>(pawn));
		if (current)
		{
			current->OnTeleport(origin, angles, velocity);
		}
		RETURN_META(MRES_IGNORED);
	}

	bool RemoveTeleportHook(CPlayerSlot slot)
	{
		if (slot.Get() < 0 || slot.Get() >= MAXPLAYERS || !teleportHooks[slot.Get()])
		{
			return true;
		}
		if (!SH_REMOVE_HOOK_ID(teleportHooks[slot.Get()]))
		{
			Warning("[CS2AC] The player teleport hook for slot %d could not be removed yet. Metamod will try again during unload.\n", slot.Get());
			return false;
		}
		teleportHooks[slot.Get()] = 0;
		return true;
	}

	bool AddTeleportHook(MovementPlayer *player)
	{
		if (!player || !player->GetPlayerPawn())
		{
			return false;
		}
		if (!RemoveTeleportHook(player->GetPlayerSlot()))
		{
			return false;
		}
		teleportHooks[player->GetPlayerSlot().Get()] = SH_ADD_MANUALHOOK(Teleport, player->GetPlayerPawn(), SH_STATIC(HookTeleport), false);
		if (!teleportHooks[player->GetPlayerSlot().Get()])
		{
			Warning("[CS2AC] Player teleport tracking could not be attached for %s.\n", player->GetName());
			return false;
		}
		return true;
	}

	void HookGameFrameBefore(bool, bool, bool)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		if (auto *globals = g_pCS2ACUtils->GetGlobals())
		{
			g_CS2AC.serverGlobals = *globals;
		}
		RETURN_META(MRES_IGNORED);
	}

	void HookGameFrameAfter(bool simulating, bool, bool)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		if (auto *globals = g_pCS2ACUtils->GetGlobals())
		{
			g_CS2AC.serverGlobals = *globals;
		}
		g_CS2AC.OnGameFrame(simulating);
		ProcessTimers();
		MovementEventService::ActiveCheck();
		RETURN_META(MRES_IGNORED);
	}

	void HookClientFullyConnect(CPlayerSlot slot)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		g_pCS2ACPlayerManager->OnClientFullyConnect(slot);
		g_ClientCvarValue.OnClientFullyConnected(slot, g_pCS2ACPlayerManager->ToPlayer(slot)->IsFakeClient());
		g_CS2AC.OnClientFullyConnect(slot);
		RETURN_META(MRES_IGNORED);
	}

	void HookClientSettingsChanged(CPlayerSlot slot)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		g_CS2AC.OnClientSettingsChanged(slot);
		RETURN_META(MRES_IGNORED);
	}

	void HookClientActive(CPlayerSlot slot, bool, const char *, uint64 xuid)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		g_pCS2ACPlayerManager->OnClientActive(slot, xuid);
		auto *player = g_pCS2ACPlayerManager->ToPlayer(slot);
		if (player && player->GetPlayerPawn())
		{
			AddTeleportHook(player);
		}
		RETURN_META(MRES_IGNORED);
	}

	void HookClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason, const char *, uint64, const char *)
	{
		if (!g_CS2AC.IsLoaded())
		{
			RETURN_META(MRES_IGNORED);
		}
		RemoveTeleportHook(slot);
		g_ClientCvarValue.OnClientDisconnect(slot);
		g_CS2AC.OnClientDisconnect(slot);
		g_pCS2ACPlayerManager->OnClientDisconnect(slot);
		RETURN_META(MRES_IGNORED);
	}

} // namespace

bool hooks::Initialize(std::vector<std::string> &missing)
{
	SH_MANUALHOOK_RECONFIGURE(Teleport, g_pGameConfig->GetOffset("Teleport"), 0, 0);

	auto add = [&](i32 id, const char *name)
	{
		if (id)
		{
			hookIds.AddToTail({id, name});
		}
		else
		{
			missing.emplace_back(std::string("The ") + name + " server hook could not be started.");
		}
	};
	add(SH_ADD_HOOK(ISource2Server, GameFrame, interfaces::pServer, SH_STATIC(HookGameFrameBefore), false), "game frame preparation");
	gameFrameHookId = SH_ADD_HOOK(ISource2Server, GameFrame, interfaces::pServer, SH_STATIC(HookGameFrameAfter), true);
	if (!gameFrameHookId)
	{
		missing.emplace_back("The game frame server hook could not be started.");
	}
	add(SH_ADD_HOOK(ISource2GameClients, ClientFullyConnect, g_pSource2GameClients, SH_STATIC(HookClientFullyConnect), true),
		"fully connected player");
	add(SH_ADD_HOOK(ISource2GameClients, ClientSettingsChanged, g_pSource2GameClients, SH_STATIC(HookClientSettingsChanged), true),
		"player setting update");
	add(SH_ADD_HOOK(ISource2GameClients, ClientActive, g_pSource2GameClients, SH_STATIC(HookClientActive), true), "active player");
	add(SH_ADD_HOOK(ISource2GameClients, ClientDisconnect, g_pSource2GameClients, SH_STATIC(HookClientDisconnect), true), "disconnecting player");
	add(SH_ADD_HOOK(IGameEventManager2, FireEvent, interfaces::pGameEventManager, SH_STATIC(HookFireEventBefore), false), "game event preparation");
	add(SH_ADD_HOOK(IGameEventManager2, FireEvent, interfaces::pGameEventManager, SH_STATIC(HookFireEventAfter), true), "completed game event");

	if (!missing.empty())
	{
		Cleanup();
		return false;
	}
	return true;
}

void hooks::HookActivePlayers()
{
	for (i32 i = 1; i <= MAXPLAYERS; ++i)
	{
		auto *player = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(i));
		if (player && player->IsInGame() && player->GetPlayerPawn())
		{
			AddTeleportHook(player);
		}
	}
}

bool hooks::ResetMap()
{
	completedRounds = 0;
	bool removed = true;
	for (i32 slot = 0; slot < MAXPLAYERS; ++slot)
	{
		removed = RemoveTeleportHook(CPlayerSlot(slot)) && removed;
	}
	return removed;
}

bool hooks::Cleanup()
{
	bool removed = ResetMap();
	if (gameFrameHookId)
	{
		if (SH_REMOVE_HOOK_ID(gameFrameHookId))
		{
			gameFrameHookId = 0;
		}
		else
		{
			Warning("[CS2AC] The completed game frame hook could not be removed yet. Metamod will try again during unload.\n");
			removed = false;
		}
	}
	for (i32 i = hookIds.Count() - 1; i >= 0; --i)
	{
		if (SH_REMOVE_HOOK_ID(hookIds[i].id))
		{
			hookIds.Remove(i);
		}
		else
		{
			Warning("[CS2AC] The %s hook could not be removed yet. Metamod will try again during unload.\n", hookIds[i].name);
			removed = false;
		}
	}
	if (interfaces::pGameEventManager)
	{
		for (const auto &pending : pendingGameEvents)
		{
			if (pending.event)
			{
				interfaces::pGameEventManager->FreeEvent(pending.event);
			}
		}
	}
	pendingGameEvents.clear();
	return removed;
}
