// Detects automated air strafing from repeated, mechanically perfect jump data.

#include "movement_analysis/detection/movement_detection.h"
#include "movement_analysis/jump_analysis/jump_analysis.h"
#include "settings.h"

CConVar<bool> cs2ac_autostrafe_debug("cs2ac_autostrafe_debug", FCVAR_NONE, "Show Autostrafe jump statistics, optimizer matches, and evidence counts",
									 false);

#define AUTOSTRAFE_DEBUG(...) \
	do \
	{ \
		if (cs2ac_autostrafe_debug.GetBool()) \
			Msg("[CS2AC Autostrafe] " __VA_ARGS__); \
	} while (0)

#define MIN_JUMP_DURATION_FOR_DETECTION 0.6f // Only consider jumps longer than this duration
#define MIN_SYNC_FOR_DETECTION          0.7f // Minimum sync ratio to consider for detection

#define NUM_JUMPS_WINDOW_SIZE           20 // How many recent jumps to consider for autostrafe detection
#define BASE_SUSPICIOUS_JUMPS_THRESHOLD 15 // Minimum number of high strafe count jumps within the window to consider for detection.

// If the player has a high number of strafes per second, mark them as suspicious.
#define BASE_STRAFES_PER_SECOND_THRESHOLD 20.0f

// Sometimes unstable mouse can cause 1 tick strafes.
#define MIN_STRAFE_DURATION              0.02f // If we ignore strafes shorter than this duration...
#define REAL_STRAFE_PER_SECOND_THRESHOLD 18.0f // then this is the threshold for suspicious strafes per second.

#define MAX_STRAFES_PER_SECOND_THRESHOLD 30.0f // At this many strafes per second, even a few jumps are suspicious
#define MIN_SUSPICIOUS_JUMPS_THRESHOLD   5     //... in which case we lower the threshold to this many jumps.

// High gain efficiency jumps are suspicious if the player has a high number of strafes per second or if the jump distance is significant.
#define GAIN_EFFICIENCY_THRESHOLD               0.95f // Roughly 397 units for a standard long jump.
#define STRAFE_PER_SECOND_THRESHOLD_FOR_GAINEFF 6.0f
#define JUMP_DISTANCE_THRESHOLD_FOR_GAINEFF     240.0f

