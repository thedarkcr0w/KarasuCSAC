#include "movement_detection.h"

#include "cs2ac.h"
#include "sdk/usercmd.h"
#include "settings.h"

extern CConVar<bool> cs2ac_invalid_cvar_debug;

#define INVALID_CVAR_DEBUG(...) \
	do \
	{ \
		if (cs2ac_invalid_cvar_debug.GetBool()) \
			Msg("[CS2AC Invalid CVar] " __VA_ARGS__); \
	} while (0)

namespace
{
	DetectionType DetectionForInfraction(MovementDetectionService::Infraction::Type type)
	{
		using Type = MovementDetectionService::Infraction::Type;
		switch (type)
		{
			case Type::StrafeHack:
				return DetectionType::Autostrafe;
			case Type::BhopHack:
				return DetectionType::Bhop;
			case Type::Hyperscroll:
				return DetectionType::Hyperscroll;
			case Type::InvalidCvar:
				return DetectionType::InvalidCvar;
			case Type::InvalidInput:
				return DetectionType::InvalidInput;
			case Type::Nulls:
				return DetectionType::Nulls;
			case Type::SubtickSpam:
				return DetectionType::SubtickSpam;
			case Type::Desubtick:
				return DetectionType::Desubticking;
			case Type::Other:
			case Type::COUNT:
				return DetectionType::Count;
		}
		return DetectionType::Count;
	}
} // namespace

void MovementDetectionService::Reset()
{
	currentMaxFps = 0.0f;
	cvarMonitorStarted = false;
	cvarQueryIndex = 0;
	cvarCycleInterval = 0.0;
	nullsFramerateBuffer.reserve(maxNullInputEvents);
	nullsUnderlapBuffer.reserve(maxNullInputEvents);
	invalidCvarLatches.clear();
	invalidQueriedCvars.clear();
	invalidUserInfoCvars.clear();
	ResetTransientDetectionState();
	settingsMask = settings::GetDetectionMask();
	settingsRevision = settings::GetRevision();
}

void MovementDetectionService::ResetTransientDetectionState()
{
	currentMaxFps = 0.0f;
	currentCmdNum = 0;
	lastNullsCmdNum = -1;
	forwardBackwardNullsDirty = false;
	leftRightNullsDirty = false;
	nullsFramerateBuffer.clear();
	nullsUnderlapBuffer.clear();
	invalidCvarLatches.clear();
	invalidQueriedCvars.clear();
	invalidUserInfoCvars.clear();
	ClearDetectionBuffers();
}

void MovementDetectionService::RefreshSettings()
{
	const std::uint64_t currentMask = settings::GetDetectionMask();
	const std::uint64_t currentRevision = settings::GetRevision();
	if (currentMask != settingsMask || currentRevision != settingsRevision)
	{
		ResetTransientDetectionState();
		settingsMask = currentMask;
		settingsRevision = currentRevision;
	}
}

void MovementDetectionService::OnPlayerFullyConnect()
{
	if (!player->IsFakeClient() && !player->IsCSTV())
	{
		InitCvarMonitor();
	}
}

void MovementDetectionService::OnSetupMove(PlayerCommand *command)
{
	RefreshSettings();
	if (player->IsFakeClient() || player->IsCSTV() || !ShouldRunDetections() || !player->IsAlive())
	{
		if (!player->IsAlive())
		{
			ClearDetectionBuffers();
			lastNullsCmdNum = -1;
			forwardBackwardNullsDirty = false;
			leftRightNullsDirty = false;
		}
		return;
	}

	currentCmdNum = command->cmdNum;
	CheckSubtickAbuse(command);
	if (settings::IsDetectionEnabled(DetectionType::Nulls))
	{
		CreateInputEvents(command);
	}
	ParseCommandForJump(command);
	if (settings::IsDetectionEnabled(DetectionType::Autostrafe))
	{
		DetectOptimization(command);
	}
}

void MovementDetectionService::OnPhysicsSimulatePost()
{
	RefreshSettings();
	if (player->IsFakeClient() || player->IsCSTV() || !ShouldRunDetections() || !player->IsAlive())
	{
		return;
	}
	if (settings::IsDetectionEnabled(DetectionType::Nulls))
	{
		CheckNulls();
	}
	CheckSuspiciousSubtickCommands();
	CheckLandingEvents();
}

void MovementDetectionService::ClearDetectionBuffers()
{
	recentForwardBackwardEvents.clear();
	recentLeftRightEvents.clear();
	suspiciousSubtickMoveTimes.clear();
	invalidCommandTimes.clear();
	zeroWhenCommandTimes.clear();
	numCommandsWithSubtickInputs.clear();
	recentJumpStatuses.clear();
	recentJumps.clear();
	recentLandingEvents.clear();
	bhopDirty = false;
	currentAirTime = 0.0f;
	airMovedThisFrame = false;
	lastValidMoveTypeTime = -1.0f;
	angleFrameHistory.clear();
	yawAccelPercent = 0.0f;
}

void MovementDetectionService::MarkInvalidCvar(const char *cvarName, const localization::Text &reason, bool kickOnly)
{
	RefreshSettings();
	if (!cvarName)
	{
		return;
	}
	if (invalidCvarLatches.emplace(cvarName).second)
	{
		INVALID_CVAR_DEBUG("%s marked %s invalid and armed its latch: %s\n", player->GetName(), cvarName, reason.english.c_str());
		MarkInfraction(Infraction::Type::InvalidCvar, reason, kickOnly);
	}
	else
	{
		INVALID_CVAR_DEBUG("%s still has invalid %s; its existing latch prevented a duplicate detection.\n", player->GetName(), cvarName);
	}
}

void MovementDetectionService::MarkValidCvar(const char *cvarName)
{
	if (cvarName && invalidCvarLatches.erase(cvarName) > 0)
	{
		INVALID_CVAR_DEBUG("%s returned %s to a valid value; its latch is armed for a future change.\n", player->GetName(), cvarName);
	}
}

void MovementDetectionService::MarkCvarSource(const char *cvarName, const localization::Text &reason, bool invalid, bool userInfo, bool kickOnly)
{
	auto &source = userInfo ? invalidUserInfoCvars : invalidQueriedCvars;
	if (invalid)
	{
		source.emplace(cvarName);
		MarkInvalidCvar(cvarName, reason, kickOnly);
		return;
	}
	source.erase(cvarName);
	const auto &other = userInfo ? invalidQueriedCvars : invalidUserInfoCvars;
	if (other.find(cvarName) == other.end())
	{
		MarkValidCvar(cvarName);
	}
}

void MovementDetectionService::MarkInfraction(Infraction::Type type, const localization::Text &reason, bool kickOnly)
{
	RefreshSettings();
	const auto index = static_cast<u8>(type);
	if (index >= static_cast<u8>(Infraction::Type::COUNT))
	{
		return;
	}
	const DetectionType detection = DetectionForInfraction(type);
	if (!settings::IsDetectionEnabled(detection))
	{
		return;
	}
	g_CS2AC.HandleDetection(Infraction::displayNames[index], player, reason, kickOnly);
}
