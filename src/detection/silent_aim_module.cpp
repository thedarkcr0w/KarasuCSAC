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
		if (!shot.silentSubtickAnglesValid)
		{
			shot.silentRejected = true;
			SILENTAIM_DEBUG("%s shot rejected: its subtick view movement was invalid.\n", player->GetName());
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

		const float pitchToBase = shot.baseAngles.x - shot.angles.x;
		const float yawToBase = std::remainder(shot.baseAngles.y - shot.angles.y, 360.0f);
		QAngle movementAdjusted = shot.angles;
		movementAdjusted.x += std::copysign((std::min)(std::abs(pitchToBase), shot.silentSubtickPitchTravel), pitchToBase);
		movementAdjusted.y += std::copysign((std::min)(std::abs(yawToBase), shot.silentSubtickYawTravel), yawToBase);
		shot.silentUnsupportedDeviation = AngularDistance(shot.baseAngles, movementAdjusted);
		const bool hasSubtickMovement = shot.silentSubtickPitchTravel > 0.0f || shot.silentSubtickYawTravel > 0.0f;
		shot.silentMovementSupported =
			hasSubtickMovement && std::isfinite(shot.silentUnsupportedDeviation) && shot.silentUnsupportedDeviation <= shot.silentAllowance;

		if (shot.silentPreviousBaseValid && IsFinite(shot.silentPreviousBaseAngles))
		{
			// A legitimate firing angle lies on the reported path between adjacent base angles, so the two path legs
			// should not be meaningfully longer than the direct angle between those commands.
			const float previousToAttack = AngularDistance(shot.silentPreviousBaseAngles, shot.angles);
			const float previousToCurrent = AngularDistance(shot.silentPreviousBaseAngles, shot.baseAngles);
			shot.silentAdjacentPathExcess = (std::max)(0.0f, previousToAttack + shot.silentDeviation - previousToCurrent);
			shot.silentMovementSupported =
				shot.silentMovementSupported || (std::isfinite(shot.silentAdjacentPathExcess) && shot.silentAdjacentPathExcess <= minimumAllowance);
		}

		SILENTAIM_DEBUG("%s matched command %d to FireBullets weapon %u: deviation %.2f, allowance %.2f, unexplained %.2f, adjacent path "
						"excess %.2f, subtick travel %.2f pitch/%.2f yaw, inaccuracy %.5f, spread %.5f.\n",
						player->GetName(), shot.commandNumber, shot.silentWeaponId, shot.silentDeviation, shot.silentAllowance,
						shot.silentUnsupportedDeviation, shot.silentPreviousBaseValid ? shot.silentAdjacentPathExcess : -1.0f,
						shot.silentSubtickPitchTravel, shot.silentSubtickYawTravel, shot.silentInaccuracy, shot.silentSpread);
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

		if (shot.silentDeviation <= shot.silentAllowance || shot.silentMovementSupported)
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
			if (shot.silentMovementSupported && shot.silentDeviation > shot.silentAllowance)
			{
				SILENTAIM_DEBUG(
					"%s confirmed hit was ignored: reported view movement explained the %.2f-degree mismatch; score decayed by %d to %d/%d.\n",
					player->GetName(), shot.silentDeviation, normalHitDecay - remainingDecay, total, detectionScore);
			}
			else
			{
				SILENTAIM_DEBUG("%s confirmed hit was normal: %.2f <= %.2f degrees; score decayed by %d to %d/%d.\n", player->GetName(),
								shot.silentDeviation, shot.silentAllowance, normalHitDecay - remainingDecay, total, detectionScore);
			}
			return;
		}

		const float excess = shot.silentDeviation - shot.silentAllowance;
		const std::string_view weapon = NormalizeWeapon(shot.weapon);
		const bool noscope = !shot.scoped && (weapon == "awp" || weapon == "ssg08" || weapon == "g3sg1" || weapon == "scar20");
		const bool blatant = excess > blatantExcess;
		const int points = (blatant         ? 4
							: shot.airborne ? 3
											: 2)
						   + static_cast<int>(shot.headshot) + 2 * static_cast<int>(shot.wallbang) + 2 * static_cast<int>(shot.throughSmoke)
						   + static_cast<int>(noscope);
		const SilentEvidenceContext context = blatant         ? SilentEvidenceContext::Blatant
											  : shot.airborne ? SilentEvidenceContext::Airborne
															  : SilentEvidenceContext::Grounded;
		incidents.push_back({now, points, points, excess, context});

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
				localization::Text weightDetails;
				auto addWeightSentence = [&](const char *key, const char *english)
				{
					const auto sentence = localization::Format(key, english);
					const char *separator = weightDetails.english.empty() ? "" : " ";
					weightDetails = {weightDetails.english + separator + sentence.english, weightDetails.localized + separator + sentence.localized};
				};
				if (blatant)
				{
					addWeightSentence("evidence.silentaim.weight.blatant",
									  "The unexplained angle was over 22.5 degrees, so the shot started at 4 points.");
				}
				else if (shot.airborne)
				{
					addWeightSentence("evidence.silentaim.weight.airborne", "The player was airborne, so the shot started at 3 points.");
				}
				else
				{
					addWeightSentence("evidence.silentaim.weight.grounded", "The player was on the ground, so the shot started at 2 points.");
				}
				if (shot.headshot)
				{
					addWeightSentence("evidence.silentaim.weight.headshot", "A headshot added 1 point.");
				}
				if (shot.wallbang)
				{
					addWeightSentence("evidence.silentaim.weight.wallbang", "A wallbang added 2 points.");
				}
				if (shot.throughSmoke)
				{
					addWeightSentence("evidence.silentaim.weight.smoke", "A shot through smoke added 2 points.");
				}
				if (noscope)
				{
					addWeightSentence("evidence.silentaim.weight.noscope", "A no-scope added 1 point.");
				}
				auto formatEvidence = [&](const std::string &weight)
				{
					const localization::Arguments values {{"deviation", tfm::format("%.2f", shot.silentDeviation)},
														  {"allowance", tfm::format("%.2f", shot.silentAllowance)},
														  {"excess", tfm::format("%.2f", excess)},
														  {"weight", weight},
														  {"score", tfm::format("%d", total)},
														  {"threshold", tfm::format("%d", detectionScore)}};
					return localization::Format(
						"evidence.silentaim.single",
						"This damaging shot landed {deviation} degrees from the visible aim. Weapon spread allowed {allowance}, "
						"leaving {excess} degrees unexplained. {weight} Score: {score}/{threshold}.",
						values);
				};
				std::vector<localization::Text> history;
				for (size_t index = 0; index + 1 < incidents.size(); ++index)
				{
					const auto &incident = incidents[index];
					const auto contextText = incident.context == SilentEvidenceContext::Blatant
												 ? localization::Format("evidence.silentaim.context.blatant", "over 22.5 degrees")
											 : incident.context == SilentEvidenceContext::Airborne
												 ? localization::Format("evidence.silentaim.context.airborne", "airborne")
												 : localization::Format("evidence.silentaim.context.grounded", "grounded");
					const auto pointsText = incident.points == incident.originalPoints
												? localization::Text {tfm::format("+%d", incident.points), tfm::format("+%d", incident.points)}
												: localization::Format("evidence.history.decayed_points", "+{remaining} remaining from +{original}",
																	   {{"remaining", tfm::format("%d", incident.points)},
																		{"original", tfm::format("%d", incident.originalPoints)}});
					auto formatLine = [&](const std::string &contextValue, const std::string &pointsValue)
					{
						return localization::Format(
							"evidence.silentaim", "{degrees}° unexplained - {context} - {points}",
							{{"degrees", tfm::format("%.2f", incident.unexplainedDegrees)}, {"context", contextValue}, {"points", pointsValue}});
					};
					const auto englishLine = formatLine(contextText.english, pointsText.english);
					const auto localizedLine = formatLine(contextText.localized, pointsText.localized);
					history.push_back({englishLine.english, localizedLine.localized});
				}
				const auto englishEvidence = formatEvidence(weightDetails.english);
				const auto localizedEvidence = formatEvidence(weightDetails.localized);
				announce("SILENTAIM", player, FormatEvidenceHistory(history, {englishEvidence.english, localizedEvidence.localized}));
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