void MovementDetectionService::OnJumpFinish(Jump *jump)
{
	if (this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	if (!this->ShouldRunDetections())
	{
		AUTOSTRAFE_DEBUG("%s jump ignored: inherited movement detections are currently unavailable.\n", this->player->GetName());
		this->recentJumpStatuses.clear();
		return;
	}
	if (!settings::IsDetectionEnabled(DetectionType::Autostrafe))
	{
		this->recentJumpStatuses.clear();
		return;
	}
	if (jump->invalidateReason[0] != '\0' || !jump->IsValid())
	{
		AUTOSTRAFE_DEBUG("%s jump rejected: %s.\n", this->player->GetName(),
						 jump->invalidateReason[0] != '\0' ? jump->invalidateReason : "jump data is invalid");
		return;
	}
	if (jump->airtime < MIN_JUMP_DURATION_FOR_DETECTION)
	{
		AUTOSTRAFE_DEBUG("%s jump rejected: %.3f seconds in air is below the %.3f-second minimum.\n", this->player->GetName(), jump->airtime,
						 MIN_JUMP_DURATION_FOR_DETECTION);
		return;
	}

	// Rejected jumps are skipped rather than treated as normal or used to erase earlier valid evidence.
	if (jump->GetSync() <= MIN_SYNC_FOR_DETECTION)
	{
		this->recentJumpStatuses.push_back(JumpStatus::Normal);
	}
	else if (jump->strafes.Count() / jump->airtime > MAX_STRAFES_PER_SECOND_THRESHOLD)
	{
		this->recentJumpStatuses.push_back(JumpStatus::VeryHighStrafeCount);
	}
	else if (jump->strafes.Count() / jump->airtime > BASE_STRAFES_PER_SECOND_THRESHOLD)
	{
		this->recentJumpStatuses.push_back(JumpStatus::HighStrafeCount);
	}
	else
	{
		i32 numEffectiveStrafes = 0;
		FOR_EACH_VEC(jump->strafes, i)
		{
			if (jump->strafes[i].duration >= MIN_STRAFE_DURATION)
			{
				numEffectiveStrafes++;
			}
		}
		f32 effectiveStrafesPerSecond = (f32)numEffectiveStrafes / jump->airtime;
		if (effectiveStrafesPerSecond > REAL_STRAFE_PER_SECOND_THRESHOLD)
		{
			this->recentJumpStatuses.push_back(JumpStatus::HighStrafeCount);
		}
		else if (effectiveStrafesPerSecond > STRAFE_PER_SECOND_THRESHOLD_FOR_GAINEFF
				 && (jump->GetGainEfficiency() > GAIN_EFFICIENCY_THRESHOLD && jump->GetDistance() > JUMP_DISTANCE_THRESHOLD_FOR_GAINEFF))
		{
			this->recentJumpStatuses.push_back(JumpStatus::HighEfficiency);
		}
		else
		{
			this->recentJumpStatuses.push_back(JumpStatus::Normal);
		}
	}
	// Keep only the recent N jumps.
	while (this->recentJumpStatuses.size() > NUM_JUMPS_WINDOW_SIZE)
	{
		this->recentJumpStatuses.pop_front();
	}
	// Count suspicious jumps.
	i32 suspiciousJumpCount = 0;
	i32 numVeryHighStrafeJumps = 0;
	for (auto status : this->recentJumpStatuses)
	{
		if (status != JumpStatus::Normal)
		{
			suspiciousJumpCount++;
		}
		if (status == JumpStatus::VeryHighStrafeCount)
		{
			numVeryHighStrafeJumps++;
		}
	}
	const char *statusName = "normal";
	switch (this->recentJumpStatuses.back())
	{
		case JumpStatus::HighStrafeCount:
			statusName = "high strafe count";
			break;
		case JumpStatus::VeryHighStrafeCount:
			statusName = "very high strafe count";
			break;
		case JumpStatus::HighEfficiency:
			statusName = "high efficiency";
			break;
		case JumpStatus::Normal:
			break;
	}
	AUTOSTRAFE_DEBUG("%s jump evaluated as %s: %.1f%% sync, %.2f strafes/s, %.1f%% gain efficiency, %.2f units; "
					 "%d of %zu recent jumps are suspicious.\n",
					 this->player->GetName(), statusName, jump->GetSync() * 100.0f, jump->strafes.Count() / jump->airtime,
					 jump->GetGainEfficiency() * 100.0f, jump->GetDistance(), suspiciousJumpCount, this->recentJumpStatuses.size());
	if (suspiciousJumpCount >= BASE_SUSPICIOUS_JUMPS_THRESHOLD
		|| (suspiciousJumpCount >= MIN_SUSPICIOUS_JUMPS_THRESHOLD && numVeryHighStrafeJumps > 0))
	{
		const size_t evaluatedJumpCount = this->recentJumpStatuses.size();
		localization::Text details = localization::Format(
			"evidence.autostrafe.jumps", "{suspicious} of {evaluated} evaluated jumps matched automated strafe patterns ({ratio}%).",
			{{"suspicious", tfm::format("%d", suspiciousJumpCount)},
			 {"evaluated", tfm::format("%zu", evaluatedJumpCount)},
			 {"ratio", tfm::format("%.2f", (f32)suspiciousJumpCount / (f32)evaluatedJumpCount * 100.0f)}});
		this->MarkInfraction(Infraction::Type::StrafeHack, details);
		this->recentJumpStatuses.clear();
	}
}
