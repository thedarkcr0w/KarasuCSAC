#include "detection/detection_system.h"

#include "igameevents.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"

#include <algorithm>
#include <cmath>

CConVar<bool> cs2ac_irregular_debug("cs2ac_irregular_debug", FCVAR_NONE, "Show Irregular Behavior attempts, evidence, and rejections", false);

#define IRREGULAR_DEBUG(...) \
	do \
	{ \
		if (cs2ac_irregular_debug.GetBool()) \
			Msg("[CS2AC Irregular Behavior] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr int evidenceWindowSeconds = 5 * 60;
	constexpr float minimumDistance = 10.0f;
	constexpr float longDistance = 20.0f;
	constexpr int detectionScore = 16;
	constexpr int minimumSuccesses = 3;
	constexpr int minimumAttempts = 4;
	constexpr size_t pendingLimit = 8;

	bool IsSniperWeapon(std::string_view weapon)
	{
		weapon = detection::NormalizeWeapon(weapon);
		return weapon == "awp" || weapon == "ssg08" || weapon == "scar20" || weapon == "g3sg1";
	}

	std::int64_t ToSecond(detection::Clock::time_point time)
	{
		return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
	}
} // namespace

namespace detection
{
	void IrregularBehaviorModule::Load(AnnounceCallback announceCallback)
	{
		announce = announceCallback;
	}

	void IrregularBehaviorModule::Unload()
	{
		Reset();
		announce = nullptr;
	}

	void IrregularBehaviorModule::Reset()
	{
		playerData = {};
	}

	void IrregularBehaviorModule::OnWeaponFire(MovementPlayer *player, const ShotRecord &shot)
	{
		if (!IsEligibleHuman(player) || shot.playerIndex != player->index || !IsBallisticWeapon(shot.weapon))
		{
			return;
		}
		const bool sniper = IsSniperWeapon(shot.weapon);
		const bool noScope = sniper && !shot.scoped;
		if (!shot.airborne && !noScope)
		{
			return;
		}

		auto &data = playerData[player->index];
		while (data.pending.size() >= pendingLimit)
		{
			const IrregularPending attempt = data.pending.front();
			data.pending.pop_front();
			Finalize(player, data, attempt);
		}
		data.pending.push_back({shot.id, shot.fireTime, shot.fireTick, 0, shot.airborne, noScope});
		IRREGULAR_DEBUG("%s recorded difficult shot %llu (%s%s).\n", player->GetName(), static_cast<unsigned long long>(shot.id),
						shot.airborne ? "airborne" : "",
						shot.airborne && noScope ? " no-scope"
						: noScope                ? "no-scope"
												 : "");
	}

	void IrregularBehaviorModule::OnPlayerDeath(IGameEvent *event, MovementPlayer *attacker, MovementPlayer *victim, ShotRecord &shot)
	{
		if (!event || !IsEligibleHuman(attacker) || !victim || attacker == victim || shot.playerIndex != attacker->index || shot.irregularConsumed)
		{
			return;
		}
		auto *attackerPawn = attacker->GetPlayerPawn();
		auto *victimPawn = victim->GetPlayerPawn();
		if (!attackerPawn || !victimPawn || !AreOpponents(attackerPawn->GetTeam(), victimPawn->GetTeam()))
		{
			return;
		}
		shot.irregularConsumed = true;
		auto &data = playerData[attacker->index];
		auto attempt =
			std::find_if(data.pending.begin(), data.pending.end(), [&](const IrregularPending &pending) { return pending.shotId == shot.id; });
		if (attempt == data.pending.end())
		{
			return;
		}

		const bool sniper = IsSniperWeapon(shot.weapon);
		const bool airborne = shot.airborne || event->GetBool("attackerinair", false);
		const bool noScope = sniper && (!shot.scoped || event->GetBool("noscope", false));
		attempt->airborne = airborne;
		attempt->noScope = noScope;

		const float distance = event->GetFloat("distance", 0.0f);
		if (!std::isfinite(distance) || distance < minimumDistance)
		{
			IRREGULAR_DEBUG("%s difficult kill rejected at %.1f metres.\n", attacker->GetName(), distance);
			return;
		}

		attempt->success = true;
		attempt->points = static_cast<int>(airborne) + static_cast<int>(noScope) + 2 * static_cast<int>(airborne && noScope)
						  + static_cast<int>(distance >= longDistance) + static_cast<int>(event->GetInt("penetrated", 0) > 0) + 2
						  + static_cast<int>(event->GetBool("headshot", false));
		IRREGULAR_DEBUG("%s matched difficult kill %llu worth %d points.\n", attacker->GetName(), static_cast<unsigned long long>(shot.id),
						attempt->points);
	}

	void IrregularBehaviorModule::OnGameFrame(int currentTick)
	{
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto &data = playerData[index];
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			if (!IsEligibleHuman(player))
			{
				data = {};
				continue;
			}
			while (!data.pending.empty() && static_cast<std::int64_t>(currentTick) - data.pending.front().fireTick >= 2)
			{
				const IrregularPending attempt = data.pending.front();
				data.pending.pop_front();
				Finalize(player, data, attempt);
			}
			Prune(data, ToSecond(Clock::now()));
		}
	}

	void IrregularBehaviorModule::Finalize(MovementPlayer *player, IrregularPlayerData &data, const IrregularPending &attempt)
	{
		const std::int64_t second = ToSecond(attempt.time);
		Prune(data, ToSecond(Clock::now()));
		if (data.evidence.empty() || data.evidence.back().second != second)
		{
			data.evidence.push_back({second});
		}
		auto &bucket = data.evidence.back();
		++bucket.attempts;
		bucket.successes += attempt.success;
		bucket.score += attempt.points;
		Evaluate(player, data);
	}

	void IrregularBehaviorModule::Prune(IrregularPlayerData &data, std::int64_t nowSecond)
	{
		while (!data.evidence.empty() && nowSecond - data.evidence.front().second >= evidenceWindowSeconds)
		{
			data.evidence.pop_front();
		}
	}

	void IrregularBehaviorModule::Evaluate(MovementPlayer *player, IrregularPlayerData &data)
	{
		int attempts = 0;
		int successes = 0;
		int score = 0;
		for (const auto &bucket : data.evidence)
		{
			attempts += bucket.attempts;
			successes += bucket.successes;
			score += bucket.score;
		}
		IRREGULAR_DEBUG("%s evidence %d/%d points from %d/%d difficult shots.\n", player->GetName(), score, detectionScore, successes, attempts);
		if (score < detectionScore || successes < minimumSuccesses || attempts < minimumAttempts || successes * 2 < attempts)
		{
			return;
		}
		if (announce)
		{
			announce(
				"IRREGULAR BEHAVIOR", player,
				localization::Format("evidence.irregular_behavior",
									 "{score} points from {successes} successful difficult shots out of {attempts} attempts ({accuracy}% success).",
									 {{"score", tfm::format("%d", score)},
									  {"successes", tfm::format("%d", successes)},
									  {"attempts", tfm::format("%d", attempts)},
									  {"accuracy", tfm::format("%.1f", attempts ? successes * 100.0 / attempts : 0.0)}}));
		}
		data.evidence.clear();
		data.pending.clear();
	}

	void IrregularBehaviorModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}
} // namespace detection
