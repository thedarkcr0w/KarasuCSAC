/*
	A bhop attempt is added when the player lands and:
	1. The player spent at least 4 ticks in the air before landing. This is to avoid cases where weird collisions cause false positives.
	2. The player did not teleport or noclip in the last 4 ticks. This is to avoid false positives from telehops or leaving noclips.
	3. sv_autobunnyhopping is off.
*/

#include "movement_analysis/detection/movement_detection.h"
#include "movement_analysis/settings/movement_settings.h"
#include "sdk/usercmd.h"
#include "settings.h"

CConVar<bool> cs2ac_bhop_debug("cs2ac_bhop_debug", FCVAR_NONE, "Show Bhop landing gates, perfect chains, and input patterns", false);
CConVar<bool> cs2ac_hyperscroll_debug("cs2ac_hyperscroll_debug", FCVAR_NONE,
									  "Show Hyperscroll landing samples, jump-input patterns, and perfect ratios", false);

#define BHOP_DEBUG(...) \
	do \
	{ \
		if (cs2ac_bhop_debug.GetBool()) \
			Msg("[CS2AC Bhop] " __VA_ARGS__); \
	} while (0)

#define HYPERSCROLL_DEBUG(...) \
	do \
	{ \
		if (cs2ac_hyperscroll_debug.GetBool()) \
			Msg("[CS2AC Hyperscroll] " __VA_ARGS__); \
	} while (0)

#define JUMP_INPUT_DEBUG(...) \
	do \
	{ \
		BHOP_DEBUG(__VA_ARGS__); \
		HYPERSCROLL_DEBUG(__VA_ARGS__); \
	} while (0)

#define JUMP_PATTERN_WINDOW      0.25f
#define MIN_AIR_TIME_FOR_BHOP    (4.0f * ENGINE_FIXED_TICK_INTERVAL)            // Minimum air time to consider a jump for bhop hack detection
#define BHOP_IGNORE_DURATION     (4.0f * ENGINE_FIXED_TICK_INTERVAL)            // Ignore teleports/noclips in the last 4 ticks
#define OLD_JUMP_PURGE_THRESHOLD (JUMP_PATTERN_WINDOW * ENGINE_FIXED_TICK_RATE) // Purge jump attempts older than 0.25s
#define MIN_SAMPLE_COUNT         20                                             // Minimum number of samples before we start checking for bhop hacks
#define WINDOW_SIZE              30                                             // Number of recent jumps to consider for bhop hack detection
// Number of consecutive perfect bhops in the window to trigger a bhop hack infraction, regardless of ratio
#define NUM_CONSECUTIVE_PERFS_FOR_INFRACTION 10
// Perfect jumps add one and failed jumps remove one before repetitive input patterns can trigger an infraction.
#define PERF_SCORE_FOR_PATTERN_CHECK          7
#define PERF_RATIO_FOR_HYPERSCROLL_INFRACTION 0.6f

#define REPETITIVE_PATTERN_THRESHOLD 0.9f // If 90% of the perfs are the same pattern, it might be a cheat...
#define LOW_PATTERN_THRESHOLD        4    // ...if the most common pattern is smaller than 4.
#define HIGH_PATTERN_THRESHOLD       16.0f

namespace
{
	bool ShouldTrackBhop()
	{
		return settings::IsDetectionEnabled(DetectionType::Bhop) || settings::IsDetectionEnabled(DetectionType::Hyperscroll);
	}
} // namespace

