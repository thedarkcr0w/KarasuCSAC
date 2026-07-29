#pragma once

#include "localization.h"
#include "movement_analysis/player_context.h"

class Jump;

class MovementDetectionService : public PlayerService
{
public:
	using PlayerService::PlayerService;

	struct Infraction
	{
		// These categories connect movement evidence to CS2AC's public detection names.
		enum class Type : u8
		{
			Other = 0,
			// Detects automated or mathematically optimized air strafing.
			StrafeHack,
			// Detects repeated, mechanically perfect bunny hops.
			BhopHack,
			// Detects impossible or automated jump-input frequency.
			Hyperscroll,
			// Detects client settings outside the values accepted by CS2AC.
			InvalidCvar,
			// Detects movement button changes without matching subtick records.
			InvalidInput,
			// Detects mechanically perfect airborne opposite-direction switches.
			Nulls,
			// Detects repeated same-time button aliases carrying pitch or yaw changes.
			SubtickSpam,
			// Detects commands engineered to remove normal subtick timing.
			Desubtick,
			COUNT,
		};

		static inline const char *displayNames[] = {
			"OTHER", "AUTOSTRAFE", "BHOP", "HYPERSCROLL", "INVALID CVAR", "INVALID INPUT", "NULLS", "SUBTICK SPAM", "DESUBTICKING",
		};
	};

	void Reset() override;
	void ResetTransientDetectionState();
	bool ShouldRunDetections() const;
	bool ShouldCheckClientCvars() const;

	static void InitSvCheatsWatcher();
	static void CleanupSvCheatsWatcher();
	static bool IsSvCheatsTestingAllowed();
	void InitCvarMonitor();

	struct InputEvent
	{
		i32 cmdNum;
		f32 fraction;
		f32 framerate;
		u64 button;
		bool pressed;
		bool analog = false;
		f32 airSpeed = -1.0f;
	};

	std::deque<InputEvent> recentForwardBackwardEvents;
	std::deque<InputEvent> recentLeftRightEvents;
	i32 lastNullsCmdNum {-1};
	bool forwardBackwardNullsDirty {};
	bool leftRightNullsDirty {};
	std::vector<f32> nullsFramerateBuffer;
	std::vector<f32> nullsUnderlapBuffer;
	void CreateInputEvents(PlayerCommand *command);
	void CheckNulls();
	void AnalyzeNullsForAxis(const std::deque<InputEvent> &events, u64 button1, u64 button2);

	std::deque<f32> recentJumps;

	struct LandingEvent
	{
		i32 cmdNum;
		f32 landingTime;
		bool pendingPerf = true;
		bool hasPerfectBhop {};
		bool shouldCountTowardsPerfChains {};
		u32 numJumpBefore {};
		u32 numJumpAfter {};
	};

	f32 lastValidMoveTypeTime = -1.0f;
	std::deque<LandingEvent> recentLandingEvents;
	bool bhopDirty {};
	void ParseCommandForJump(PlayerCommand *command);
	void CreateLandEvent();
	void CheckLandingEvents();
	void OnChangeMoveType(MoveType_t oldMoveType);
	f32 currentAirTime {};
	bool airMovedThisFrame {};
	void OnProcessMovement();
	void OnAirMove();
	void OnProcessMovementPost();

	std::deque<f32> suspiciousSubtickMoveTimes;
	std::deque<f32> invalidCommandTimes;
	std::deque<f32> zeroWhenCommandTimes;
	std::deque<f32> numCommandsWithSubtickInputs;
	void CheckSubtickAbuse(PlayerCommand *command);
	void CheckSuspiciousSubtickCommands();

	struct AngleFrame
	{
		i32 cmdNum;
		QAngle angle;
		f32 frametime;
	};

	f32 yawAccelPercent {};
	std::deque<AngleFrame> angleFrameHistory;
	f32 CalculateYawSpeed(size_t index);
	f32 CalculateYawAccel(size_t index);
	void DetectOptimization(PlayerCommand *command);

	void OnJump();
	void OnSetupMove(PlayerCommand *command);
	void OnPhysicsSimulatePost();
	void OnPlayerFullyConnect();

	enum class JumpStatus : u8
	{
		Normal = 0,
		HighStrafeCount,
		VeryHighStrafeCount,
		HighEfficiency,
	};
	std::deque<JumpStatus> recentJumpStatuses;
	void OnJumpFinish(Jump *jump);

	f32 currentMaxFps {};
	bool cvarMonitorStarted {};
	std::set<std::string> invalidCvarLatches;
	std::set<std::string> invalidQueriedCvars;
	std::set<std::string> invalidUserInfoCvars;
	void MarkInvalidCvar(const char *cvarName, const localization::Text &reason, bool kickOnly = false);
	void MarkValidCvar(const char *cvarName);
	void MarkCvarSource(const char *cvarName, const localization::Text &reason, bool invalid, bool userInfo, bool kickOnly = false);
	void MarkInfraction(Infraction::Type type, const localization::Text &reason, bool kickOnly = false);

private:
	void RefreshSettings();
	u32 currentCmdNum {};
	std::uint64_t settingsMask {};
	std::uint64_t settingsRevision {};
	void ClearDetectionBuffers();
};
