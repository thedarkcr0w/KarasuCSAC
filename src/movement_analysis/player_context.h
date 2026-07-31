#pragma once

#include "common.h"
#include "movement/movement.h"

enum MovementSetting : int;

#define CS2AC_RECENT_TELEPORT_THRESHOLD 0.05f

class CS2ACPlayer;
class MovementDetectionService;
class JumpAnalysisService;
class MovementEventService;

class PlayerService
{
public:
	explicit PlayerService(CS2ACPlayer *player) : player(player) {}

	virtual ~PlayerService() = default;

	virtual void Reset() {}

	CS2ACPlayer *player;
};

class CS2ACPlayer : public MovementPlayer
{
public:
	explicit CS2ACPlayer(i32 index) : MovementPlayer(index)
	{
		Init();
	}

	~CS2ACPlayer() override;

	void Init() override;
	void Reset() override;
	void OnPlayerFullyConnect() override;
	void OnPhysicsSimulate() override;
	void OnPhysicsSimulatePost() override;
	void OnProcessUsercmds(PlayerCommand *commands, int numCommands) override;
	void OnSetupMove(PlayerCommand *command) override;
	void OnProcessMovement() override;
	void OnProcessMovementPost() override;
	void OnDuck() override;
	void OnJumpLegacy() override;
	void OnJumpLegacyPost() override;
	void OnJumpModern() override;
	void OnJumpModernPost() override;
	void OnAirMove() override;
	void OnAirAccelerate(Vector &wishdir, f32 &wishspeed, f32 &accel) override;
	void OnAirAcceleratePost(Vector wishdir, f32 wishspeed, f32 accel) override;
	void OnTryPlayerMove(Vector *destination, trace_t *trace, bool *surfing) override;
	void OnTryPlayerMovePost(Vector *destination, trace_t *trace, bool *surfing) override;
	void OnStartTouchGround() override;
	void OnStopTouchGround() override;
	void OnChangeMoveType(MoveType_t oldMoveType) override;
	void OnTeleport(const Vector *origin, const QAngle *angles, const Vector *velocity) override;

	bool JustTeleported(f32 threshold = CS2AC_RECENT_TELEPORT_THRESHOLD);
	const CVValue_t *GetMovementSetting(MovementSetting setting);
	void PrintDebug(const char *format, ...);

	MovementDetectionService *movementDetection {};
	JumpAnalysisService *jumpAnalysis {};
	MovementEventService *movementEvents {};

private:
	f64 lastTeleportTime {};
	bool jumpAnalysisActive {};
};

class CS2ACPlayerManager : public MovementPlayerManager
{
public:
	CS2ACPlayerManager();
	CS2ACPlayer *ToPlayer(CPlayerPawnComponent *component);
	CS2ACPlayer *ToPlayer(CBasePlayerController *controller);
	CS2ACPlayer *ToPlayer(CBasePlayerPawn *pawn);
	CS2ACPlayer *ToPlayer(CPlayerSlot slot);
	CS2ACPlayer *ToPlayer(CEntityIndex index);
	CS2ACPlayer *ToPlayer(CPlayerUserId userID);
	CS2ACPlayer *ToPlayer(u32 index);
	CS2ACPlayer *SteamIdToPlayer(u64 steamID, bool validated = true);
};

extern CS2ACPlayerManager *g_pCS2ACPlayerManager;
