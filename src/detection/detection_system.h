#pragma once

#include "common.h"
#include "localization.h"
#include "sdk/datatypes.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

class IGameEvent;
class MovementPlayer;
class PlayerCommand;
class CMsgTEFireBullets;
class CMsgPlayerBulletHit;

namespace detection
{
	using Clock = std::chrono::steady_clock;
	using AnnounceCallback = void (*)(const char *detection, MovementPlayer *player, const localization::Text &evidence);

	bool IsBallisticWeapon(std::string_view weapon);
	bool IsEligibleHuman(MovementPlayer *player);
	bool AreOpponents(int firstTeam, int secondTeam);
	bool IsFinite(const QAngle &angles);
	bool IsFinite(const Vector &vector);
	Vector AimForward(const QAngle &angles);
	float AngularDistance(const QAngle &first, const QAngle &second);
	std::string_view NormalizeWeapon(std::string_view weapon);
	localization::Text FormatEvidenceHistory(const std::vector<localization::Text> &history, const localization::Text &latest);

	struct NetworkSafetyEvidence
	{
		float pingMilliseconds {};
		float jitterMilliseconds {};
		float incomingLoss {};
		float outgoingLoss {};
		float incomingChoke {};
		float outgoingChoke {};
		int commandGaps {};
		int unavailableSamples {};
		bool vetoed {};
	};

	localization::Text AddNetworkSafetyDetails(localization::Text details, const NetworkSafetyEvidence &evidence);

	struct NetworkSafetySample
	{
		Clock::time_point time;
		float pingMilliseconds {};
		float incomingLoss {};
		float outgoingLoss {};
		float incomingChoke {};
		float outgoingChoke {};
		bool valid {};
	};

	struct NetworkSafetyPlayerData
	{
		int lastCommandNumber {-1};
		int lastClientTick {-1};
		Clock::time_point nextSample;
		std::deque<Clock::time_point> commandGaps;
		std::deque<NetworkSafetySample> samples;
	};

	class NetworkSafetyMonitor
	{
	public:
		void Reset();
		void OnGameFrame();
		void OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands);
		NetworkSafetyEvidence Evaluate(MovementPlayer *player);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		static void Prune(NetworkSafetyPlayerData &data, Clock::time_point now);

