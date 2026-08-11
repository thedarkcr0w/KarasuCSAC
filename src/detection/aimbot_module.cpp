#include "detection/detection_system.h"

#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/usercmd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numeric>
#include <vector>

CConVar<bool> cs2ac_aimbot_debug("cs2ac_aimbot_debug", FCVAR_NONE, "Show why Aimbot accepts or rejects each damaging shot", false);

#define AIMBOT_DEBUG(...) \
	do \
	{ \
		if (cs2ac_aimbot_debug.GetBool()) \
			Msg("[CS2AC Aimbot] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr size_t commandHistorySize = 128;
	constexpr int snapWindowTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE * 0.5f);
	constexpr float minimumDistance = 100.0f;
	constexpr int baseIncidentPoints = 2;
	constexpr int humanizedBaseIncidentPoints = 1;
	constexpr int detectionThreshold = 6;
	constexpr int contextWaitTicks = 2;
	constexpr auto evidenceWindow = std::chrono::minutes(5);
	// A confirmed smoothed-aimbot demo produced 12 damaging curves inside this envelope: 13.66-29.69 degrees of travel,
	// 94.9-99.5% error removal, 0.12-0.85 degrees of final error, 94.6-100% path efficiency, and 3-5 shrinking steps.
	constexpr float smoothMinimumMovement = 13.0f;
	constexpr float smoothMinimumImprovement = 0.94f;
	constexpr float smoothMaximumFinalError = 1.0f;
	constexpr float smoothMinimumPathEfficiency = 0.94f;
	constexpr float smoothMinimumStep = 0.15f;
	constexpr float smoothRelativeStep = 0.10f;
	constexpr int smoothMinimumSteps = 3;
	// A confirmed low-FOV aimbot repeatedly moved 10-40% of the remaining target error per update. Across six-update
	// windows, 10/20 damaging shots followed that proportional controller with at least 70% fit and 80% alignment.
	constexpr int humanizedSteps = 6;
	constexpr int humanizedLagSearchRadius = 2;
	constexpr float humanizedMinimumGain = 0.10f;
	constexpr float humanizedMaximumGain = 0.40f;
	constexpr float humanizedMinimumMovement = 0.70f;
	constexpr float humanizedMinimumFit = 0.70f;
	constexpr float humanizedMinimumAlignment = 0.80f;
	constexpr int humanizedMinimumAlignedSteps = 5;
	constexpr float humanizedGainTolerance = 0.08f;
	constexpr float maximumInterpolationTicks = 19.0f;
	constexpr float bodyHeights[] = {8.0f, 46.0f, 64.0f};

	constexpr bool MeetsSmoothThresholds(float movement, float improvement, float finalError, float efficiency, int steps, bool shrinking)
	{
		return movement >= smoothMinimumMovement && improvement >= smoothMinimumImprovement && finalError <= smoothMaximumFinalError
			   && efficiency >= smoothMinimumPathEfficiency && steps >= smoothMinimumSteps && shrinking;
	}

	static_assert(MeetsSmoothThresholds(13.66f, 0.949f, 0.85f, 0.946f, 3, true));
	static_assert(!MeetsSmoothThresholds(6.31f, 0.934f, 0.41f, 1.0f, 3, true));

	constexpr bool MeetsHumanizedThresholds(float gain, float movement, float fit, float alignment, int alignedSteps)
	{
		return gain >= humanizedMinimumGain && gain <= humanizedMaximumGain && movement >= humanizedMinimumMovement
			   && fit >= humanizedMinimumFit && alignment >= humanizedMinimumAlignment && alignedSteps >= humanizedMinimumAlignedSteps;
	}

	static_assert(MeetsHumanizedThresholds(0.218f, 1.204f, 0.969f, 0.984f, 6));
	static_assert(!MeetsHumanizedThresholds(0.218f, 0.50f, 0.969f, 0.984f, 6));
	static_assert(!MeetsHumanizedThresholds(0.50f, 1.204f, 0.969f, 0.984f, 6));

	constexpr int AimbotPoints(int basePoints, bool airborne, bool wallbang, bool throughSmoke, bool headshot, bool noscope)
	{
		return basePoints + static_cast<int>(airborne) + static_cast<int>(wallbang) + static_cast<int>(throughSmoke)
			   + static_cast<int>(headshot) + static_cast<int>(noscope);
	}

	static_assert(AimbotPoints(baseIncidentPoints, false, false, false, false, false) * 3 == detectionThreshold);
	static_assert(AimbotPoints(baseIncidentPoints, false, false, false, false, false) * 2 < detectionThreshold);
	static_assert(AimbotPoints(humanizedBaseIncidentPoints, false, false, false, true, false) * 3 == detectionThreshold);
	static_assert(AimbotPoints(baseIncidentPoints, true, true, true, true, false) == detectionThreshold);
	static_assert(AimbotPoints(baseIncidentPoints, true, true, true, true, true) == 7);

	bool IsScopedRifle(std::string_view weapon)
	{
		weapon = detection::NormalizeWeapon(weapon);
		return weapon == "awp" || weapon == "ssg08" || weapon == "g3sg1" || weapon == "scar20";
	}

	enum class AimbotRule
	{
		None,
		Convergence,
		SnapReturn,
		SmoothConvergence,
		Humanized,
	};

	struct HumanizedMeasurement
	{
		float gain {};
		float movement {};
		float fit {};
		float alignment {};
		int alignedSteps {};
		int lagTicks {};
		bool valid {};
	};

	float AimError(const Vector &eye, const QAngle &angles, const Vector &target)
	{
		Vector direction = target - eye;
		if (!detection::IsFinite(direction) || direction.LengthSqr() < EPSILON)
		{
			return 0.0f;
		}
		direction.NormalizeInPlace();
		const float dot = std::clamp(DotProduct(detection::AimForward(angles), direction), -1.0f, 1.0f);
		return static_cast<float>(std::acos(dot) * (180.0 / M_PI));
	}

	float NearestBodyAimError(const Vector &eye, const QAngle &angles, const Vector &feet)
	{
		float best = 180.0f;
		for (float height : bodyHeights)
		{
			best = (std::min)(best, AimError(eye, angles, feet + Vector(0.0f, 0.0f, height)));
		}
		return best;
	}

	QAngle Bearing(const Vector &eye, const Vector &target)
	{
		const Vector delta = target - eye;
		constexpr float radiansToDegrees = static_cast<float>(180.0 / M_PI);
		return {-std::atan2(delta.z, std::sqrt(delta.x * delta.x + delta.y * delta.y)) * radiansToDegrees,
				std::atan2(delta.y, delta.x) * radiansToDegrees, 0.0f};
	}

	int EstimateVisualLagTicks(MovementPlayer *player)
	{
		if (!player || !interfaces::pEngine)
		{
			return -1;
		}

		INetChannelInfo *netChannel = interfaces::pEngine->GetPlayerNetInfo(player->GetPlayerSlot());
		const char *interpolationValue = interfaces::pEngine->GetClientConVarValue(player->GetPlayerSlot(), "cl_interp_ratio");
		if (!netChannel || !utils::IsNumeric(interpolationValue))
		{
			return -1;
		}

		const float roundTripSeconds = netChannel->GetEngineLatency();
		float interpolationTicks = static_cast<float>(std::strtod(interpolationValue, nullptr));
		if (!std::isfinite(roundTripSeconds) || roundTripSeconds < 0.0f || roundTripSeconds > 2.0f || !std::isfinite(interpolationTicks)
			|| interpolationTicks < 0.0f || interpolationTicks > maximumInterpolationTicks)
		{
			return -1;
		}
		if (interpolationTicks == 0.0f)
		{
			interpolationTicks = 1.0f;
		}
		// The target snapshot travelled to the client before this aim command travelled back to the server, so use the full RTT.
		return (std::max)(0, static_cast<int>(std::lround(roundTripSeconds * ENGINE_FIXED_TICK_RATE + interpolationTicks)));
	}

	HumanizedMeasurement MeasureHumanized(const detection::ShotCorrelator *shots, MovementPlayer *attacker, int victimIndex, int bodyPoint,
										 const std::array<detection::AimCommand *, humanizedSteps + 1> &commands, int lagTicks)
	{
		HumanizedMeasurement result;
		if (!shots || !attacker || victimIndex < 1 || victimIndex > MAXPLAYERS || bodyPoint < 0
			|| bodyPoint >= static_cast<int>(std::size(bodyHeights)))
		{
			return result;
		}

		double desiredNorm = 0.0;
		double actualNorm = 0.0;
		double cross = 0.0;
		double movement = 0.0;
		std::array<std::array<double, 2>, humanizedSteps> desired {};
		std::array<std::array<double, 2>, humanizedSteps> actual {};
		for (int index = 1; index <= humanizedSteps; ++index)
		{
			const auto *previous = commands[index - 1];
			const auto *current = commands[index];
			const auto *observer = current ? shots->FindPosition(current->serverTick, attacker->index) : nullptr;
			const auto *target = current ? shots->FindPosition(current->serverTick - lagTicks, victimIndex) : nullptr;
			if (!previous || !current || !observer || !target || !observer->alive || !target->alive || observer->teleported || target->teleported
				|| !detection::AreOpponents(observer->team, target->team))
			{
				return result;
			}

			const QAngle targetAngles = Bearing(observer->eyePosition, target->origin + Vector(0.0f, 0.0f, bodyHeights[bodyPoint]));
			if (!detection::IsFinite(targetAngles))
			{
				return result;
			}

			desired[index - 1] = {static_cast<double>(targetAngles.x - previous->angles.x),
								  static_cast<double>(std::remainder(targetAngles.y - previous->angles.y, 360.0f))};
			actual[index - 1] = {static_cast<double>(current->angles.x - previous->angles.x),
								 static_cast<double>(std::remainder(current->angles.y - previous->angles.y, 360.0f))};
			const double dot = desired[index - 1][0] * actual[index - 1][0] + desired[index - 1][1] * actual[index - 1][1];
			desiredNorm += desired[index - 1][0] * desired[index - 1][0] + desired[index - 1][1] * desired[index - 1][1];
			actualNorm += actual[index - 1][0] * actual[index - 1][0] + actual[index - 1][1] * actual[index - 1][1];
			cross += dot;
			movement += std::hypot(actual[index - 1][0], actual[index - 1][1]);
			result.alignedSteps += dot > 0.0;
		}

		if (desiredNorm <= EPSILON || actualNorm <= EPSILON)
		{
			return result;
		}
		const double gain = cross / desiredNorm;
		double residual = 0.0;
		for (int index = 0; index < humanizedSteps; ++index)
		{
			const double pitch = actual[index][0] - gain * desired[index][0];
			const double yaw = actual[index][1] - gain * desired[index][1];
			residual += pitch * pitch + yaw * yaw;
		}

		result.gain = static_cast<float>(gain);
		result.movement = static_cast<float>(movement);
		result.fit = static_cast<float>(1.0 - residual / actualNorm);
		result.alignment = static_cast<float>(cross / std::sqrt(desiredNorm * actualNorm));
		result.lagTicks = lagTicks;
		result.valid = std::isfinite(result.gain) && std::isfinite(result.movement) && std::isfinite(result.fit)
					   && std::isfinite(result.alignment);
		return result;
	}
} // namespace

