#include "detection/detection_system.h"

#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/usercmd.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
	constexpr int detectionThreshold = 3;
	constexpr auto evidenceWindow = std::chrono::minutes(5);

	enum class AimbotRule
	{
		None,
		Convergence,
		SnapReturn,
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
		static constexpr float bodyHeights[] = {8.0f, 46.0f, 64.0f};
		float best = 180.0f;
		for (float height : bodyHeights)
		{
			best = (std::min)(best, AimError(eye, angles, feet + Vector(0.0f, 0.0f, height)));
		}
		return best;
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
			if (!playerData[index].pending)
			{
				continue;
			}
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			if (!IsEligibleHuman(player))
			{
				playerData[index] = {};
				continue;
			}
			Evaluate(player, playerData[index], currentTick);
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
		AimbotRule matchedRule = AimbotRule::None;
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
		if (!suspicious && !next)
		{
			if (static_cast<std::int64_t>(currentTick) - shot->serverTick <= 1)
			{
				return false;
			}
			AIMBOT_DEBUG("%s rejected because the next adjacent simulated command never arrived.\n", attacker->GetName());
			clearPending();
			return true;
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

		const int incidentCommand = shot->commandNumber;
		clearPending();
		if (!suspicious)
		{
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
		const auto now = Clock::now();
		auto &incidents = evidence[attacker->index];
		while (!incidents.empty() && now - incidents.front() >= evidenceWindow)
		{
			incidents.pop_front();
		}
		incidents.push_back(now);
		if (matchedRule == AimbotRule::SnapReturn)
		{
			AIMBOT_DEBUG("%s counted snap-return %.2f, evidence %d/%d.\n", attacker->GetName(), largestSnap, static_cast<int>(incidents.size()),
						 detectionThreshold);
		}
		else
		{
			AIMBOT_DEBUG("%s counted convergence %.2f, target error %.2f -> %.2f, evidence %d/%d.\n", attacker->GetName(), largestSnap, bestBefore,
						 bestAfter, static_cast<int>(incidents.size()), detectionThreshold);
		}
		if (incidents.size() >= detectionThreshold)
		{
			if (announce)
			{
				const localization::Text details =
					matchedRule == AimbotRule::SnapReturn
						? localization::Format("evidence.aimbot.snap_return",
											   "{incidents} snap-hit incidents reached the threshold; the latest was a {snap}-degree snap-return.",
											   {{"incidents", tfm::format("%zu", incidents.size())}, {"snap", tfm::format("%.2f", largestSnap)}})
						: localization::Format("evidence.aimbot.convergence",
											   "{incidents} snap-hit incidents reached the threshold; latest snap {snap} degrees, target error "
											   "{before} -> {after} degrees.",
											   {{"incidents", tfm::format("%zu", incidents.size())},
												{"snap", tfm::format("%.2f", largestSnap)},
												{"before", tfm::format("%.2f", bestBefore)},
												{"after", tfm::format("%.2f", bestAfter)}});
				announce("AIMBOT", attacker, details);
			}
			incidents.clear();
		}
		return true;
	}

	void AimbotModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
			evidence[player->index].clear();
		}
	}
} // namespace detection
