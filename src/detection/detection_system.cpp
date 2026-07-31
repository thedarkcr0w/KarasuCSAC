#include "detection/detection_system.h"

#include "gametrace.h"
#include "igameevents.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/entity/ccsplayerpawn.h"
#include "sdk/usercmd.h"
#include "settings.h"

#include "cs_gameevents.pb.h"

#include <algorithm>
#include <cmath>
#include <limits>

CConVarRef<bool> mp_teammates_are_enemies("mp_teammates_are_enemies");
extern CConVar<bool> cs2ac_silentaim_debug;

namespace
{
	constexpr size_t shotCommandLimit = 32;
	constexpr size_t pendingShotLimit = 8;
	constexpr size_t positionHistoryLimit = 128;
	constexpr int shotMatchTicks = 1;
	constexpr int shotLifetimeTicks = 2;
	bool cachedTeammatesAreEnemies {};
	static_assert(shotLifetimeTicks > shotMatchTicks, "Shots must survive long enough for every accepted event.");

	bool TeammatesAreEnemies()
	{
		return mp_teammates_are_enemies.IsValidRef() && mp_teammates_are_enemies.IsConVarDataAvailable() && mp_teammates_are_enemies.Get();
	}
} // namespace

namespace detection
{
	std::string_view NormalizeWeapon(std::string_view weapon)
	{
		if (weapon.rfind("weapon_", 0) == 0)
		{
			weapon.remove_prefix(7);
		}
		return weapon;
	}

	bool IsBallisticWeapon(std::string_view weapon)
	{
		weapon = NormalizeWeapon(weapon);
		static constexpr std::string_view weapons[] = {
			"ak47",    "aug",      "awp",      "bizon",         "cz75a", "deagle", "elite", "famas", "fiveseven", "g3sg1",        "galilar", "glock",
			"hkp2000", "m249",     "m4a1",     "m4a1_silencer", "mac10", "mag7",   "mp5sd", "mp7",   "mp9",       "negev",        "nova",    "p250",
			"p90",     "revolver", "sawedoff", "scar20",        "sg556", "ssg08",  "taser", "tec9",  "ump45",     "usp_silencer", "xm1014",
		};
		return std::find(std::begin(weapons), std::end(weapons), weapon) != std::end(weapons);
	}

	bool IsEligibleHuman(MovementPlayer *player)
	{
		return player && player->index >= 1 && player->index <= MAXPLAYERS && player->GetController() && !player->IsFakeClient() && !player->IsCSTV();
	}

	bool AreOpponents(int firstTeam, int secondTeam)
	{
		const bool firstIsPlaying = firstTeam == CS_TEAM_T || firstTeam == CS_TEAM_CT;
		const bool secondIsPlaying = secondTeam == CS_TEAM_T || secondTeam == CS_TEAM_CT;
		return firstIsPlaying && secondIsPlaying && (firstTeam != secondTeam || cachedTeammatesAreEnemies);
	}

	bool IsFinite(const QAngle &angles)
	{
		return std::isfinite(angles.x) && std::isfinite(angles.y) && std::isfinite(angles.z);
	}

	bool IsFinite(const Vector &vector)
	{
		return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
	}

	Vector AimForward(const QAngle &angles)
	{
		constexpr float degreesToRadians = static_cast<float>(M_PI / 180.0);
		const float pitch = angles.x * degreesToRadians;
		const float yaw = angles.y * degreesToRadians;
		const float cosinePitch = std::cos(pitch);
		return {cosinePitch * std::cos(yaw), cosinePitch * std::sin(yaw), -std::sin(pitch)};
	}

	float AngularDistance(const QAngle &first, const QAngle &second)
	{
		if (!IsFinite(first) || !IsFinite(second))
		{
			return std::numeric_limits<float>::quiet_NaN();
		}
		const float dot = std::clamp(DotProduct(AimForward(first), AimForward(second)), -1.0f, 1.0f);
		return static_cast<float>(std::acos(dot) * (180.0 / M_PI));
	}

	void ShotCorrelator::AdvanceGeneration(ShotPlayerData &data)
	{
		data.commands.clear();
		data.shots.clear();
		if (++data.generation == 0)
		{
			data.generation = 1;
		}
	}