		std::array<NetworkSafetyPlayerData, MAXPLAYERS + 1> playerData;
	};

	struct TrackedPosition
	{
		Vector origin;
		Vector eyePosition;
		int team {};
		bool valid {};
		bool alive {};
		bool teleported {};
	};

	struct PositionFrame
	{
		int serverTick {-1};
		std::array<TrackedPosition, MAXPLAYERS + 1> players;
	};

	struct ShotCommand
	{
		int commandNumber {};
		int clientTick {};
		int serverTick {-1};
		QAngle baseAngles;
		QAngle attackAngles;
		Vector eyePosition;
		float subtickPitchTravel {};
		float subtickYawTravel {};
		QAngle previousBaseAngles;
		bool airborne {};
		bool scoped {};
		bool simulated {};
		bool consumed {};
		bool subtickAnglesValid {true};
		bool previousBaseValid {};
	};

	struct ShotRecord
	{
		std::uint64_t id {};
		std::uint32_t generation {};
		int playerIndex {};
		int commandNumber {};
		int clientTick {};
		int serverTick {-1};
		int fireTick {-1};
		QAngle angles;
		QAngle baseAngles;
		QAngle silentPreviousBaseAngles;
		Vector eyePosition;
		std::string weapon;
		Clock::time_point fireTime;
		float silentDeviation {};
		float silentAllowance {};
		float silentInaccuracy {};
		float silentSpread {};
		float silentSubtickPitchTravel {};
		float silentSubtickYawTravel {};
		float silentUnsupportedDeviation {};
		float silentAdjacentPathExcess {};
		std::uint32_t silentWeaponId {};
		int victimIndex {-1};
		bool airborne {};
		bool scoped {};
		bool hurtSeen {};
		bool deathSeen {};
		bool headshot {};
		bool wallbang {};
		bool throughSmoke {};
		bool silentFireSeen {};
		bool silentHitSeen {};
		bool silentRejected {};
		bool silentMovementSupported {};
		bool silentSubtickAnglesValid {true};
		bool silentPreviousBaseValid {};
		bool aimbotConsumed {};
		bool silentMeasured {};
		bool silentConsumed {};
		bool irregularConsumed {};
		bool accuracyConsumed {};
	};

	struct ShotPlayerData
	{
		std::deque<ShotCommand> commands;
		std::deque<ShotRecord> shots;
		std::uint32_t generation {1};
		int lastViewCommandNumber {-1};
		QAngle lastViewBaseAngles;
		bool lastViewValid {};
	};

	class ShotCorrelator
	{
	public:
		void Reset();
		void OnClientDisconnect(MovementPlayer *player);
		void OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands);
		void OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick);
		void CaptureFrame(int currentTick);
		void Prune(int currentTick);

		ShotRecord *OnWeaponFire(IGameEvent *event, MovementPlayer *player, int currentTick);
		ShotRecord *OnFireBullets(const CMsgTEFireBullets &event, int currentTick);
		ShotRecord *OnPlayerBulletHit(const CMsgPlayerBulletHit &event, int currentTick);
		ShotRecord *OnPlayerHurt(IGameEvent *event, MovementPlayer *victim, int currentTick);
		ShotRecord *OnPlayerDeath(IGameEvent *event, MovementPlayer *victim, int currentTick);

		const PositionFrame *FindFrame(int serverTick) const;
		const TrackedPosition *FindPosition(int serverTick, int playerIndex) const;
		const ShotRecord *FindShot(int playerIndex, std::uint64_t shotId) const;
		std::deque<ShotRecord> &GetShots(int playerIndex);

	private:
		ShotRecord *MatchEvent(MovementPlayer *player, std::string_view weapon, int currentTick, int victimIndex = -1);
		static void AdvanceGeneration(ShotPlayerData &data);

		std::array<ShotPlayerData, MAXPLAYERS + 1> playerData;
		std::deque<PositionFrame> positionFrames;
		std::uint64_t nextShotId {1};
	};

	struct DoubletapState
	{
		int serverTick {-1};
		int incidents {};
		int firstIncidentTicks {-1};
		Clock::time_point firstIncidentTime;
		std::string weapon;
		NetworkSafetyEvidence networkEvidence;
	};

	// Detects two weapon-fire events arriving in the same or next server tick.
	class DoubletapModule
	{
	public:
		void Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto, NetworkSafetyMonitor *networkSafety);
		void Unload();
		void Reset();
		void OnWeaponFire(IGameEvent *event, MovementPlayer *player, int currentTick);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		AnnounceCallback announce {};
		AnnounceCallback announceNetworkVeto {};
		NetworkSafetyMonitor *networkSafety {};
		std::array<DoubletapState, MAXPLAYERS + 1> playerData;
	};

	enum class TriggerContactMode : std::uint8_t
	{
		AimDriven,
		TargetDriven,
	};

	struct TriggerbotPending
	{
		int commandNumber {};
		int targetIndex {-1};
		int reactionTicks {};
		TriggerContactMode mode {};
	};

	struct TriggerbotIncident
	{
		Clock::time_point time;
		int reactionTicks {};
		TriggerContactMode mode {};
	};

	struct TriggerbotPlayerData
	{
		std::array<int, MAXPLAYERS + 1> lastContactTick;
		std::deque<TriggerbotPending> pending;
		std::deque<TriggerbotIncident> history;
		QAngle lastAngles;
		int lastCommandNumber {-1};
		int activeTarget {-1};
		int contactStartCommand {-1};
		TriggerContactMode activeMode {};
		bool lastAnglesValid {};
	};

	struct TriggerbotSmoke
	{
		Vector center;
		Clock::time_point expires;
	};

	// Detects repeated damaging shots fired within three ticks of fresh, unobstructed enemy hitbox contact.
	class TriggerbotModule
	{
	public:
		void Load(AnnounceCallback announce, NetworkSafetyMonitor *networkSafety);
		void Unload();
		void Reset();
		void OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick);
		void OnPlayerHurt(IGameEvent *event, MovementPlayer *attacker, MovementPlayer *victim, const ShotRecord &shot);
		void OnGameEvent(IGameEvent *event);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		AnnounceCallback announce {};
		NetworkSafetyMonitor *networkSafety {};
		std::array<TriggerbotPlayerData, MAXPLAYERS + 1> playerData;
		std::deque<TriggerbotSmoke> smokes;
		Clock::time_point smokeStateUnknownUntil;
	};

	enum class SilentEvidenceContext : std::uint8_t
	{
		Grounded,
		Airborne,
		Blatant,
	};

	struct SilentIncident
	{
		Clock::time_point time;
		int points {};
		int originalPoints {};
		float unexplainedDegrees {};
		SilentEvidenceContext context {};
	};

	// Detects damaging shots whose attack-history angle disagrees with the command's base view angle.
	class SilentAimModule
	{
	public:
		void Load(AnnounceCallback announce, ShotCorrelator *shots);
		void Unload();
		void Reset();
		void OnShotUpdated(MovementPlayer *player, ShotRecord &shot);
		void OnGameFrame(int currentTick);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		void Finalize(MovementPlayer *player, ShotRecord &shot);

		AnnounceCallback announce {};
		ShotCorrelator *shots {};
		std::array<std::deque<SilentIncident>, MAXPLAYERS + 1> evidence;
	};

	struct AimCommand
	{
		int commandNumber {};
		int clientTick {};
		int serverTick {-1};
		QAngle angles;
		Vector eyePosition;
		bool simulated {};
	};

	struct AimbotPlayerData
	{
		std::deque<AimCommand> commands;
		int pendingShot {-1};
		std::uint64_t pendingShotId {};
		int victimIndex {-1};
		int lastCountedIncidentCommand {};
		bool pending {};
		bool hasCountedIncident {};
	};

	enum class AimbotEvidenceType : std::uint8_t
	{
		Convergence,
		SnapReturn,
		SmoothConvergence,
	};

	struct AimbotIncident
	{
		Clock::time_point time;
		AimbotEvidenceType type {};
		int points {};
		float movement {};
		float before {};
		float after {};
		bool airborne {};
		bool wallbang {};
		bool throughSmoke {};
		bool headshot {};
		bool noscope {};
	};

	struct PendingAimbotIncident
	{
		std::uint64_t shotId {};
		int fireTick {-1};
		AimbotIncident incident;
	};

	// Detects damaging command-angle snaps that rapidly converge on an enemy.
	class AimbotModule
	{
	public:
		void Load(AnnounceCallback announce, ShotCorrelator *shots);
		void Unload();
		void Reset();
		void OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands);
		void OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick);
		void OnGameFrame(int currentTick);
		void OnPlayerHurt(MovementPlayer *attacker, MovementPlayer *victim, ShotRecord &shot);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		bool Evaluate(MovementPlayer *attacker, AimbotPlayerData &data, int currentTick);
		void FinalizePending(MovementPlayer *attacker, int currentTick);
		void AddIncident(MovementPlayer *attacker, const PendingAimbotIncident &pending);

		AnnounceCallback announce {};
		ShotCorrelator *shots {};
		std::array<AimbotPlayerData, MAXPLAYERS + 1> playerData;
		std::array<std::deque<PendingAimbotIncident>, MAXPLAYERS + 1> pendingEvidence;
		std::array<std::deque<AimbotIncident>, MAXPLAYERS + 1> evidence;
	};

	struct AimlockSample
	{
		int serverTick {-1};
		QAngle angles;
		Vector eyePosition;
		bool valid {};
	};

	struct AimlockLagHypothesis
	{
		QAngle startBearing;
		float maximumTargetDisplacement {};
		float requiredTargetDisplacement {};
		int lagTicks {};
		int onTargetSamples {};
		bool valid {};
	};

	struct AimlockTrack
	{
		std::array<AimlockLagHypothesis, 5> hypotheses;
		int targetIndex {-1};
		int bodyPoint {-1};
		int startServerTick {-1};
		int lastServerTick {-1};
		int hypothesisCount {};
		int samples {};
	};

	struct AimlockPlayerData
	{
		AimlockSample pending;
		AimlockTrack track;
		int latchedTarget {-1};
		int latchedBodyPoint {-1};
		int breakStartTick {-1};
		int lastProcessedTick {-1};
		bool latched {};
	};

	// Detects sustained, unnaturally precise target tracking, including through walls.
	class AimlockModule
	{
	public:
		void Load(AnnounceCallback announce, ShotCorrelator *shots);
		void Unload();
		void Reset();
		void OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick);
		void OnGameFrame(int currentTick);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		void Evaluate(MovementPlayer *player, AimlockPlayerData &data, const AimlockSample &sample);
		void AddIncident(MovementPlayer *player, AimlockPlayerData &data, const AimlockLagHypothesis &hypothesis);

		AnnounceCallback announce {};
		ShotCorrelator *shots {};
		std::array<AimlockPlayerData, MAXPLAYERS + 1> playerData;
		std::array<std::deque<Clock::time_point>, MAXPLAYERS + 1> evidence;
	};

	// Detects blacklisted client event subscriptions commonly created by injected code.
	class DllInjectionModule
	{
	public:
		void Load(AnnounceCallback announce);
		void Unload();
		void Reset();
		void OnGameFrame();
		void OnClientDisconnect(MovementPlayer *player);

	private:
		AnnounceCallback announce {};
		std::array<Clock::time_point, MAXPLAYERS> nextScans;
	};

	struct AntiAimCommand
	{
		int commandNumber {};
		int clientTick {};
		int serverTick {-1};
		QAngle baseAngles;
		QAngle shotAngles;
		float historyYawDifference {};
		float subtickPitch {};
		float subtickYaw {};
		int mouseX {};
		int mouseY {};
		std::uint32_t problems {};
		bool attack {};
		bool hasHistoryAngles {};
		bool hasShotAngles {};
		bool simulated {};
	};

	enum class AntiAimEvidenceType : std::uint8_t
	{
		Spin,
		Jitter,
		AttackReturn,
		InconsistentCommand,
		HistoryMismatch,
		InvalidAngles,
		Count,
	};

	struct AntiAimPlayerData
	{
		std::deque<AntiAimCommand> commands;
		Clock::time_point scoreTime;
		float score {};
		float mismatchScore {};
		std::array<float, 3> spinSeconds;
		std::array<float, 3> spinBreakSeconds;
		float jitterSeconds {};
		float jitterBreakSeconds {};
		std::array<int, static_cast<size_t>(AntiAimEvidenceType::Count)> evidenceCounts;
		std::array<float, static_cast<size_t>(AntiAimEvidenceType::Count)> evidencePoints;
		int pendingShot {-1};
		int pendingShotTick {-1};
		int spinDebugBucket {-1};
		int jitterDebugBucket {-1};
		int lastMotionServerTick {-1};
		int lastMismatchEvidenceCommand {-1};
		bool invalidActive {};
		bool inconsistencyActive {};
		bool spinActive {};
		bool jitterActive {};
		bool suppressContinuous {};
		bool teleportGrace {};
	};

	// Detects impossible angles, command inconsistencies, jitter, attack-return, and sustained spin patterns.
	class AntiAimModule
	{
	public:
		void Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto, NetworkSafetyMonitor *networkSafety);
		void Unload();
		void Reset();
		void OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands);
		void OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick);
		void OnGameFrame(int currentTick);
		void OnWeaponFire(MovementPlayer *player, const ShotRecord &shot);
		void OnGameEvent(IGameEvent *event, MovementPlayer *player);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		void AddEvidence(MovementPlayer *player, AntiAimPlayerData &data, AntiAimEvidenceType type, float weight, const char *reasonKey,
						 const char *reason, bool continuous, bool mismatch = false);
		void ApplyDecay(MovementPlayer *player, AntiAimPlayerData &data);
		void EvaluateMotion(MovementPlayer *player, AntiAimPlayerData &data, AntiAimCommand &command);
		void EvaluatePendingShot(MovementPlayer *player, AntiAimPlayerData &data, int currentTick);
		void ResetMotion(AntiAimPlayerData &data);

		AnnounceCallback announce {};
		AnnounceCallback announceNetworkVeto {};
		NetworkSafetyMonitor *networkSafety {};
		std::array<AntiAimPlayerData, MAXPLAYERS + 1> playerData;
	};

	struct IrregularPending
	{
		std::uint64_t shotId {};
		Clock::time_point time;
		int fireTick {-1};
		int points {};
		bool airborne {};
		bool noScope {};
		bool success {};
	};

	struct IrregularBucket
	{
		std::int64_t second {};
		int attempts {};
		int successes {};
		int score {};
	};

	struct IrregularPlayerData
	{
		std::deque<IrregularPending> pending;
		std::deque<IrregularBucket> evidence;
	};

	// Detects repeated success with unusually difficult airborne or unscoped shots.
	class IrregularBehaviorModule
	{
	public:
		void Load(AnnounceCallback announce);
		void Unload();
		void Reset();
		void OnWeaponFire(MovementPlayer *player, const ShotRecord &shot);
		void OnPlayerDeath(IGameEvent *event, MovementPlayer *attacker, MovementPlayer *victim, ShotRecord &shot);
		void OnGameFrame(int currentTick);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		void Finalize(MovementPlayer *player, IrregularPlayerData &data, const IrregularPending &attempt);
		void Prune(IrregularPlayerData &data, std::int64_t nowSecond);
		void Evaluate(MovementPlayer *player, IrregularPlayerData &data);

		AnnounceCallback announce {};
		std::array<IrregularPlayerData, MAXPLAYERS + 1> playerData;
	};

	struct AccuracyBucket
	{
		std::int64_t second {};
		int attempts {};
		int hits {};
	};

	struct AccuracyPlayerData
	{
		std::deque<AccuracyBucket> evidence;
	};

	// Detects sustained near-perfect accuracy across a meaningful number of aimed shots.
	class InhumanAccuracyModule
	{
	public:
		void Load(AnnounceCallback announce, ShotCorrelator *shotCorrelator);
		void Unload();
		void Reset();
		void OnGameFrame(int currentTick);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		void Finalize(MovementPlayer *player, ShotRecord &shot);
		void Prune(AccuracyPlayerData &data, std::int64_t nowSecond);

		AnnounceCallback announce {};
		ShotCorrelator *shots {};
		std::array<AccuracyPlayerData, MAXPLAYERS + 1> playerData;
	};

	struct NameChangerPlayerData
	{
		std::string lastName;
		std::deque<Clock::time_point> changes;
		bool initialized {};
	};

	// Detects clients that change their visible name five times within one rolling minute.
	class NameChangerModule
	{
	public:
		void Load(AnnounceCallback announce);
		void Unload();
		void Reset();
		void OnClientReady(MovementPlayer *player);
		void OnClientSettingsChanged(MovementPlayer *player);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		AnnounceCallback announce {};
		std::array<NameChangerPlayerData, MAXPLAYERS + 1> playerData;
	};

	class DetectionSystem
	{
	public:
		void Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto);
		void Unload();
		void Reset();
		void OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands);
		void OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick);
		void OnGameFrame(int currentTick);
		void OnGameEvent(IGameEvent *event, MovementPlayer *player, int currentTick);
		void OnFireBullets(const CMsgTEFireBullets &event, int currentTick);
		void OnPlayerBulletHit(const CMsgPlayerBulletHit &event, int currentTick);
		void OnClientReady(MovementPlayer *player);
		void OnClientSettingsChanged(MovementPlayer *player);
		void OnClientDisconnect(MovementPlayer *player);

	private:
		void RefreshSettings();

		NetworkSafetyMonitor networkSafety;
		ShotCorrelator shots;
		DoubletapModule doubletap;
		TriggerbotModule triggerbot;
		SilentAimModule silentAim;
		AimbotModule aimbot;
		AimlockModule aimlock;
		DllInjectionModule dllInjection;
		AntiAimModule antiAim;
		IrregularBehaviorModule irregularBehavior;
		InhumanAccuracyModule inhumanAccuracy;
		NameChangerModule nameChanger;
		std::uint64_t settingsMask {};
		std::uint64_t settingsRevision {};
		bool teammatesAreEnemies {};
	};
} // namespace detection
