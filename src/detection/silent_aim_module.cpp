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
	constexpr int detectionScore = 12;
	constexpr auto evidenceWindow = std::chrono::minutes(10);
	constexpr float minimumImpactDistance = 100.0f;
	constexpr float maximumImpactDistance = 10000.0f;
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
		if (!IsEligibleHuman(player) || shot.playerIndex != player->index || shot.silentMeasured || shot.silentConsumed || !shot.hasVisibleAngles
			|| !shot.impactSeen || !IsFinite(shot.eyePosition) || !IsFinite(shot.impactPosition) || !IsFinite(shot.visibleAngles))
		{
			return;
		}

		Vector direction = shot.impactPosition - shot.eyePosition;
		const float distance = direction.Length();
		if (!std::isfinite(distance) || distance < minimumImpactDistance || distance > maximumImpactDistance)
		{
			SILENTAIM_DEBUG("%s impact rejected at %.1f units.\n", player->GetName(), distance);
			return;
		}
		direction.NormalizeInPlace();
		const float dot = std::clamp(DotProduct(AimForward(shot.visibleAngles), direction), -1.0f, 1.0f);
		const float deviation = static_cast<float>(std::acos(dot) * (180.0 / M_PI));
		if (!std::isfinite(deviation))
		{
			return;
		}
		shot.silentMeasured = true;
		shot.silentMaxDeviation = (std::max)(shot.silentMaxDeviation, deviation);
		SILENTAIM_DEBUG("%s matched impact %.2f degrees from visible aim for command %d.\n", player->GetName(), deviation, shot.commandNumber);
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
		if (!IsEligibleHuman(player) || !shot.hurtSeen || !shot.impactSeen)
		{
			return;
		}

		const float threshold = GetHighDeviationThreshold(shot.weapon);
		if (!std::isfinite(shot.silentMaxDeviation) || shot.silentMaxDeviation <= threshold)
		{
			SILENTAIM_DEBUG("%s confirmed hit was normal: %.2f <= %.2f degrees.\n", player->GetName(), shot.silentMaxDeviation, threshold);
			return;
		}

		const int points = (shot.silentMaxDeviation > 22.5f ? 3
							: shot.airborne                 ? 1
															: 2)
						   + static_cast<int>(shot.headshot) + static_cast<int>(shot.wallbang) + static_cast<int>(shot.throughSmoke);
		const auto now = Clock::now();
		auto &incidents = evidence[player->index];
		while (!incidents.empty() && now - incidents.front().time >= evidenceWindow)
		{
			incidents.pop_front();
		}
		incidents.push_back({now, points});

		int total = 0;
		for (const auto &incident : incidents)
		{
			total += incident.points;
		}
		SILENTAIM_DEBUG("%s added %d point%s for %.2f degrees; score %d/%d.\n", player->GetName(), points, points == 1 ? "" : "s",
						shot.silentMaxDeviation, total, detectionScore);
		if (total >= detectionScore)
		{
			if (announce)
			{
				announce(
					"SILENTAIM", player,
					localization::Format("evidence.silentaim",
										 "{deviation} degrees from visible aim added {points} points; the rolling score reached {score}/{threshold}.",
										 {{"deviation", tfm::format("%.2f", shot.silentMaxDeviation)},
										  {"points", tfm::format("%d", points)},
										  {"score", tfm::format("%d", total)},
										  {"threshold", tfm::format("%d", detectionScore)}}));
			}
			incidents.clear();
		}
	}

	float SilentAimModule::GetHighDeviationThreshold(std::string_view weapon)
	{
		weapon = NormalizeWeapon(weapon);
		if (weapon == "ak47" || weapon == "m4a1" || weapon == "m4a1_silencer" || weapon == "galilar" || weapon == "famas" || weapon == "aug"
			|| weapon == "sg556" || weapon == "g3sg1" || weapon == "scar20")
		{
			return 12.5f;
		}
		if (weapon == "awp" || weapon == "ssg08")
		{
			return 2.5f;
		}
		if (weapon == "deagle" || weapon == "revolver")
		{
			return 4.5f;
		}
		if (weapon == "glock" || weapon == "hkp2000" || weapon == "usp_silencer" || weapon == "elite" || weapon == "p250" || weapon == "tec9"
			|| weapon == "fiveseven" || weapon == "cz75a")
		{
			return 4.3f;
		}
		if (weapon == "mac10" || weapon == "mp9" || weapon == "mp7" || weapon == "mp5sd" || weapon == "ump45" || weapon == "p90" || weapon == "bizon")
		{
			return 22.5f;
		}
		if (weapon == "nova" || weapon == "xm1014" || weapon == "sawedoff" || weapon == "mag7" || weapon == "m249" || weapon == "negev")
		{
			return 13.5f;
		}
		return 15.5f;
	}

	void SilentAimModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			evidence[player->index].clear();
		}
	}
} // namespace detection
