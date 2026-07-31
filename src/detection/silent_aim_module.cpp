#include "detection/detection_system.h"

#include "movement_analysis/player_context.h"
#include "movement/movement.h"

#include <algorithm>
#include <cmath>

CConVar<bool> cs2ac_silentaim_debug("cs2ac_silentaim_debug", FCVAR_NONE, "Show why Silentaim accepts or rejects each matched shot", false);

#define SILENTAIM_DEBUG(...) \
	do \
	{ \
		if (cs2ac_silentaim_debug.GetBool()) \
			Msg("[CS2AC Silentaim] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr int detectionScore = 10;
	constexpr int normalHitDecay = 2;
	constexpr auto evidenceWindow = std::chrono::minutes(5);
	constexpr float minimumAllowance = 1.0f;
	constexpr float blatantExcess = 22.5f;
} // namespace

namespace detection
{
	void SilentAimModule::Load(AnnounceCallback announceCallback, ShotCorrelator *shotCorrelator)
	{
		announce = announceCallback;
		shots = shotCorrelator;
	}

	void SilentAimModule::Unload()
	{
		Reset();
		shots = nullptr;
		announce = nullptr;
	}

	void SilentAimModule::Reset()
	{
		evidence = {};
	}

	void SilentAimModule::OnShotUpdated(MovementPlayer *player, ShotRecord &shot)
	{
		if (!IsEligibleHuman(player) || shot.playerIndex != player->index || shot.silentMeasured || shot.silentConsumed || !shot.silentFireSeen
			|| !IsFinite(shot.baseAngles) || !IsFinite(shot.angles) || !std::isfinite(shot.silentInaccuracy) || !std::isfinite(shot.silentSpread)
			|| shot.silentInaccuracy < 0.0f || shot.silentSpread < 0.0f)
		{
			return;
		}
		shot.silentMeasured = true;

		if (NormalizeWeapon(shot.weapon) == "taser")
		{
			shot.silentRejected = true;
			SILENTAIM_DEBUG("%s shot rejected: tasers are not evaluated.\n", player->GetName());
			return;
		}

		constexpr float radiansToDegrees = static_cast<float>(180.0 / M_PI);
		shot.silentAllowance =
			(std::max)(minimumAllowance, static_cast<float>(std::atan(shot.silentInaccuracy + shot.silentSpread) * radiansToDegrees));
		shot.silentDeviation = AngularDistance(shot.baseAngles, shot.angles);
		if (!std::isfinite(shot.silentAllowance) || !std::isfinite(shot.silentDeviation))
		{
			shot.silentRejected = true;
			return;
		}

		SILENTAIM_DEBUG("%s matched command %d to FireBullets weapon %u: deviation %.2f, allowance %.2f, inaccuracy %.5f, spread %.5f.\n",
						player->GetName(), shot.commandNumber, shot.silentWeaponId, shot.silentDeviation, shot.silentAllowance, shot.silentInaccuracy,
						shot.silentSpread);
	}

	void SilentAimModule::OnGameFrame(int currentTick)
	{
		if (!shots)
		{
			return;
		}
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			for (auto &shot : shots->GetShots(index))
			{
				const std::int64_t age = static_cast<std::int64_t>(currentTick) - shot.fireTick;
				if (!shot.silentConsumed && age >= 2)
				{
					Finalize(player, shot);
				}
			}
		}
	}

	void SilentAimModule::Finalize(MovementPlayer *player, ShotRecord &shot)
	{
		shot.silentConsumed = true;
		if (!IsEligibleHuman(player) || !shot.silentMeasured || !shot.silentHitSeen || shot.silentRejected)
		{
			return;
		}

		const auto now = Clock::now();
		auto &incidents = evidence[player->index];
		while (!incidents.empty() && now - incidents.front().time >= evidenceWindow)
		{
			incidents.pop_front();
		}

		if (shot.silentDeviation <= shot.silentAllowance)
		{
			int remainingDecay = normalHitDecay;
			while (remainingDecay > 0 && !incidents.empty())
			{
				const int applied = (std::min)(remainingDecay, incidents.back().points);
				incidents.back().points -= applied;
				remainingDecay -= applied;
				if (incidents.back().points == 0)
				{
					incidents.pop_back();
				}
			}
			int total = 0;
			for (const auto &incident : incidents)
			{
				total += incident.points;
			}
			SILENTAIM_DEBUG("%s confirmed hit was normal: %.2f <= %.2f degrees; score decayed by %d to %d/%d.\n", player->GetName(),
							shot.silentDeviation, shot.silentAllowance, normalHitDecay - remainingDecay, total, detectionScore);
			return;
		}

		const float excess = shot.silentDeviation - shot.silentAllowance;
		const std::string_view weapon = NormalizeWeapon(shot.weapon);
		const bool noscope = !shot.scoped && (weapon == "awp" || weapon == "ssg08" || weapon == "g3sg1" || weapon == "scar20");
		const int points = (excess > blatantExcess ? 3
							: shot.airborne        ? 3
												   : 2)
						   + static_cast<int>(shot.headshot) + 2 * static_cast<int>(shot.wallbang) + 2 * static_cast<int>(shot.throughSmoke)
						   + static_cast<int>(noscope);
		incidents.push_back({now, points});

		int total = 0;
		for (const auto &incident : incidents)
		{
			total += incident.points;
		}
		SILENTAIM_DEBUG("%s added %d point%s for %.2f degrees; score %d/%d.\n", player->GetName(), points, points == 1 ? "" : "s",
						shot.silentDeviation, total, detectionScore);
		if (total >= detectionScore)
		{
			if (announce)
			{
				announce(
					"SILENTAIM", player,
					localization::Format("evidence.silentaim",
										 "{deviation} degrees from visible aim added {points} points; the rolling score reached {score}/{threshold}.",
										 {{"deviation", tfm::format("%.2f", shot.silentDeviation)},
										  {"points", tfm::format("%d", points)},
										  {"score", tfm::format("%d", total)},
										  {"threshold", tfm::format("%d", detectionScore)}}));
			}
			incidents.clear();
		}
	}

	void SilentAimModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			evidence[player->index].clear();
		}
	}
} // namespace detection