namespace detection
{
	void AimbotModule::Load(AnnounceCallback announceCallback, ShotCorrelator *shotCorrelator)
	{
		announce = announceCallback;
		shots = shotCorrelator;
	}

	void AimbotModule::Unload()
	{
		Reset();
		shots = nullptr;
		announce = nullptr;
	}

	void AimbotModule::Reset()
	{
		playerData = {};
		pendingEvidence = {};
		evidence = {};
	}

	void AimbotModule::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
	{
		if (!IsEligibleHuman(player) || !commands || numCommands <= 0)
		{
			return;
		}

		auto &data = playerData[player->index];
		for (int i = 0; i < numCommands; ++i)
		{
			PlayerCommand &command = commands[i];
			if (!command.has_base() || !command.base().has_viewangles())
			{
				continue;
			}
			if (std::any_of(data.commands.rbegin(), data.commands.rend(),
							[&](const AimCommand &stored) { return stored.commandNumber == command.cmdNum; }))
			{
				continue;
			}

			const auto &base = command.base();
			QAngle angles(base.viewangles().x(), base.viewangles().y(), base.viewangles().z());
			const int attackIndex = command.attack1_start_history_index();
			if (attackIndex >= 0 && attackIndex < command.input_history_size() && command.input_history(attackIndex).has_view_angles())
			{
				const auto &view = command.input_history(attackIndex).view_angles();
				const QAngle firingAngles(view.x(), view.y(), view.z());
				if (IsFinite(firingAngles))
				{
					angles = firingAngles;
				}
			}
			if (!IsFinite(angles))
			{
				continue;
			}

			data.commands.push_back({command.cmdNum, base.client_tick(), -1, angles});
			while (data.commands.size() > commandHistorySize)
			{
				data.commands.pop_front();
			}
		}
	}