void MovementDetectionService::ParseCommandForJump(PlayerCommand *cmd)
{
	if (!ShouldTrackBhop() || this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}

	auto recordJump = [this](f32 jumpTime)
	{
		this->recentJumps.push_back(jumpTime);
		for (auto &event : this->recentLandingEvents)
		{
			const f32 timeSinceLanding = jumpTime - static_cast<f32>(event.cmdNum);
			if (timeSinceLanding >= 0.0f && timeSinceLanding <= OLD_JUMP_PURGE_THRESHOLD)
			{
				event.numJumpAfter++;
			}
		}
		this->bhopDirty = true;
		JUMP_INPUT_DEBUG("%s recorded a jump press at command time %.3f; %zu recent presses remain in the pattern window.\n", this->player->GetName(),
						 jumpTime, this->recentJumps.size());
	};

	if (cmd->base().subtick_moves_size() == 0)
	{
		if (cmd->buttonstates.IsButtonNewlyPressed(IN_JUMP))
		{
			recordJump(static_cast<f32>(this->currentCmdNum));
		}
	}
	else // Has subtick moves
	{
		// Assume that the +jump inputs are all legit. Faking +jump's wouldn't really help with anything.
		for (i32 i = 0; i < cmd->base().subtick_moves_size(); i++)
		{
			const CSubtickMoveStep &step = cmd->base().subtick_moves(i);
			if (step.button() == IN_JUMP && step.pressed())
			{
				if (!std::isfinite(step.when()) || step.when() < 0.0f || step.when() > 1.0f)
				{
					JUMP_INPUT_DEBUG("%s evidence reset: jump input command %d had invalid subtick time %.6f.\n", this->player->GetName(),
									 cmd->cmdNum, step.when());
					this->recentJumps.clear();
					this->recentLandingEvents.clear();
					this->bhopDirty = false;
					this->currentAirTime = 0.0f;
					this->airMovedThisFrame = false;
					return;
				}
				recordJump(static_cast<f32>(this->currentCmdNum) + step.when());
			}
		}
	}

	// Purge old jump attempts
	// clang-format off
	this->recentJumps.erase(
		std::remove_if(
			this->recentJumps.begin(), this->recentJumps.end(),
			[&](f32 when) {
				return (this->currentCmdNum - when) > OLD_JUMP_PURGE_THRESHOLD;
			}),
		this->recentJumps.end());
	// clang-format on
}