	void ShotCorrelator::Reset()
	{
		for (auto &data : playerData)
		{
			AdvanceGeneration(data);
		}
		positionFrames.clear();
		nextShotId = 1;
	}

	void ShotCorrelator::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			AdvanceGeneration(playerData[player->index]);
			for (auto &frame : positionFrames)
			{
				frame.players[player->index] = {};
			}
		}
	}

	void ShotCorrelator::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
	{
		if (!IsEligibleHuman(player) || !commands || numCommands <= 0)
		{
			return;
		}

		auto &data = playerData[player->index];
		for (int i = 0; i < numCommands; ++i)
		{
			PlayerCommand &command = commands[i];
			const int attackIndex = command.attack1_start_history_index();
			if (!command.has_base() || !command.base().has_viewangles() || attackIndex < 0 || attackIndex >= command.input_history_size()
				|| !command.input_history(attackIndex).has_view_angles())
			{
				continue;
			}
			if (std::any_of(data.commands.rbegin(), data.commands.rend(),
							[&](const ShotCommand &stored) { return stored.commandNumber == command.cmdNum; }))
			{
				continue;
			}

			const auto &base = command.base();
			const auto &baseView = base.viewangles();
			const auto &attackView = command.input_history(attackIndex).view_angles();
			const QAngle baseAngles(baseView.x(), baseView.y(), baseView.z());
			const QAngle attackAngles(attackView.x(), attackView.y(), attackView.z());
			if (!IsFinite(baseAngles) || !IsFinite(attackAngles))
			{
				continue;
			}

			data.commands.push_back({command.cmdNum, base.client_tick(), -1, baseAngles, attackAngles});
			while (data.commands.size() > shotCommandLimit)
			{
				data.commands.pop_front();
			}
		}
	}

	void ShotCorrelator::OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick)
	{
		if (!IsEligibleHuman(player) || !command || !player->GetPlayerPawn())
		{
			return;
		}
		auto &data = playerData[player->index];
		auto found = std::find_if(data.commands.rbegin(), data.commands.rend(),
								  [&](const ShotCommand &stored) { return stored.commandNumber == command->cmdNum; });
		if (found == data.commands.rend())
		{
			return;
		}

		Vector eye;
		player->GetEyeOrigin(&eye);
		if (!IsFinite(eye))
		{
			return;
		}
		auto *pawn = player->GetPlayerPawn();
		const bool grounded = pawn->m_bOnGroundLastTick() || ((pawn->m_fFlags() & FL_ONGROUND) && pawn->m_hGroundEntity().IsValid());
		found->serverTick = currentTick;
		found->eyePosition = eye;
		found->airborne = !grounded && pawn->m_MoveType() == MOVETYPE_WALK && pawn->m_nActualMoveType() == MOVETYPE_WALK;
		found->scoped = pawn->m_bIsScoped();
		found->simulated = true;
	}

	void ShotCorrelator::CaptureFrame(int currentTick)
	{
		PositionFrame frame;
		frame.serverTick = currentTick;
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			auto *pawn = player ? player->GetPlayerPawn() : nullptr;
			auto *body = pawn ? pawn->m_CBodyComponent() : nullptr;
			auto *scene = body ? body->m_pSceneNode() : nullptr;
			if (!player || !pawn || !scene)
			{
				continue;
			}

			const Vector origin = scene->m_vecAbsOrigin();
			Vector eye;
			player->GetEyeOrigin(&eye);
			if (!IsFinite(origin) || !IsFinite(eye))
			{
				continue;
			}
			frame.players[index] = {origin, eye, pawn->GetTeam(), true, pawn->IsAlive(), player->JustTeleported(5.0f)};
		}

		if (!positionFrames.empty() && positionFrames.back().serverTick == currentTick)
		{
			positionFrames.back() = frame;
		}
		else
		{
			positionFrames.push_back(frame);
			while (positionFrames.size() > positionHistoryLimit)
			{
				positionFrames.pop_front();
			}
		}
	}

	void ShotCorrelator::Prune(int currentTick)
	{
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto &data = playerData[index];
			data.commands.erase(std::remove_if(data.commands.begin(), data.commands.end(),
											   [&](const ShotCommand &command)
											   {
												   const std::int64_t delta = static_cast<std::int64_t>(currentTick) - command.serverTick;
												   return command.simulated && delta > shotLifetimeTicks;
											   }),
								data.commands.end());
			data.shots.erase(std::remove_if(data.shots.begin(), data.shots.end(),
											[&](const ShotRecord &shot)
											{
												const std::int64_t delta = static_cast<std::int64_t>(currentTick) - shot.fireTick;
												return delta > shotLifetimeTicks;
											}),
							 data.shots.end());
		}
	}

	ShotRecord *ShotCorrelator::OnWeaponFire(IGameEvent *event, MovementPlayer *player, int currentTick)
	{
		if (!event || !IsEligibleHuman(player))
		{
			return nullptr;
		}
		const std::string_view weapon = NormalizeWeapon(event->GetString("weapon", ""));
		if (!IsBallisticWeapon(weapon))
		{
			return nullptr;
		}

		auto &data = playerData[player->index];
		ShotCommand *match = nullptr;
		int matches = 0;
		for (auto &command : data.commands)
		{
			const std::int64_t delta = static_cast<std::int64_t>(currentTick) - command.serverTick;
			if (!command.simulated || command.consumed || delta < 0 || delta > shotMatchTicks)
			{
				continue;
			}
			match = &command;
			++matches;
		}
		if (matches != 1)
		{
			for (auto &command : data.commands)
			{
				const std::int64_t delta = static_cast<std::int64_t>(currentTick) - command.serverTick;
				if (command.simulated && !command.consumed && delta >= 0 && delta <= shotMatchTicks)
				{
					command.consumed = true;
				}
			}
			return nullptr;
		}

		match->consumed = true;
		ShotRecord shot;
		shot.id = nextShotId++;
		if (nextShotId == 0)
		{
			nextShotId = 1;
		}
		shot.generation = data.generation;
		shot.playerIndex = player->index;
		shot.commandNumber = match->commandNumber;
		shot.clientTick = match->clientTick;
		shot.serverTick = match->serverTick;
		shot.fireTick = currentTick;
		shot.angles = match->attackAngles;
		shot.baseAngles = match->baseAngles;
		shot.eyePosition = match->eyePosition;
		shot.weapon.assign(weapon);
		shot.fireTime = Clock::now();
		shot.airborne = match->airborne;
		shot.scoped = match->scoped;
		data.shots.push_back(std::move(shot));
		while (data.shots.size() > pendingShotLimit)
		{
			data.shots.pop_front();
		}
		return &data.shots.back();
	}

	ShotRecord *ShotCorrelator::OnFireBullets(const CMsgTEFireBullets &event, int currentTick)
	{
		std::string missing;
		const auto require = [&](bool present, const char *name)
		{
			if (!present)
			{
				missing += missing.empty() ? name : tfm::format(", %s", name);
			}
		};
		require(event.has_player(), "player");
		require(event.has_inaccuracy(), "inaccuracy");
		require(event.has_spread(), "spread");
		require(event.has_weapon_id(), "weapon_id");
		if (!missing.empty())
		{
			if (cs2ac_silentaim_debug.GetBool())
			{
				Msg("[CS2AC Silentaim] FireBullets rejected: missing %s.\n", missing.c_str());
			}
			return nullptr;
		}

		const bool validInaccuracy = std::isfinite(event.inaccuracy()) && event.inaccuracy() >= 0.0f;
		const bool validSpread = std::isfinite(event.spread()) && event.spread() >= 0.0f;
		if (!validInaccuracy || !validSpread)
		{
			if (cs2ac_silentaim_debug.GetBool())
			{
				Msg("[CS2AC Silentaim] FireBullets rejected: invalid%s%s.\n", validInaccuracy ? "" : " inaccuracy", validSpread ? "" : " spread");
			}
			return nullptr;
		}

		const CEntityIndex playerEntity(static_cast<int>(event.player() & 0x3FFF));
		auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(playerEntity) : nullptr;
		if (!IsEligibleHuman(player))
		{
			return nullptr;
		}

		auto &data = playerData[player->index];
		ShotRecord *match = nullptr;
		int matches = 0;
		for (auto &shot : data.shots)
		{
			const std::int64_t delta = static_cast<std::int64_t>(currentTick) - shot.fireTick;
			if (shot.generation != data.generation || shot.silentFireSeen || delta < 0 || delta > shotMatchTicks)
			{
				continue;
			}
			match = &shot;
			++matches;
		}
		if (matches != 1)
		{
			if (cs2ac_silentaim_debug.GetBool())
			{
				Msg("[CS2AC Silentaim] FireBullets rejected for entity %d: %d compatible shots.\n", playerEntity.Get(), matches);
			}
			return nullptr;
		}

		match->silentInaccuracy = event.inaccuracy();
		match->silentSpread = event.spread();
		match->silentWeaponId = event.weapon_id();
		match->silentFireSeen = true;
		return match;
	}

	ShotRecord *ShotCorrelator::MatchEvent(MovementPlayer *player, std::string_view weapon, int currentTick)
	{
		if (!IsEligibleHuman(player))
		{
			return nullptr;
		}
		weapon = NormalizeWeapon(weapon);
		auto &data = playerData[player->index];
		ShotRecord *match = nullptr;
		int matches = 0;
		for (auto &shot : data.shots)
		{
			const std::int64_t delta = static_cast<std::int64_t>(currentTick) - shot.fireTick;
			if (shot.generation != data.generation || delta < 0 || delta > shotMatchTicks
				|| (!weapon.empty() && NormalizeWeapon(shot.weapon) != weapon))
			{
				continue;
			}
			match = &shot;
			++matches;
		}
		return matches == 1 ? match : nullptr;
	}

	ShotRecord *ShotCorrelator::OnPlayerHurt(IGameEvent *event, MovementPlayer *victim, int currentTick)
	{
		if (!event || !g_pCS2ACPlayerManager)
		{
			return nullptr;
		}
		auto *attacker = g_pCS2ACPlayerManager->ToPlayer(static_cast<CBasePlayerController *>(event->GetPlayerController("attacker")));
		if (!victim)
		{
			victim = g_pCS2ACPlayerManager->ToPlayer(static_cast<CBasePlayerController *>(event->GetPlayerController("userid")));
		}
		if (!IsEligibleHuman(attacker) || !victim || attacker == victim)
		{
			return nullptr;
		}
		ShotRecord *shot = MatchEvent(attacker, {}, currentTick);
		if (!shot || shot->hurtSeen)
		{
			return nullptr;
		}
		shot->hurtSeen = true;
		shot->silentHitSeen = true;
		shot->victimIndex = victim->index;
		shot->headshot = event->GetInt("hitgroup", HITGROUP_GENERIC) == HITGROUP_HEAD;
		return shot;
	}

	ShotRecord *ShotCorrelator::OnPlayerDeath(IGameEvent *event, MovementPlayer *victim, int currentTick)
	{
		if (!event || !g_pCS2ACPlayerManager)
		{
			return nullptr;
		}
		auto *attacker = g_pCS2ACPlayerManager->ToPlayer(static_cast<CBasePlayerController *>(event->GetPlayerController("attacker")));
		if (!victim)
		{
			victim = g_pCS2ACPlayerManager->ToPlayer(static_cast<CBasePlayerController *>(event->GetPlayerController("userid")));
		}
		if (!IsEligibleHuman(attacker) || !victim || attacker == victim)
		{
			return nullptr;
		}
		ShotRecord *shot = MatchEvent(attacker, event->GetString("weapon", ""), currentTick);
		if (!shot || shot->deathSeen)
		{
			return nullptr;
		}
		shot->deathSeen = true;
		shot->victimIndex = victim->index;
		shot->wallbang = event->GetInt("penetrated", 0) > 0;
		shot->throughSmoke = event->GetBool("thrusmoke", false);
		return shot;
	}

	const PositionFrame *ShotCorrelator::FindFrame(int serverTick) const
	{
		for (auto frame = positionFrames.rbegin(); frame != positionFrames.rend(); ++frame)
		{
			if (frame->serverTick == serverTick)
			{
				return &*frame;
			}
			if (frame->serverTick < serverTick)
			{
				break;
			}
		}
		return nullptr;
	}

	const TrackedPosition *ShotCorrelator::FindPosition(int serverTick, int playerIndex) const
	{
		const PositionFrame *frame = FindFrame(serverTick);
		if (!frame || playerIndex < 1 || playerIndex > MAXPLAYERS || !frame->players[playerIndex].valid)
		{
			return nullptr;
		}
		return &frame->players[playerIndex];
	}

	std::deque<ShotRecord> &ShotCorrelator::GetShots(int playerIndex)
	{
		static std::deque<ShotRecord> empty;
		if (playerIndex < 1 || playerIndex > MAXPLAYERS)
		{
			empty.clear();
			return empty;
		}
		return playerData[playerIndex].shots;
	}

	void DetectionSystem::Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto)
	{
		networkSafety.Reset();
		shots.Reset();
		doubletap.Load(announce, announceNetworkVeto, &networkSafety);
		silentAim.Load(announce, &shots);
		aimbot.Load(announce, &shots);
		aimlock.Load(announce, &shots);
		dllInjection.Load(announce);
		antiAim.Load(announce, announceNetworkVeto, &networkSafety);
		irregularBehavior.Load(announce);
		inhumanAccuracy.Load(announce, &shots);
		nameChanger.Load(announce);
		settingsMask = settings::GetDetectionMask();
		settingsRevision = settings::GetRevision();
		teammatesAreEnemies = TeammatesAreEnemies();
		cachedTeammatesAreEnemies = teammatesAreEnemies;
	}

	void DetectionSystem::Unload()
	{
		doubletap.Unload();
		silentAim.Unload();
		aimbot.Unload();
		aimlock.Unload();
		dllInjection.Unload();
		antiAim.Unload();
		irregularBehavior.Unload();
		inhumanAccuracy.Unload();
		nameChanger.Unload();
		networkSafety.Reset();
		shots.Reset();
		settingsMask = 0;
		settingsRevision = 0;
		teammatesAreEnemies = false;
		cachedTeammatesAreEnemies = false;
	}

	void DetectionSystem::Reset()
	{
		doubletap.Reset();
		silentAim.Reset();
		aimbot.Reset();
		aimlock.Reset();
		dllInjection.Reset();
		antiAim.Reset();
		irregularBehavior.Reset();
		inhumanAccuracy.Reset();
		nameChanger.Reset();
		networkSafety.Reset();
		shots.Reset();
	}

	void DetectionSystem::RefreshSettings()
	{
		const std::uint64_t currentMask = settings::GetDetectionMask();
		const std::uint64_t currentRevision = settings::GetRevision();
		const bool currentTeammatesAreEnemies = TeammatesAreEnemies();
		if (currentMask != settingsMask || currentRevision != settingsRevision || currentTeammatesAreEnemies != teammatesAreEnemies)
		{
			Reset();
			settingsMask = currentMask;
			settingsRevision = currentRevision;
			teammatesAreEnemies = currentTeammatesAreEnemies;
			cachedTeammatesAreEnemies = currentTeammatesAreEnemies;
		}
	}

	void DetectionSystem::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
	{
		RefreshSettings();
		if (!settings::IsPluginEnabled())
		{
			return;
		}
		shots.OnProcessUsercmds(player, commands, numCommands);
		if (settings::IsDetectionEnabled(DetectionType::Doubletap) || settings::IsDetectionEnabled(DetectionType::AntiAim))
		{
			networkSafety.OnProcessUsercmds(player, commands, numCommands);
		}
		if (settings::IsDetectionEnabled(DetectionType::Aimbot))
		{
			aimbot.OnProcessUsercmds(player, commands, numCommands);
		}
		if (settings::IsDetectionEnabled(DetectionType::AntiAim))
		{
			antiAim.OnProcessUsercmds(player, commands, numCommands);
		}
	}

	void DetectionSystem::OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick)
	{
		RefreshSettings();
		if (!settings::IsPluginEnabled())
		{
			return;
		}
		shots.OnSetupMove(player, command, currentTick);
		if (settings::IsDetectionEnabled(DetectionType::Aimbot))
		{
			aimbot.OnSetupMove(player, command, currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::Aimlock))
		{
			aimlock.OnSetupMove(player, command, currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::AntiAim))
		{
			antiAim.OnSetupMove(player, command, currentTick);
		}
	}

	void DetectionSystem::OnGameFrame(int currentTick)
	{
		RefreshSettings();
		if (!settings::IsPluginEnabled())
		{
			return;
		}
		shots.CaptureFrame(currentTick);
		if (settings::IsDetectionEnabled(DetectionType::Doubletap) || settings::IsDetectionEnabled(DetectionType::AntiAim))
		{
			networkSafety.OnGameFrame();
		}
		if (settings::IsDetectionEnabled(DetectionType::Aimbot))
		{
			aimbot.OnGameFrame(currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::Aimlock))
		{
			aimlock.OnGameFrame(currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::AntiAim))
		{
			antiAim.OnGameFrame(currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::SilentAim))
		{
			silentAim.OnGameFrame(currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::IrregularBehavior))
		{
			irregularBehavior.OnGameFrame(currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::InhumanAccuracy))
		{
			inhumanAccuracy.OnGameFrame(currentTick);
		}
		if (settings::IsDetectionEnabled(DetectionType::DllInjection))
		{
			dllInjection.OnGameFrame();
		}
		shots.Prune(currentTick);
	}

	void DetectionSystem::OnGameEvent(IGameEvent *event, MovementPlayer *player, int currentTick)
	{
		RefreshSettings();
		if (!event || !settings::IsPluginEnabled())
		{
			return;
		}

		if (settings::IsDetectionEnabled(DetectionType::AntiAim))
		{
			antiAim.OnGameEvent(event, player);
		}
		if (CS2AC_STREQ(event->GetName(), "weapon_fire"))
		{
			if (settings::IsDetectionEnabled(DetectionType::Doubletap))
			{
				doubletap.OnWeaponFire(event, player, currentTick);
			}
			if (ShotRecord *shot = shots.OnWeaponFire(event, player, currentTick))
			{
				if (settings::IsDetectionEnabled(DetectionType::AntiAim))
				{
					antiAim.OnWeaponFire(player, *shot);
				}
				if (settings::IsDetectionEnabled(DetectionType::IrregularBehavior))
				{
					irregularBehavior.OnWeaponFire(player, *shot);
				}
			}
		}
		else if (CS2AC_STREQ(event->GetName(), "player_hurt"))
		{
			if (ShotRecord *shot = shots.OnPlayerHurt(event, player, currentTick))
			{
				auto *attacker = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(shot->playerIndex));
				auto *victim = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(shot->victimIndex));
				if (settings::IsDetectionEnabled(DetectionType::Aimbot))
				{
					aimbot.OnPlayerHurt(attacker, victim, *shot);
				}
			}
		}
		else if (CS2AC_STREQ(event->GetName(), "player_death"))
		{
			if (ShotRecord *shot = shots.OnPlayerDeath(event, player, currentTick))
			{
				auto *attacker = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(shot->playerIndex));
				auto *victim = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(shot->victimIndex));
				if (settings::IsDetectionEnabled(DetectionType::IrregularBehavior))
				{
					irregularBehavior.OnPlayerDeath(event, attacker, victim, *shot);
				}
			}
		}
	}

	void DetectionSystem::OnFireBullets(const CMsgTEFireBullets &event, int currentTick)
	{
		RefreshSettings();
		if (!settings::IsPluginEnabled() || !settings::IsDetectionEnabled(DetectionType::SilentAim))
		{
			return;
		}
		if (ShotRecord *shot = shots.OnFireBullets(event, currentTick))
		{
			auto *player = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(shot->playerIndex));
			silentAim.OnShotUpdated(player, *shot);
		}
	}

	void DetectionSystem::OnClientReady(MovementPlayer *player)
	{
		RefreshSettings();
		if (settings::IsDetectionEnabled(DetectionType::NameChanger))
		{
			nameChanger.OnClientReady(player);
		}
	}

	void DetectionSystem::OnClientSettingsChanged(MovementPlayer *player)
	{
		RefreshSettings();
		if (settings::IsDetectionEnabled(DetectionType::NameChanger))
		{
			nameChanger.OnClientSettingsChanged(player);
		}
	}

	void DetectionSystem::OnClientDisconnect(MovementPlayer *player)
	{
		doubletap.OnClientDisconnect(player);
		silentAim.OnClientDisconnect(player);
		aimbot.OnClientDisconnect(player);
		aimlock.OnClientDisconnect(player);
		dllInjection.OnClientDisconnect(player);
		antiAim.OnClientDisconnect(player);
		irregularBehavior.OnClientDisconnect(player);
		inhumanAccuracy.OnClientDisconnect(player);
		nameChanger.OnClientDisconnect(player);
		networkSafety.OnClientDisconnect(player);
		shots.OnClientDisconnect(player);
	}

} // namespace detection
