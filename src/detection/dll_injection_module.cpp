#include "detection/detection_system.h"

#include "igameevents.h"
#include "movement_analysis/player_context.h"
#include "utils/interfaces.h"
#include "utils/utils.h"

#include <bitset>

CConVar<bool> cs2ac_dll_injection_debug("cs2ac_dll_injection_debug", FCVAR_NONE, "Show DLL Injection scan timing, availability, and matches", false);

#define DLL_INJECTION_DEBUG(...) \
	do \
	{ \
		if (cs2ac_dll_injection_debug.GetBool()) \
			Msg("[CS2AC DLL Injection] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr auto initialScanDelay = std::chrono::seconds(10);
	constexpr auto scanInterval = std::chrono::minutes(2);

	// Keep normal client and game-instructor subscriptions out of this list.
	constexpr std::array<const char *, 117> blacklistedEvents = {
		"gameui_hidden",
		"player_chat",
		"player_score",
		"player_shoot",
		"game_init",
		"game_start",
		"game_end",
		"ugc_map_info_received",
		"ugc_map_unsubscribed",
		"ugc_map_download_error",
		"ugc_file_download_finished",
		"ugc_file_download_start",
		"dm_bonus_weapon_start",
		"survival_announce_phase",
		"reset_game_titledata",
		"vote_ended",
		"vote_started",
		"vote_options",
		"endmatch_mapvote_selecting_map",
		"endmatch_cmm_start_reveal_items",
		"inventory_updated",
		"client_loadout_changed",
		"add_player_sonar_icon",
		"door_open",
		"door_closed",
		"door_break",
		"bullet_damage",
		"hostage_call_for_help",
		"vip_escaped",
		"vip_killed",
		"silencer_detach",
		"inspect_weapon",
		"player_spawned",
		"item_pickup_slerp",
		"item_pickup_failed",
		"enter_rescue_zone",
		"exit_rescue_zone",
		"silencer_off",
		"silencer_on",
		"round_poststart",
		"tagrenade_detonate",
		"inferno_extinguish",
		"mb_input_lock_success",
		"mb_input_lock_cancel",
		"nav_generate",
		"achievement_info_loaded",
		"hltv_changed_mode",
		"show_deathpanel",
		"hide_deathpanel",
		"player_avenged_teammate",
		"achievement_earned_local",
		"repost_xbox_achievements",
		"match_end_conditions",
		"write_profile_data",
		"trial_time_expired",
		"update_matchmaking_stats",
		"enable_restart_voting",
		"sfuievent",
		"start_vote",
		"teamchange_pending",
		"material_default_complete",
		"cs_prev_next_spectator",
		"tournament_reward",
		"start_halftime",
		"ammo_refill",
		"parachute_pickup",
		"parachute_deploy",
		"dronegun_attack",
		"drone_dispatched",
		"loot_crate_visible",
		"loot_crate_opened",
		"open_crate_instr",
		"smoke_beacon_paradrop",
		"drone_cargo_detached",
		"drone_above_roof",
		"dz_item_interaction",
		"survival_teammate_respawn",
		"guardian_wave_restart",
		"bullet_flight_resolution",
		"server_message",
		"player_full_update",
		"local_player_controller_team",
		"local_player_pawn_changed",
		"ragdoll_dissolved",
		"team_info",
		"team_score",
		"hltv_rank_camera",
		"hltv_rank_entity",
		"demo_stop",
		"map_shutdown",
		"hostname_changed",
		"game_message",
		"round_start_pre_entity",
		"round_start_post_nav",
		"player_hintmessage",
		"broken_breakable",
		"door_close",
		"vote_failed",
		"vote_passed",
		"vote_cast_yes",
		"vote_cast_no",
		"achievement_event",
		"achievement_write_failed",
		"bonus_updated",
		"flare_ignite_npc",
		"helicopter_grenade_punt_miss",
		"physgun_pickup",
		"cart_updated",
		"store_pricesheet_updated",
		"item_schema_initialized",
		"drop_rate_modified",
		"event_ticket_modified",
		"gc_connected",
		"instructor_start_lesson",
		"instructor_close_lesson",
		"clientside_lesson_closed",
		"dynamic_shadow_light_changed",
	};

	static_assert(blacklistedEvents.size() == 117, "The DLL Injection event list must contain 117 entries.");
} // namespace

namespace detection
{
	void DllInjectionModule::Load(AnnounceCallback callback)
	{
		announce = callback;
		Reset();
	}

	void DllInjectionModule::Unload()
	{
		nextScans = {};
		announce = nullptr;
	}

	void DllInjectionModule::Reset()
	{
		nextScans = {};
	}

	void DllInjectionModule::OnGameFrame()
	{
		auto now = std::chrono::steady_clock::now();
		if (!announce || !interfaces::pGameEventManager || !g_pCS2ACPlayerManager || !g_pCS2ACUtils)
		{
			return;
		}

		bool scannedPlayerThisFrame = false;
		for (u32 index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *player = g_pCS2ACPlayerManager->ToPlayer(index);
			if (!player || !player->IsConnected() || !player->IsInGame() || player->IsFakeClient() || player->IsCSTV())
			{
				continue;
			}

			const int slot = player->GetPlayerSlot().Get();
			if (slot < 0 || slot >= MAXPLAYERS)
			{
				continue;
			}

			auto &nextScan = nextScans[slot];
			if (nextScan == Clock::time_point {})
			{
				nextScan = now + initialScanDelay;
				DLL_INJECTION_DEBUG("%s will receive the first subscription scan in 10 seconds.\n", player->GetName());
				continue;
			}
			if (now < nextScan)
			{
				continue;
			}
			if (scannedPlayerThisFrame)
			{
				continue;
			}

			auto *listener = g_pCS2ACUtils->GetLegacyGameEventListener(player->GetPlayerSlot());
			if (!listener)
			{
				nextScan = now + initialScanDelay;
				DLL_INJECTION_DEBUG("%s has no legacy event listener yet; retrying in 10 seconds.\n", player->GetName());
				continue;
			}
			scannedPlayerThisFrame = true;
			nextScan = now + scanInterval;

			std::bitset<blacklistedEvents.size()> current;
			for (size_t eventIndex = 0; eventIndex < blacklistedEvents.size(); ++eventIndex)
			{
				if (!interfaces::pGameEventManager->FindListener(listener, blacklistedEvents[eventIndex]))
				{
					continue;
				}

				current.set(eventIndex);
			}
			const bool detected = current.any();
			DLL_INJECTION_DEBUG("%s scan completed: %zu of %zu blacklisted subscriptions found; next scan in 2 minutes.\n", player->GetName(),
								current.count(), blacklistedEvents.size());

			if (!detected)
			{
				continue;
			}

			std::string matches;
			for (size_t eventIndex = 0; eventIndex < blacklistedEvents.size(); ++eventIndex)
			{
				if (!current.test(eventIndex))
				{
					continue;
				}
				if (!matches.empty())
				{
					matches += ", ";
				}
				matches += blacklistedEvents[eventIndex];
				if (matches.size() > 700)
				{
					matches += ", ...";
					break;
				}
			}
			announce("DLL INJECTION", player,
					 localization::Format(current.count() == 1 ? "evidence.dll_injection.one" : "evidence.dll_injection.many",
										  current.count() == 1 ? "{count} blacklisted client event subscription found: {events}."
															   : "{count} blacklisted client event subscriptions found: {events}.",
										  {{"count", tfm::format("%zu", current.count())}, {"events", matches}}));
		}
	}

	void DllInjectionModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (!player)
		{
			return;
		}

		const int slot = player->GetPlayerSlot().Get();
		if (slot >= 0 && slot < MAXPLAYERS)
		{
			nextScans[slot] = {};
		}
	}
} // namespace detection