void MovementDetectionService::CreateLandEvent()
{
	if (!ShouldTrackBhop() || this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	if (!this->ShouldRunDetections())
	{
		JUMP_INPUT_DEBUG("%s landing ignored: inherited movement detections are currently unavailable.\n", this->player->GetName());
		return;
	}
	// Check for minimum air time
	if (this->currentAirTime < MIN_AIR_TIME_FOR_BHOP)
	{
		JUMP_INPUT_DEBUG("%s landing ignored: %.4f seconds in air is below the %.4f-second minimum.\n", this->player->GetName(), this->currentAirTime,
						 MIN_AIR_TIME_FOR_BHOP);
		return;
	}
	// Check for valid movetype
	const f32 timeSinceInvalidMoveType = g_pCS2ACUtils->GetServerGlobals()->curtime - this->lastValidMoveTypeTime;
	if (timeSinceInvalidMoveType < BHOP_IGNORE_DURATION)
	{
		JUMP_INPUT_DEBUG("%s landing ignored: a non-walking movement type was used %.4f seconds ago.\n", this->player->GetName(),
						 timeSinceInvalidMoveType);
		return;
	}
	// Check for teleport
	if (this->player->JustTeleported(BHOP_IGNORE_DURATION))
	{
		JUMP_INPUT_DEBUG("%s landing ignored: the player teleported within the four-tick grace period.\n", this->player->GetName());
		return;
	}
	// Check sv_autobunnyhopping
	if (this->player->GetMovementSetting(MOVEMENT_SETTING_SV_AUTOBUNNYHOPPING)->m_bValue != false)
	{
		JUMP_INPUT_DEBUG("%s landing ignored: sv_autobunnyhopping is enabled.\n", this->player->GetName());
		return;
	}
	auto &event = this->recentLandingEvents.emplace_back();
	event.cmdNum = this->currentCmdNum;
	event.landingTime = this->player->landingTime;
	event.numJumpBefore = this->recentJumps.size();
	event.shouldCountTowardsPerfChains =
		this->player->GetMovementSetting(MOVEMENT_SETTING_SV_JUMP_SPAM_PENALTY_TIME)->m_fl32Value >= ENGINE_FIXED_TICK_INTERVAL;
	this->bhopDirty = true;
	JUMP_INPUT_DEBUG("%s accepted landing %zu of %d at command %d: %.4f seconds in air, %u earlier jump presses, chain eligible: %s.\n",
					 this->player->GetName(), this->recentLandingEvents.size(), MIN_SAMPLE_COUNT, event.cmdNum, this->currentAirTime,
					 event.numJumpBefore, event.shouldCountTowardsPerfChains ? "yes" : "no");
}

void MovementDetectionService::OnChangeMoveType(MoveType_t oldMoveType)
{
	if (!ShouldTrackBhop())
	{
		return;
	}
	if (oldMoveType != MOVETYPE_WALK || this->player->GetMoveType() != MOVETYPE_WALK)
	{
		this->lastValidMoveTypeTime = g_pCS2ACUtils->GetServerGlobals()->curtime;
	}
}

void MovementDetectionService::OnProcessMovement()
{
	if (!ShouldTrackBhop())
	{
		return;
	}
	this->airMovedThisFrame = false;
}

void MovementDetectionService::OnAirMove()
{
	if (!ShouldTrackBhop())
	{
		return;
	}
	this->airMovedThisFrame = true;
	this->currentAirTime += g_pCS2ACUtils->GetGlobals()->frametime;
}

void MovementDetectionService::OnProcessMovementPost()
{
	if (!ShouldTrackBhop())
	{
		return;
	}
	if (!this->airMovedThisFrame)
	{
		this->currentAirTime = 0.0f;
		if (!this->recentLandingEvents.empty() && this->recentLandingEvents.back().pendingPerf)
		{
			this->recentLandingEvents.back().pendingPerf = false;
			this->bhopDirty = true;
		}
	}
}

void MovementDetectionService::OnJump()
{
	if (!ShouldTrackBhop() || !this->ShouldRunDetections())
	{
		return;
	}
	const bool hasPendingLanding = !this->recentLandingEvents.empty() && this->recentLandingEvents.back().pendingPerf;
	const bool framePerfect = this->player->IsPerfing(true);
	JUMP_INPUT_DEBUG("%s server jump: frame-perfect %s, pending accepted landing %s.\n", this->player->GetName(), framePerfect ? "yes" : "no",
					 hasPendingLanding ? "yes" : "no");
	if (framePerfect && hasPendingLanding)
	{
		recentLandingEvents.back().hasPerfectBhop = true;
		this->bhopDirty = true;
	}
}

void MovementDetectionService::CheckLandingEvents()
{
	if (!ShouldTrackBhop() || !this->bhopDirty || this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	this->bhopDirty = false;
	while (this->recentLandingEvents.size() > WINDOW_SIZE)
	{
		this->recentLandingEvents.pop_front();
	}
	if (this->recentLandingEvents.size() >= MIN_SAMPLE_COUNT)
	{
		u32 numPerfs = 0;
		u32 totalChainEligibleEvents = 0;
		u32 maxPerfChain = 0;
		u32 currentPerfChain = 0;
		u32 maxPatternPerfScore = 0;
		u32 currentPatternPerfScore = 0;
		std::unordered_map<u32, u32> patterns;
		u32 mostCommonPattern = 0;
		u32 mostCommonPatternCount = 0;
		u32 totalPatternOccurrences = 0;
		u32 weightedPatternSum = 0;
		const f32 currentTime = g_pCS2ACUtils->GetGlobals()->curtime;
		for (const auto &event : this->recentLandingEvents)
		{
			const f32 patternAge = currentTime - event.landingTime;
			if (!std::isfinite(patternAge) || patternAge < JUMP_PATTERN_WINDOW)
			{
				this->bhopDirty |= std::isfinite(patternAge) && patternAge >= 0.0f;
			}
			else if (event.numJumpAfter > 0 || event.numJumpBefore > 0)
			{
				u32 pattern = event.numJumpBefore + event.numJumpAfter;
				u32 count = ++patterns[pattern];
				totalPatternOccurrences++;
				weightedPatternSum += pattern;
				if (count > mostCommonPatternCount)
				{
					mostCommonPatternCount = count;
					mostCommonPattern = pattern;
				}
			}
			if (!event.shouldCountTowardsPerfChains)
			{
				continue;
			}
			totalChainEligibleEvents++;
			if (event.hasPerfectBhop)
			{
				numPerfs++;
				currentPerfChain++;
				maxPerfChain = Max(maxPerfChain, currentPerfChain);
				currentPatternPerfScore++;
				maxPatternPerfScore = Max(maxPatternPerfScore, currentPatternPerfScore);
			}
			else
			{
				currentPerfChain = 0;
				currentPatternPerfScore = currentPatternPerfScore > 0 ? currentPatternPerfScore - 1 : 0;
			}
		}
		f32 averagePattern = totalPatternOccurrences > 0 ? (f32)weightedPatternSum / (f32)totalPatternOccurrences : 0.0f;
		BHOP_DEBUG("%s evaluation: %zu landings, %u eligible, %u perfect, longest perfect chain %u, best pattern score %u, %u completed patterns, "
				   "dominant pattern %u repeated %u times.\n",
				   this->player->GetName(), this->recentLandingEvents.size(), totalChainEligibleEvents, numPerfs, maxPerfChain, maxPatternPerfScore,
				   totalPatternOccurrences, mostCommonPattern, mostCommonPatternCount);

		// Hard consecutive perf chain check.
		if (maxPerfChain >= NUM_CONSECUTIVE_PERFS_FOR_INFRACTION && settings::IsDetectionEnabled(DetectionType::Bhop))
		{
			this->MarkInfraction(
				MovementDetectionService::Infraction::Type::BhopHack,
				localization::Format("evidence.bhop.perfect_chain",
									 "{perfect} of {landings} eligible landings formed one consecutive frame-perfect bhop chain.",
									 {{"perfect", tfm::format("%u", maxPerfChain)}, {"landings", tfm::format("%u", totalChainEligibleEvents)}}));
			this->recentLandingEvents.clear();
			this->bhopDirty = false;
			return;
		}

		// Pattern-based bhop infraction after a decaying perfect-jump score.
		if (maxPatternPerfScore >= PERF_SCORE_FOR_PATTERN_CHECK)
		{
			if (totalPatternOccurrences >= PERF_SCORE_FOR_PATTERN_CHECK
				&& mostCommonPatternCount >= totalPatternOccurrences * REPETITIVE_PATTERN_THRESHOLD && mostCommonPattern < LOW_PATTERN_THRESHOLD
				&& settings::IsDetectionEnabled(DetectionType::Bhop))
			{
				this->MarkInfraction(
					MovementDetectionService::Infraction::Type::BhopHack,
					localization::Format(
						"evidence.bhop.repeated_pattern",
						"{repeated} of {patterns} completed jump patterns repeated {inputs} inputs, with an average of {average} inputs.",
						{{"repeated", tfm::format("%u", mostCommonPatternCount)},
						 {"patterns", tfm::format("%u", totalPatternOccurrences)},
						 {"inputs", tfm::format("%u", mostCommonPattern)},
						 {"average", tfm::format("%.2f", averagePattern)}}));
				this->recentLandingEvents.clear();
				this->bhopDirty = false;
				return;
			}
		}

		// Hyperscroll check
		f32 perfectRatio = totalChainEligibleEvents > 0 ? (f32)numPerfs / (f32)totalChainEligibleEvents : 0.0f;
		HYPERSCROLL_DEBUG("%s evaluation: %u completed patterns averaged %.2f jump presses; %u of %u eligible landings were "
						  "frame-perfect (%.2f%%).\n",
						  this->player->GetName(), totalPatternOccurrences, averagePattern, numPerfs, totalChainEligibleEvents,
						  perfectRatio * 100.0f);
		if (averagePattern >= HIGH_PATTERN_THRESHOLD && perfectRatio > PERF_RATIO_FOR_HYPERSCROLL_INFRACTION
			&& totalPatternOccurrences >= MIN_SAMPLE_COUNT && totalChainEligibleEvents >= MIN_SAMPLE_COUNT
			&& settings::IsDetectionEnabled(DetectionType::Hyperscroll))
		{
			this->MarkInfraction(
				MovementDetectionService::Infraction::Type::Hyperscroll,
				localization::Format("evidence.hyperscroll",
									 "The player averaged {average} jump inputs across {patterns} completed landing patterns, while {perfect} of "
									 "{landings} eligible landings were frame-perfect ({ratio}%).",
									 {{"average", tfm::format("%.2f", averagePattern)},
									  {"patterns", tfm::format("%u", totalPatternOccurrences)},
									  {"perfect", tfm::format("%u", numPerfs)},
									  {"landings", tfm::format("%u", totalChainEligibleEvents)},
									  {"ratio", tfm::format("%.2f", perfectRatio * 100.0f)}}));
			this->recentLandingEvents.clear();
			this->bhopDirty = false;
			return;
		}
	}
}
