#include "detection/detection_system.h"

#include "movement_analysis/player_context.h"
#include "movement/movement.h"

#include <algorithm>
#include <cmath>

CConVar<bool> cs2ac_inhuman_accuracy_debug("cs2ac_inhuman_accuracy_debug", FCVAR_NONE, "Show Inhuman Accuracy attempts, evidence, and rejections",
										   false);

#define ACCURACY_DEBUG(...) \
	do \
	{ \
		if (cs2ac_inhuman_accuracy_debug.GetBool()) \
			Msg("[CS2AC Inhuman Accuracy] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr int evidenceWindowSeconds = 5 * 60;
	constexpr int minimumAttempts = 15;
	constexpr int requiredAccuracyPercent = 85;
	constexpr float minimumDistance = 100.0f;
	constexpr float attemptHalfWidth = 32.0f;
	static_assert(requiredAccuracyPercent <= 100);

	bool IsAccuracyWeapon(std::string_view weapon)
	{
		weapon = detection::NormalizeWeapon(weapon);
		static constexpr std::string_view weapons[] = {
			"ak47",    "aug",      "awp",     "bizon", "cz75a",         "deagle", "elite", "famas",        "fiveseven", "g3sg1",
			"galilar", "glock",    "hkp2000", "m4a1",  "m4a1_silencer", "mac10",  "mp5sd", "mp7",          "mp9",       "p250",
			"p90",     "revolver", "scar20",  "sg556", "ssg08",         "tec9",   "ump45", "usp_silencer",
		};
		return std::find(std::begin(weapons), std::end(weapons), weapon) != std::end(weapons);
	}

	float AimError(const Vector &eye, const QAngle &angles, const Vector &target)
	{
		Vector direction = target - eye;
		if (!detection::IsFinite(direction) || direction.LengthSqr() < EPSILON)
		{
			return 180.0f;
		}
		direction.NormalizeInPlace();
		const float dot = std::clamp(DotProduct(detection::AimForward(angles), direction), -1.0f, 1.0f);
		return static_cast<float>(std::acos(dot) * (180.0 / M_PI));
	}

	float NearestBodyAimError(const Vector &eye, const QAngle &angles, const Vector &feet)
	{
		static constexpr float bodyHeights[] = {8.0f, 46.0f, 64.0f};
		float best = 180.0f;
		for (float height : bodyHeights)
		{
			best = (std::min)(best, AimError(eye, angles, feet + Vector(0.0f, 0.0f, height)));
		}
		return best;
	}

	std::int64_t ToSecond(detection::Clock::time_point time)
	{
		return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
	}
} // namespace

namespace detection
{
	void InhumanAccuracyModule::Load(AnnounceCallback announceCallback, ShotCorrelator *shotCorrelator)
	{
		announce = announceCallback;
		shots = shotCorrelator;
	}

	void InhumanAccuracyModule::Unload()
	{
		Reset();
		shots = nullptr;
		announce = nullptr;
	}

	void InhumanAccuracyModule::Reset()
	{
		playerData = {};
	}

	void InhumanAccuracyModule::OnGameFrame(int currentTick)
	{
		if (!shots)
		{
			return;
		}
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			if (!IsEligibleHuman(player))
			{
				playerData[index] = {};
				continue;
			}

			for (auto &shot : shots->GetShots(index))
			{
				const std::int64_t age = static_cast<std::int64_t>(currentTick) - shot.fireTick;
				if (!shot.accuracyConsumed && age >= 2)
				{
					shot.accuracyConsumed = true;
					Finalize(player, shot);
				}
			}
			Prune(playerData[index], ToSecond(Clock::now()));
		}
	}

	void InhumanAccuracyModule::Finalize(MovementPlayer *player, ShotRecord &shot)
	{
		if (!IsAccuracyWeapon(shot.weapon) || !IsFinite(shot.angles) || !IsFinite(shot.eyePosition) || !shots)
		{
			return;
		}
		const PositionFrame *frame = shots->FindFrame(shot.serverTick);
		if (!frame)
		{
			ACCURACY_DEBUG("%s shot %llu rejected because its historical frame is missing.\n", player->GetName(),
						   static_cast<unsigned long long>(shot.id));
			return;
		}
		const auto &attacker = frame->players[player->index];
		if (!attacker.valid || attacker.teleported)
		{
			return;
		}

		int matchedTarget = -1;
		float matchedError = 180.0f;
		for (int targetIndex = 1; targetIndex <= MAXPLAYERS; ++targetIndex)
		{
			const auto &target = frame->players[targetIndex];
			if ((shot.hurtSeen && targetIndex != shot.victimIndex) || targetIndex == player->index || !target.valid || target.teleported
				|| !AreOpponents(attacker.team, target.team) || (!shot.hurtSeen && !target.alive))
			{
				continue;
			}
			const float distance = (target.origin - shot.eyePosition).Length();
			const float error = NearestBodyAimError(shot.eyePosition, shot.angles, target.origin);
			if (!std::isfinite(distance) || distance < minimumDistance || !std::isfinite(error))
			{
				continue;
			}
			const float aimTolerance = static_cast<float>(std::atan2(attemptHalfWidth, distance) * (180.0 / M_PI));
			if (error > aimTolerance)
			{
				continue;
			}
			if (error < matchedError)
			{
				matchedTarget = targetIndex;
				matchedError = error;
			}
		}
		if (matchedTarget == -1)
		{
			return;
		}

		auto &data = playerData[player->index];
		const std::int64_t second = ToSecond(shot.fireTime);
		Prune(data, ToSecond(Clock::now()));
		if (data.evidence.empty() || data.evidence.back().second != second)
		{
			data.evidence.push_back({second});
		}
		++data.evidence.back().attempts;
		data.evidence.back().hits += shot.hurtSeen;

		int attempts = 0;
		int hits = 0;
		for (const auto &bucket : data.evidence)
		{
			attempts += bucket.attempts;
			hits += bucket.hits;
		}
		ACCURACY_DEBUG("%s shot %llu counted at %.3f degrees: %d/%d hits.\n", player->GetName(), static_cast<unsigned long long>(shot.id),
					   matchedError, hits, attempts);
		if (attempts < minimumAttempts || hits * 100 < attempts * requiredAccuracyPercent)
		{
			return;
		}
		if (announce)
		{
			announce("INHUMAN ACCURACY", player,
					 localization::Format("evidence.inhuman_accuracy", "{hits} of {attempts} qualifying shots hit ({accuracy}% accuracy).",
										  {{"hits", tfm::format("%d", hits)},
										   {"attempts", tfm::format("%d", attempts)},
										   {"accuracy", tfm::format("%.1f", attempts ? hits * 100.0 / attempts : 0.0)}}));
		}
		data.evidence.clear();
	}

	void InhumanAccuracyModule::Prune(AccuracyPlayerData &data, std::int64_t nowSecond)
	{
		while (!data.evidence.empty() && nowSecond - data.evidence.front().second >= evidenceWindowSeconds)
		{
			data.evidence.pop_front();
		}
	}

	void InhumanAccuracyModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}
} // namespace detection