	void AimbotModule::OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick)
	{
		if (!IsEligibleHuman(player) || !command || !player->GetPlayerPawn())
		{
			return;
		}
		auto &data = playerData[player->index];
		auto found = std::find_if(data.commands.rbegin(), data.commands.rend(),
								  [&](const AimCommand &stored) { return stored.commandNumber == command->cmdNum; });
		if (found == data.commands.rend())
		{
			return;
		}
		Vector eye;
		player->GetEyeOrigin(&eye);
		if (!IsFinite(eye))
		{
			return;
		}
		found->serverTick = currentTick;
		found->eyePosition = eye;
		found->simulated = true;
		if (data.pending)
		{
			Evaluate(player, data, currentTick);
		}
	}

	void AimbotModule::OnGameFrame(int currentTick)
	{
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			if (!playerData[index].pending && pendingEvidence[index].empty())
			{
				continue;
			}
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			if (!IsEligibleHuman(player))
			{
				playerData[index] = {};
				pendingEvidence[index].clear();
				continue;
			}
			if (playerData[index].pending)
			{
				Evaluate(player, playerData[index], currentTick);
			}
			FinalizePending(player, currentTick);
		}
	}

	void AimbotModule::OnPlayerHurt(MovementPlayer *attacker, MovementPlayer *victim, ShotRecord &shot)
	{
		if (!IsEligibleHuman(attacker) || !victim || attacker == victim || shot.playerIndex != attacker->index || shot.aimbotConsumed)
		{
			return;
		}
		shot.aimbotConsumed = true;
		auto &data = playerData[attacker->index];
		if (data.pending)
		{
			Evaluate(attacker, data, shot.fireTick);
			if (data.pending)
			{
				data.pending = false;
			}
		}
		data.pendingShot = shot.commandNumber;
		data.pendingShotId = shot.id;
		data.victimIndex = victim->index;
		data.pending = true;
		AIMBOT_DEBUG("%s matched damaging shot command %d at server tick %d.\n", attacker->GetName(), shot.commandNumber, shot.serverTick);
		Evaluate(attacker, data, shot.fireTick);
	}

	bool AimbotModule::Evaluate(MovementPlayer *attacker, AimbotPlayerData &data, int currentTick)
	{
		if (!data.pending || !shots)
		{
			return true;
		}
		auto clearPending = [&]()
		{
			data.pendingShot = -1;
			data.pendingShotId = 0;
			data.victimIndex = -1;
			data.pending = false;
		};

		auto shot = std::find_if(data.commands.begin(), data.commands.end(),
								 [&](const AimCommand &command) { return command.commandNumber == data.pendingShot; });
		if (shot == data.commands.end() || !shot->simulated || data.victimIndex < 1 || data.victimIndex > MAXPLAYERS)
		{
			clearPending();
			return true;
		}

		const TrackedPosition *shotTarget = shots->FindPosition(shot->serverTick, data.victimIndex);
		const TrackedPosition *shotAttacker = shots->FindPosition(shot->serverTick, attacker->index);
		if (!shotTarget || !shotAttacker)
		{
			if (currentTick <= shot->serverTick)
			{
				return false;
			}
			AIMBOT_DEBUG("%s rejected because target history is missing for server tick %d.\n", attacker->GetName(), shot->serverTick);
			clearPending();
			return true;
		}
		if (shotTarget->teleported || shotAttacker->teleported)
		{
			AIMBOT_DEBUG("%s rejected because a player teleported inside the snap window.\n", attacker->GetName());
			clearPending();
			return true;
		}
		if (!AreOpponents(shotAttacker->team, shotTarget->team))
		{
			AIMBOT_DEBUG("%s rejected because the damaging shot was not against an enemy.\n", attacker->GetName());
			clearPending();
			return true;
		}
		const float distance = (shotTarget->origin - shotAttacker->eyePosition).Length();
		if (!std::isfinite(distance) || distance < minimumDistance)
		{
			AIMBOT_DEBUG("%s rejected because target distance %.1f is below %.0f.\n", attacker->GetName(), distance, minimumDistance);
			clearPending();
			return true;
		}

		bool suspicious = false;
		float largestSnap = 0.0f;
		float bestBefore = 0.0f;
		float bestAfter = 0.0f;
		float largestMeasuredSnap = -1.0f;
		float measuredBefore = 0.0f;
		float measuredAfter = 0.0f;
		int measuredMovements = 0;
		const char *measurementFailure = "no continuous adjacent command history was available";
		bool reusedSnap = false;
		bool humanizedSeeded = false;
		AimbotRule matchedRule = AimbotRule::None;
		float humanizedGain = 0.0f;
		float humanizedFit = 0.0f;

		struct SmoothPoint
		{
			AimCommand *command;
			float error;
		};

		int smoothBodyPoint = 0;
		float smoothShotError = 180.0f;
		for (int bodyPoint = 0; bodyPoint < static_cast<int>(std::size(bodyHeights)); ++bodyPoint)
		{
			const float error = AimError(shotAttacker->eyePosition, shot->angles, shotTarget->origin + Vector(0.0f, 0.0f, bodyHeights[bodyPoint]));
			if (error < smoothShotError)
			{
				smoothShotError = error;
				smoothBodyPoint = bodyPoint;
			}
		}
		std::vector<SmoothPoint> smoothPoints {{&*shot, smoothShotError}};
		float smoothMovement = 0.0f;
		float smoothBefore = 0.0f;
		float smoothAfter = 0.0f;
		float smoothEfficiency = 0.0f;
		int smoothSteps = 0;
		auto findCommand = [&](int commandNumber) -> AimCommand *
		{
			auto found = std::find_if(data.commands.begin(), data.commands.end(),
									  [&](AimCommand &command) { return command.commandNumber == commandNumber && command.simulated; });
			return found == data.commands.end() ? nullptr : &*found;
		};
		AimCommand *newer = &*shot;
		while (newer->commandNumber > (std::numeric_limits<int>::min)())
		{
			AimCommand *older = findCommand(newer->commandNumber - 1);
			if (!older)
			{
				measurementFailure = "the previous simulated command was missing";
				break;
			}
			const std::int64_t serverGap = static_cast<std::int64_t>(newer->serverTick) - older->serverTick;
			if (static_cast<std::int64_t>(newer->clientTick) - older->clientTick != 1 || serverGap < 0 || serverGap > 1)
			{
				measurementFailure = "the command, client-tick, or server-tick history was discontinuous";
				break;
			}
			if (static_cast<std::int64_t>(shot->serverTick) - older->serverTick > snapWindowTicks)
			{
				measurementFailure = "the preceding command was outside the 0.5-second snap window";
				break;
			}

			const TrackedPosition *olderTarget = shots->FindPosition(older->serverTick, data.victimIndex);
			const TrackedPosition *newerTarget = shots->FindPosition(newer->serverTick, data.victimIndex);
			const TrackedPosition *olderAttacker = shots->FindPosition(older->serverTick, attacker->index);
			const TrackedPosition *newerAttacker = shots->FindPosition(newer->serverTick, attacker->index);
			if (!olderTarget || !newerTarget || !olderAttacker || !newerAttacker || olderTarget->teleported || newerTarget->teleported
				|| olderAttacker->teleported || newerAttacker->teleported)
			{
				measurementFailure = "matching historical player positions were missing or interrupted by a teleport";
				break;
			}
			const float snap = AngularDistance(older->angles, newer->angles);
			const float before = NearestBodyAimError(olderAttacker->eyePosition, older->angles, olderTarget->origin);
			const float after = NearestBodyAimError(newerAttacker->eyePosition, newer->angles, newerTarget->origin);
			if (!std::isfinite(snap) || !std::isfinite(before) || !std::isfinite(after))
			{
				measurementFailure = "an angle or target-error measurement was invalid";
				break;
			}
			const float smoothError =
				AimError(olderAttacker->eyePosition, older->angles, olderTarget->origin + Vector(0.0f, 0.0f, bodyHeights[smoothBodyPoint]));
			if (!std::isfinite(smoothError))
			{
				measurementFailure = "a fixed-body target-error measurement was invalid";
				break;
			}
			smoothPoints.push_back({older, smoothError});
			++measuredMovements;
			if (snap > largestMeasuredSnap)
			{
				largestMeasuredSnap = snap;
				measuredBefore = before;
				measuredAfter = after;
			}
			const bool converged = (snap > 10.0f && after < before * 0.2f) || (snap > 5.0f && after < before * 0.1f);
			const bool fresh = !data.hasCountedIncident || newer->commandNumber > data.lastCountedIncidentCommand;
			if (converged && fresh)
			{
				suspicious = true;
				matchedRule = AimbotRule::Convergence;
				if (snap > largestSnap)
				{
					largestSnap = snap;
					bestBefore = before;
					bestAfter = after;
				}
			}
			else if (converged)
			{
				reusedSnap = true;
			}
			newer = older;
		}

		AimCommand *previous = data.pendingShot > (std::numeric_limits<int>::min)() ? findCommand(data.pendingShot - 1) : nullptr;
		AimCommand *next = data.pendingShot < (std::numeric_limits<int>::max)() ? findCommand(data.pendingShot + 1) : nullptr;
		if (!suspicious && !next && static_cast<std::int64_t>(currentTick) - shot->serverTick <= 1)
		{
			return false;
		}
		if (previous && next && shot->commandNumber - previous->commandNumber == 1 && next->commandNumber - shot->commandNumber == 1
			&& static_cast<std::int64_t>(shot->clientTick) - previous->clientTick == 1
			&& static_cast<std::int64_t>(next->clientTick) - shot->clientTick == 1
			&& static_cast<std::int64_t>(shot->serverTick) - previous->serverTick >= 0
			&& static_cast<std::int64_t>(shot->serverTick) - previous->serverTick <= 1
			&& static_cast<std::int64_t>(next->serverTick) - shot->serverTick >= 0
			&& static_cast<std::int64_t>(next->serverTick) - shot->serverTick <= 1)
		{
			const float surrounding = AngularDistance(previous->angles, next->angles);
			const float snap = AngularDistance(previous->angles, shot->angles);
			const bool fresh = !data.hasCountedIncident || shot->commandNumber > data.lastCountedIncidentCommand;
			if (fresh && std::isfinite(surrounding) && std::isfinite(snap) && surrounding < 10.0f && snap > 0.5f && snap > surrounding * 5.0f)
			{
				if (!suspicious)
				{
					matchedRule = AimbotRule::SnapReturn;
					largestSnap = snap;
				}
				suspicious = true;
			}
		}

		if (!suspicious && smoothPoints.size() >= static_cast<size_t>(smoothMinimumSteps + 1))
		{
			std::reverse(smoothPoints.begin(), smoothPoints.end());
			std::vector<float> movements;
			movements.reserve(smoothPoints.size() - 1);
			for (size_t index = 1; index < smoothPoints.size(); ++index)
			{
				movements.push_back(AngularDistance(smoothPoints[index - 1].command->angles, smoothPoints[index].command->angles));
			}
			const auto largest = std::max_element(movements.begin(), movements.end());
			const size_t start = static_cast<size_t>(std::distance(movements.begin(), largest));
			const float path = std::accumulate(movements.begin() + static_cast<std::ptrdiff_t>(start), movements.end(), 0.0f);
			smoothMovement = AngularDistance(smoothPoints[start].command->angles, smoothPoints.back().command->angles);
			smoothBefore = smoothPoints[start].error;
			smoothAfter = smoothPoints.back().error;
			smoothEfficiency = path > EPSILON ? smoothMovement / path : 0.0f;
			const float meaningfulMinimum = (std::max)(smoothMinimumStep, *largest * smoothRelativeStep);
			float previousMeaningful = std::numeric_limits<float>::infinity();
			bool shrinking = true;
			for (size_t index = start; index < movements.size(); ++index)
			{
				if (movements[index] < meaningfulMinimum)
				{
					continue;
				}
				if (movements[index] >= previousMeaningful)
				{
					shrinking = false;
				}
				previousMeaningful = movements[index];
				++smoothSteps;
			}
			const float improvement = smoothBefore > EPSILON ? 1.0f - smoothAfter / smoothBefore : 0.0f;
			const bool fresh = !data.hasCountedIncident || shot->commandNumber > data.lastCountedIncidentCommand;
			if (fresh && MeetsSmoothThresholds(smoothMovement, improvement, smoothAfter, smoothEfficiency, smoothSteps, shrinking))
			{
				suspicious = true;
				matchedRule = AimbotRule::SmoothConvergence;
				largestSnap = smoothMovement;
				bestBefore = smoothBefore;
				bestAfter = smoothAfter;
			}
			else
			{
				AIMBOT_DEBUG("%s smooth curve rejected: movement %.2f/%.2f, target error %.2f -> %.2f/%.2f (%.1f%%/%.0f%% closer), "
							 "path %.1f%%/%.0f%%, steps %d/%d, shrinking %s.\n",
							 attacker->GetName(), smoothMovement, smoothMinimumMovement, smoothBefore, smoothAfter, smoothMaximumFinalError,
							 improvement * 100.0f, smoothMinimumImprovement * 100.0f, smoothEfficiency * 100.0f, smoothMinimumPathEfficiency * 100.0f,
							 smoothSteps, smoothMinimumSteps, shrinking ? "yes" : "no");
			}
		}

		if (!suspicious && smoothPoints.size() >= static_cast<size_t>(humanizedSteps + 1))
		{
			std::array<AimCommand *, humanizedSteps + 1> commands;
			std::transform(smoothPoints.end() - static_cast<std::ptrdiff_t>(commands.size()), smoothPoints.end(), commands.begin(),
						   [](const SmoothPoint &point) { return point.command; });
			const int estimatedLag = EstimateVisualLagTicks(attacker);
			HumanizedMeasurement best;
			HumanizedMeasurement passing;
			if (estimatedLag >= 0)
			{
				const int firstLag = (std::max)(0, estimatedLag - humanizedLagSearchRadius);
				const int lastLag = estimatedLag + humanizedLagSearchRadius;
				for (int lagTicks = firstLag; lagTicks <= lastLag; ++lagTicks)
				{
					const HumanizedMeasurement measured =
						MeasureHumanized(shots, attacker, data.victimIndex, smoothBodyPoint, commands, lagTicks);
					if (!measured.valid)
					{
						continue;
					}
					if (!best.valid || measured.fit > best.fit)
					{
						best = measured;
					}
					if (MeetsHumanizedThresholds(measured.gain, measured.movement, measured.fit, measured.alignment, measured.alignedSteps)
						&& (!passing.valid || measured.fit > passing.fit))
					{
						passing = measured;
					}
				}
			}

			const bool fresh = !data.hasCountedIncident || shot->commandNumber > data.lastCountedIncidentCommand;
			if (fresh && passing.valid)
			{
				const auto now = Clock::now();
				const bool compatible = data.humanizedGainValid && now - data.humanizedGainTime < evidenceWindow
								&& std::abs(passing.gain - data.humanizedGain) <= humanizedGainTolerance;
				if (!compatible)
				{
					data.humanizedGain = passing.gain;
					data.humanizedGainTime = now;
					data.humanizedGainValid = true;
					humanizedSeeded = true;
					AIMBOT_DEBUG(
						"%s seeded humanized smoothing at gain %.3f after six corrections moved %.2f degrees with %.1f%% fit, %.1f%% alignment, "
						"and %d/6 corrections toward the victim at visual delay %d ticks; a compatible repeat is required.\n",
						attacker->GetName(), passing.gain, passing.movement, passing.fit * 100.0f, passing.alignment * 100.0f,
						passing.alignedSteps, passing.lagTicks);
				}
				else
				{
					const float referenceGain = data.humanizedGain;
					data.humanizedGain = (data.humanizedGain + passing.gain) * 0.5f;
					data.humanizedGainTime = now;
					suspicious = true;
					matchedRule = AimbotRule::Humanized;
					largestSnap = passing.movement;
					humanizedGain = passing.gain;
					humanizedFit = passing.fit;
					AIMBOT_DEBUG(
						"%s matched repeated humanized smoothing: gain %.3f (reference %.3f), six corrections moved %.2f degrees with %.1f%% fit, "
						"%.1f%% alignment, and %d/6 corrections toward the victim at visual delay %d ticks.\n",
						attacker->GetName(), passing.gain, referenceGain, passing.movement, passing.fit * 100.0f,
						passing.alignment * 100.0f, passing.alignedSteps, passing.lagTicks);
				}
			}
			else if (estimatedLag < 0)
			{
				AIMBOT_DEBUG("%s humanized smoothing was unavailable because visual delay could not be estimated.\n", attacker->GetName());
			}
			else if (best.valid)
			{
				AIMBOT_DEBUG(
					"%s humanized smoothing rejected: gain %.3f/%.2f-%.2f, movement %.2f/%.2f, fit %.1f%%/%.0f%%, alignment "
					"%.1f%%/%.0f%%, corrections toward victim %d/%d, visual delay %d ticks.\n",
					attacker->GetName(), best.gain, humanizedMinimumGain, humanizedMaximumGain, best.movement, humanizedMinimumMovement,
					best.fit * 100.0f, humanizedMinimumFit * 100.0f, best.alignment * 100.0f, humanizedMinimumAlignment * 100.0f,
					best.alignedSteps, humanizedMinimumAlignedSteps, best.lagTicks);
			}
		}

		const int incidentCommand = shot->commandNumber;
		const std::uint64_t incidentShotId = data.pendingShotId;
		clearPending();
		if (!suspicious)
		{
			if (humanizedSeeded)
			{
				return true;
			}
			if (reusedSnap)
			{
				AIMBOT_DEBUG("%s ignored a snap that already counted for an earlier damaging shot.\n", attacker->GetName());
			}
			else
			{
				if (measuredMovements == 0)
				{
					AIMBOT_DEBUG("%s damaging shot rejected because %s.\n", attacker->GetName(), measurementFailure);
				}
				else
				{
					const float improvement = measuredBefore > EPSILON ? (1.0f - measuredAfter / measuredBefore) * 100.0f : 0.0f;
					if (largestMeasuredSnap <= 5.0f)
					{
						AIMBOT_DEBUG(
							"%s damaging shot rejected after checking %d adjacent movements: the largest one-command movement was %.2f degrees "
							"with target error %.2f -> %.2f (%.1f%% closer); movement must exceed 5.00 degrees.\n",
							attacker->GetName(), measuredMovements, largestMeasuredSnap, measuredBefore, measuredAfter, improvement);
					}
					else
					{
						const float requiredImprovement = largestMeasuredSnap > 10.0f ? 80.0f : 90.0f;
						AIMBOT_DEBUG(
							"%s damaging shot rejected after checking %d adjacent movements: the largest one-command movement was %.2f degrees "
							"with target error %.2f -> %.2f (%.1f%% closer); this movement requires more than %.0f%% improvement.\n",
							attacker->GetName(), measuredMovements, largestMeasuredSnap, measuredBefore, measuredAfter, improvement,
							requiredImprovement);
					}
				}
			}
			return true;
		}

		data.lastCountedIncidentCommand = incidentCommand;
		data.hasCountedIncident = true;
		const AimbotEvidenceType evidenceType = matchedRule == AimbotRule::SnapReturn          ? AimbotEvidenceType::SnapReturn
												: matchedRule == AimbotRule::SmoothConvergence ? AimbotEvidenceType::SmoothConvergence
												: matchedRule == AimbotRule::Humanized         ? AimbotEvidenceType::Humanized
																						   : AimbotEvidenceType::Convergence;
		PendingAimbotIncident pending;
		pending.shotId = incidentShotId;
		pending.fireTick = shot->serverTick;
		pending.incident.time = Clock::now();
		pending.incident.type = evidenceType;
		pending.incident.movement = largestSnap;
		pending.incident.before = bestBefore;
		pending.incident.after = bestAfter;
		pending.incident.gain = humanizedGain;
		pending.incident.fit = humanizedFit;
		if (const ShotRecord *record = shots->FindShot(attacker->index, incidentShotId))
		{
			pending.fireTick = record->fireTick;
			pending.incident.airborne = record->airborne;
			pending.incident.wallbang = record->wallbang;
			pending.incident.throughSmoke = record->throughSmoke;
			pending.incident.headshot = record->headshot;
			pending.incident.noscope = !record->scoped && IsScopedRifle(record->weapon);
		}
		pendingEvidence[attacker->index].push_back(std::move(pending));
		AIMBOT_DEBUG("%s qualified command %d for contextual scoring.\n", attacker->GetName(), incidentCommand);
		return true;
	}

	void AimbotModule::FinalizePending(MovementPlayer *attacker, int currentTick)
	{
		auto &pending = pendingEvidence[attacker->index];
		for (auto incident = pending.begin(); incident != pending.end();)
		{
			const std::int64_t age = static_cast<std::int64_t>(currentTick) - incident->fireTick;
			if (incident->fireTick >= 0 && age < contextWaitTicks)
			{
				++incident;
				continue;
			}

			if (const ShotRecord *record = shots->FindShot(attacker->index, incident->shotId))
			{
				incident->incident.airborne = incident->incident.airborne || record->airborne;
				incident->incident.wallbang = incident->incident.wallbang || record->wallbang;
				incident->incident.throughSmoke = incident->incident.throughSmoke || record->throughSmoke;
				incident->incident.headshot = incident->incident.headshot || record->headshot;
				incident->incident.noscope = incident->incident.noscope || (!record->scoped && IsScopedRifle(record->weapon));
			}
			AddIncident(attacker, *incident);
			incident = pending.erase(incident);
		}
	}

	void AimbotModule::AddIncident(MovementPlayer *attacker, const PendingAimbotIncident &pending)
	{
		AimbotIncident added = pending.incident;
		const int basePoints = added.type == AimbotEvidenceType::Humanized ? humanizedBaseIncidentPoints : baseIncidentPoints;
		added.points = AimbotPoints(basePoints, added.airborne, added.wallbang, added.throughSmoke, added.headshot, added.noscope);

		const auto now = Clock::now();
		auto &incidents = evidence[attacker->index];
		while (!incidents.empty() && now - incidents.front().time >= evidenceWindow)
		{
			incidents.pop_front();
		}
		incidents.push_back(added);
		const int score =
			std::accumulate(incidents.begin(), incidents.end(), 0, [](int total, const AimbotIncident &incident) { return total + incident.points; });

		const char *type = added.type == AimbotEvidenceType::SnapReturn          ? "snap-return"
						   : added.type == AimbotEvidenceType::SmoothConvergence ? "smooth convergence"
						   : added.type == AimbotEvidenceType::Humanized         ? "humanized"
																		 : "convergence";
		AIMBOT_DEBUG("%s counted %s %.2f for +%d points; score %d/%d.\n", attacker->GetName(), type, added.movement, added.points, score,
					 detectionThreshold);
		if (score < detectionThreshold)
		{
			return;
		}

		if (announce)
		{
			auto describe = [](const AimbotIncident &incident) -> localization::Text
			{
				switch (incident.type)
				{
					case AimbotEvidenceType::SnapReturn:
						return localization::Format("evidence.aimbot.snap_return", "{movement}° snap-return",
													{{"movement", tfm::format("%.2f", incident.movement)}});
					case AimbotEvidenceType::SmoothConvergence:
						return localization::Format("evidence.aimbot.smooth", "{movement}° smooth move ({before}°→{after}°)",
													{{"movement", tfm::format("%.2f", incident.movement)},
													 {"before", tfm::format("%.2f", incident.before)},
													 {"after", tfm::format("%.2f", incident.after)}});
					case AimbotEvidenceType::Humanized:
						return localization::Format("evidence.aimbot.humanized", "{movement}° humanized smoothing (gain {gain}%, fit {fit}%)",
														{{"movement", tfm::format("%.2f", incident.movement)},
														 {"gain", tfm::format("%.1f", incident.gain * 100.0f)},
														 {"fit", tfm::format("%.1f", incident.fit * 100.0f)}});
					case AimbotEvidenceType::Convergence:
						return localization::Format("evidence.aimbot.convergence", "{movement}° sudden move ({before}°→{after}°)",
													{{"movement", tfm::format("%.2f", incident.movement)},
													 {"before", tfm::format("%.2f", incident.before)},
													 {"after", tfm::format("%.2f", incident.after)}});
				}
				return {};
			};
			auto addWeights = [](localization::Text text, const AimbotIncident &incident)
			{
				const auto base = localization::Format("evidence.aimbot.weight.base", "suspicious aim");
				const int basePoints = incident.type == AimbotEvidenceType::Humanized ? humanizedBaseIncidentPoints : baseIncidentPoints;
				std::string englishWeights = tfm::format("%s +%d", base.english.c_str(), basePoints);
				std::string localizedWeights = tfm::format("%s +%d", base.localized.c_str(), basePoints);
				auto add = [&](bool present, const char *key, const char *english)
				{
					if (present)
					{
						const auto label = localization::Format(key, english);
						englishWeights += tfm::format(", %s +1", label.english.c_str());
						localizedWeights += tfm::format(", %s +1", label.localized.c_str());
					}
				};
				add(incident.airborne, "evidence.aimbot.weight.airborne", "airborne");
				add(incident.wallbang, "evidence.aimbot.weight.wallbang", "wallbang");
				add(incident.throughSmoke, "evidence.aimbot.weight.smoke", "through smoke");
				add(incident.headshot, "evidence.aimbot.weight.headshot", "headshot");
				add(incident.noscope, "evidence.aimbot.weight.noscope", "no-scope");
				return localization::Text {text.english + tfm::format(" [%s = +%d]", englishWeights.c_str(), incident.points),
										   text.localized + tfm::format(" [%s = +%d]", localizedWeights.c_str(), incident.points)};
			};

			std::vector<localization::Text> history;
			for (size_t index = 0; index + 1 < incidents.size(); ++index)
			{
				history.push_back(addWeights(describe(incidents[index]), incidents[index]));
			}
			const auto values = localization::Arguments {{"score", tfm::format("%d", score)},
														 {"threshold", tfm::format("%d", detectionThreshold)},
														 {"snap", tfm::format("%.2f", added.movement)},
														 {"before", tfm::format("%.2f", added.before)},
														 {"after", tfm::format("%.2f", added.after)},
														 {"gain", tfm::format("%.1f", added.gain * 100.0f)},
														 {"fit", tfm::format("%.1f", added.fit * 100.0f)}};
			localization::Text latest =
				added.type == AimbotEvidenceType::SnapReturn
					? localization::Format("evidence.aimbot.latest.snap_return",
										   "During the latest damaging shot, the aim jumped {snap} degrees and immediately returned. Score: "
										   "{score}/{threshold} within five minutes.",
										   values)
				: added.type == AimbotEvidenceType::SmoothConvergence
					? localization::Format(
						  "evidence.aimbot.latest.smooth",
						  "During the latest damaging shot, the aim followed an unusually clean {snap} degree curve and its distance "
						  "from the enemy fell from {before} to {after} degrees. Score: {score}/{threshold} within five minutes.",
						  values)
				: added.type == AimbotEvidenceType::Humanized
					? localization::Format(
						  "evidence.aimbot.latest.humanized",
						  "During the latest damaging shot, six consecutive aim corrections followed a consistent {gain}% smoothing factor "
						  "toward the enemy with {fit}% proportional fit. Score: {score}/{threshold} within five minutes.",
						  values)
					: localization::Format(
						  "evidence.aimbot.latest.convergence",
						  "During the latest damaging shot, the aim moved {snap} degrees in one step and its distance from the enemy "
						  "fell from {before} to {after} degrees. Score: {score}/{threshold} within five minutes.",
						  values);
			latest = addWeights(std::move(latest), added);
			announce("AIMBOT", attacker, FormatEvidenceHistory(history, latest));
		}
		incidents.clear();
	}

	void AimbotModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
			pendingEvidence[player->index].clear();
			evidence[player->index].clear();
		}
	}
} // namespace detection
