#include "detection/detection_system.h"

#include "igameevents.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/usercmd.h"

#include <algorithm>
#include <cmath>
#include <limits>

CConVar<bool> cs2ac_antiaim_debug("cs2ac_antiaim_debug", FCVAR_NONE, "Show AntiAim evidence, score decay, and shot matching", false);

#define ANTIAIM_DEBUG(...) \
	do \
	{ \
		if (cs2ac_antiaim_debug.GetBool()) \
			Msg("[CS2AC AntiAim] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr int commandHistorySize = 96;
	constexpr int spinSamples = 16;
	constexpr float invalidPitch = 89.01f;
	constexpr float invalidRoll = 50.01f;
	constexpr float minimumSpinRate = 320.0f;
	constexpr float mediumSpinRate = 1000.0f;
	constexpr float fastSpinRate = 2200.0f;
	constexpr float slowSpinSeconds = 15.0f;
	constexpr float mediumSpinSeconds = 10.0f;
	constexpr float fastSpinSeconds = 10.0f;
	constexpr float spinBreakAllowance = 1.0f;
	constexpr float spinConsistency = 0.85f;
	constexpr float jitterTolerance = 0.25f;
	constexpr float minimumJitterSpan = 10.0f;
	constexpr float requiredJitterSeconds = 10.0f;
	constexpr float commandYawMismatchAngle = 120.0f;
	constexpr float minimumAttackReturnAngle = 30.0f;
	constexpr int commandMismatchSpacing = 4;
	constexpr float detectionThreshold = 100.0f;
	constexpr float scoreDecayPerSecond = 2.0f;
	constexpr float mismatchScoreDecayPerSecond = 5.0f;

	enum CommandProblem : std::uint32_t
	{
		NonFiniteBaseAngles = 1 << 0,
		InvalidAttackHistory = 1 << 1,
		NonFiniteHistoryAngles = 1 << 2,
		NonFiniteSubtickAngles = 1 << 3,
	};

	float YawDelta(float from, float to)
	{
		return std::remainder(to - from, 360.0f);
	}

	const char *ProblemName(std::uint32_t problems)
	{
		if (problems & NonFiniteBaseAngles)
		{
			return "the base view angle is not finite";
		}
		if (problems & InvalidAttackHistory)
		{
			return "the firing history index is invalid";
		}
		if (problems & NonFiniteHistoryAngles)
		{
			return "an input-history angle is not finite";
		}
		return "a subtick angle delta is not finite";
	}
} // namespace

namespace detection
{
	void AntiAimModule::Load(AnnounceCallback announceCallback, AnnounceCallback networkVetoCallback, NetworkSafetyMonitor *networkSafetyMonitor)
	{
		announce = announceCallback;
		announceNetworkVeto = networkVetoCallback;
		networkSafety = networkSafetyMonitor;
	}

	void AntiAimModule::Unload()
	{
		Reset();
		announce = nullptr;
		announceNetworkVeto = nullptr;
		networkSafety = nullptr;
	}

	void AntiAimModule::Reset()
	{
		playerData = {};
	}

	void AntiAimModule::ResetMotion(AntiAimPlayerData &data)
	{
		data.spinSeconds = {};
		data.spinBreakSeconds = {};
		data.jitterSeconds = 0.0f;
		data.jitterBreakSeconds = 0.0f;
		data.spinDebugBucket = -1;
		data.jitterDebugBucket = -1;
		data.lastMotionServerTick = -1;
		data.spinActive = false;
		data.jitterActive = false;
	}

	void AntiAimModule::ApplyDecay(MovementPlayer *player, AntiAimPlayerData &data)
	{
		auto now = std::chrono::steady_clock::now();
		if (data.scoreTime.time_since_epoch().count() == 0)
		{
			data.scoreTime = now;
			return;
		}
		float elapsed = std::chrono::duration<float>(now - data.scoreTime).count();
		data.scoreTime = now;
		if (elapsed <= 0.0f || (data.score <= 0.0f && data.mismatchScore <= 0.0f))
		{
			return;
		}
		int before = static_cast<int>(std::ceil(data.score + data.mismatchScore - 0.0001f));
		data.score = (std::max)(0.0f, data.score - elapsed * scoreDecayPerSecond);
		data.mismatchScore = (std::max)(0.0f, data.mismatchScore - elapsed * mismatchScoreDecayPerSecond);
		const float total = data.score + data.mismatchScore;
		int after = static_cast<int>(std::ceil(total - 0.0001f));
		if (player && after < before)
		{
			ANTIAIM_DEBUG("%s score decayed to %.1f/%.0f (regular %.1f, mismatch %.1f).\n", player->GetName(), total, detectionThreshold, data.score,
						  data.mismatchScore);
		}
	}

	void AntiAimModule::AddEvidence(MovementPlayer *player, AntiAimPlayerData &data, float weight, const char *reasonKey, const char *reason,
									bool continuous, bool mismatch)
	{
		if (!player || data.suppressContinuous)
		{
			return;
		}
		ApplyDecay(player, data);
		(mismatch ? data.mismatchScore : data.score) += weight;
		const float total = data.score + data.mismatchScore;
		ANTIAIM_DEBUG("%s added %.1f for %s; score %.1f/%.0f (regular %.1f, mismatch %.1f).\n", player->GetName(), weight, reason, total,
					  detectionThreshold, data.score, data.mismatchScore);
		if (total < detectionThreshold)
		{
			return;
		}

		const bool mismatchRequired = data.mismatchScore > 0.0f && data.score < detectionThreshold;
		NetworkSafetyEvidence network;
		if (mismatchRequired)
		{
			if (networkSafety)
			{
				network = networkSafety->Evaluate(player);
			}
			else
			{
				network.unavailableSamples = 1;
				network.vetoed = true;
			}
		}
		const bool networkVetoed = mismatchRequired && network.vetoed;
		const AnnounceCallback callback = networkVetoed ? announceNetworkVeto : announce;
		if (callback)
		{
			const std::string localizedReason = localization::Get(reasonKey, reason);
			const localization::Text details {
				tfm::format("%s added %.1f points and reached %.1f/%.0f evidence.", reason, weight, total, detectionThreshold),
				localization::Format("evidence.antiaim", "{reason} added {points} points and reached {score}/{threshold} evidence.",
									 {{"reason", localizedReason},
									  {"points", tfm::format("%.1f", weight)},
									  {"score", tfm::format("%.1f", total)},
									  {"threshold", tfm::format("%.0f", detectionThreshold)}})
					.localized,
			};
			callback("ANTIAIM", player, networkVetoed ? AddNetworkSafetyDetails(details, network) : details);
		}
		data.score = 0.0f;
		data.mismatchScore = 0.0f;
		data.scoreTime = std::chrono::steady_clock::now();
		data.suppressContinuous = continuous;
		if (networkVetoed)
		{
			ANTIAIM_DEBUG(
				"%s reached the threshold through mismatch evidence, but punishment was withheld: %.1f ms ping, %.1f ms jitter, %.1f/%.1f%% "
				"loss, %.1f/%.1f%% choke, %d command gaps, and %d unavailable samples.\n",
				player->GetName(), network.pingMilliseconds, network.jitterMilliseconds, network.incomingLoss * 100.0f, network.outgoingLoss * 100.0f,
				network.incomingChoke * 100.0f, network.outgoingChoke * 100.0f, network.commandGaps, network.unavailableSamples);
		}
		else
		{
			ANTIAIM_DEBUG("%s reached the threshold; evidence was cleared%s.\n", player->GetName(),
						  continuous ? " until this continuous episode ends" : "");
		}
	}

	void AntiAimModule::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
	{
		if (!player || !commands || numCommands <= 0 || player->index < 1 || player->index > MAXPLAYERS || !player->GetController()
			|| player->IsFakeClient() || player->IsCSTV())
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
							[&](const AntiAimCommand &stored) { return stored.commandNumber == command.cmdNum; }))
			{
				continue;
			}

			const auto &base = command.base();
			const auto &view = base.viewangles();
			AntiAimCommand captured;
			captured.commandNumber = command.cmdNum;
			captured.clientTick = base.client_tick();
			captured.baseAngles = {view.x(), view.y(), view.z()};
			captured.mouseX = base.mousedx();
			captured.mouseY = base.mousedy();
			if (!IsFinite(captured.baseAngles))
			{
				captured.problems |= NonFiniteBaseAngles;
			}

			int attackIndex = command.attack1_start_history_index();
			captured.attack = attackIndex >= 0;
			if (attackIndex < -1 || attackIndex >= command.input_history_size())
			{
				captured.problems |= InvalidAttackHistory;
			}
			for (int historyIndex = 0; historyIndex < command.input_history_size(); ++historyIndex)
			{
				const auto &history = command.input_history(historyIndex);
				if (!history.has_view_angles())
				{
					continue;
				}
				const auto &historyView = history.view_angles();
				QAngle historyAngles(historyView.x(), historyView.y(), historyView.z());
				if (!IsFinite(historyAngles))
				{
					captured.problems |= NonFiniteHistoryAngles;
					continue;
				}
				captured.hasHistoryAngles = true;
				float yawDifference = std::abs(YawDelta(captured.baseAngles.y, historyAngles.y));
				captured.historyYawDifference = (std::max)(captured.historyYawDifference, yawDifference);
				if (historyIndex == attackIndex)
				{
					captured.shotAngles = historyAngles;
					captured.hasShotAngles = true;
				}
			}
			for (int moveIndex = 0; moveIndex < base.subtick_moves_size(); ++moveIndex)
			{
				const auto &move = base.subtick_moves(moveIndex);
				if ((move.has_pitch_delta() && !std::isfinite(move.pitch_delta())) || (move.has_yaw_delta() && !std::isfinite(move.yaw_delta())))
				{
					captured.problems |= NonFiniteSubtickAngles;
					continue;
				}
				if (move.has_pitch_delta())
				{
					captured.subtickPitch += move.pitch_delta();
				}
				if (move.has_yaw_delta())
				{
					captured.subtickYaw += move.yaw_delta();
				}
			}
			if (!std::isfinite(captured.subtickPitch) || !std::isfinite(captured.subtickYaw))
			{
				captured.problems |= NonFiniteSubtickAngles;
				captured.subtickPitch = 0.0f;
				captured.subtickYaw = 0.0f;
			}

			data.commands.push_back(captured);
			while (data.commands.size() > commandHistorySize)
			{
				data.commands.pop_front();
			}
		}
	}

	void AntiAimModule::EvaluateMotion(MovementPlayer *player, AntiAimPlayerData &data, AntiAimCommand &command)
	{
		if (command.serverTick == data.lastMotionServerTick)
		{
			return;
		}
		const std::int64_t serverGap = data.lastMotionServerTick < 0 ? 1 : static_cast<std::int64_t>(command.serverTick) - data.lastMotionServerTick;
		if (serverGap <= 0)
		{
			ResetMotion(data);
			return;
		}
		if (serverGap > ENGINE_FIXED_TICK_RATE)
		{
			ResetMotion(data);
		}
		data.lastMotionServerTick = command.serverTick;

		std::array<AntiAimCommand *, 20> history {};
		size_t historyCount = 0;
		std::int64_t wantedTick = command.serverTick;
		for (auto candidate = data.commands.rbegin(); candidate != data.commands.rend() && historyCount < history.size(); ++candidate)
		{
			if (!candidate->simulated || !IsFinite(candidate->baseAngles) || candidate->serverTick > wantedTick)
			{
				continue;
			}
			if (candidate->serverTick < wantedTick)
			{
				break;
			}
			history[historyCount++] = &*candidate;
			--wantedTick;
		}

		bool spinMatches = false;
		float spinRate = 0.0f;
		float spinDirectionConsistency = 0.0f;
		if (historyCount >= spinSamples)
		{
			float total = 0.0f;
			float net = 0.0f;
			for (int i = 0; i < spinSamples - 1; ++i)
			{
				float delta = YawDelta(history[i + 1]->baseAngles.y, history[i]->baseAngles.y);
				total += std::abs(delta);
				net += delta;
			}
			float elapsed =
				static_cast<float>(static_cast<std::int64_t>(history[0]->serverTick) - history[spinSamples - 1]->serverTick) / ENGINE_FIXED_TICK_RATE;
			spinRate = elapsed > 0.0f ? total / elapsed : 0.0f;
			spinDirectionConsistency = total > 0.0f ? std::abs(net) / total : 0.0f;
			float latestRate = std::abs(YawDelta(history[1]->baseAngles.y, history[0]->baseAngles.y)) * ENGINE_FIXED_TICK_RATE;
			spinMatches = spinRate >= minimumSpinRate && latestRate >= minimumSpinRate && spinDirectionConsistency >= spinConsistency;
		}

		const float commandSeconds = static_cast<float>(serverGap) / ENGINE_FIXED_TICK_RATE;
		static constexpr float tierRates[] = {minimumSpinRate, mediumSpinRate, fastSpinRate};
		static constexpr float tierSeconds[] = {slowSpinSeconds, mediumSpinSeconds, fastSpinSeconds};
		bool spinDetected = false;
		bool spinEpisodeActive = false;
		float bestProgress = 0.0f;
		for (size_t tier = 0; tier < std::size(tierRates); ++tier)
		{
			if (spinMatches && spinRate >= tierRates[tier] && serverGap == 1)
			{
				data.spinBreakSeconds[tier] = 0.0f;
				if (!data.suppressContinuous)
				{
					data.spinSeconds[tier] += 1.0f / ENGINE_FIXED_TICK_RATE;
				}
				spinEpisodeActive = true;
			}
			else if (data.spinSeconds[tier] > 0.0f || data.spinBreakSeconds[tier] > 0.0f)
			{
				data.spinBreakSeconds[tier] += commandSeconds;
				if (data.spinBreakSeconds[tier] > spinBreakAllowance)
				{
					data.spinSeconds[tier] = 0.0f;
					data.spinBreakSeconds[tier] = 0.0f;
				}
				else
				{
					spinEpisodeActive = true;
				}
			}

			bestProgress = (std::max)(bestProgress, data.spinSeconds[tier] / tierSeconds[tier]);
			spinDetected = spinDetected || data.spinSeconds[tier] >= tierSeconds[tier];
		}
		data.spinActive = spinEpisodeActive;
		const int debugBucket = static_cast<int>(bestProgress * 10.0f);
		if (spinMatches && debugBucket > data.spinDebugBucket)
		{
			data.spinDebugBucket = debugBucket;
			ANTIAIM_DEBUG("%s sustained %.0f deg/s spin with %.2f direction consistency; progress %d%%.\n", player->GetName(), spinRate,
						  spinDirectionConsistency, (std::min)(debugBucket * 10, 100));
		}
		if (spinDetected && !data.suppressContinuous)
		{
			AddEvidence(player, data, detectionThreshold, "evidence.antiaim.reason.spin", "continuous spin", true);
			data.spinActive = true;
		}
		else if (!spinEpisodeActive)
		{
			if (data.spinDebugBucket >= 0)
			{
				ANTIAIM_DEBUG("%s spin stopped; progress was reset.\n", player->GetName());
			}
			data.spinDebugBucket = -1;
		}

		data.jitterActive = false;
		int jitterPeriod = 0;
		for (int period : {2, 3, 5})
		{
			int required = period * 4;
			if (historyCount < static_cast<size_t>(required))
			{
				continue;
			}
			bool repeats = true;
			for (int i = 0; i < required - period; ++i)
			{
				if (std::abs(YawDelta(history[i + period]->baseAngles.y, history[i]->baseAngles.y)) > jitterTolerance)
				{
					repeats = false;
					break;
				}
			}
			float span = 0.0f;
			for (int i = 0; i < period; ++i)
			{
				for (int j = i + 1; j < period; ++j)
				{
					span = (std::max)(span, std::abs(YawDelta(history[i]->baseAngles.y, history[j]->baseAngles.y)));
				}
			}
			if (repeats && span > minimumJitterSpan)
			{
				jitterPeriod = period;
				break;
			}
		}
		bool jitterEpisodeActive = false;
		// Require sustained jitter instead of scoring every repeated step; legitimate 180-degree binds can briefly form the same pattern.
		if (jitterPeriod != 0 && serverGap == 1)
		{
			data.jitterBreakSeconds = 0.0f;
			if (!data.suppressContinuous)
			{
				data.jitterSeconds += 1.0f / ENGINE_FIXED_TICK_RATE;
			}
			jitterEpisodeActive = true;
		}
		else if (data.jitterSeconds > 0.0f || data.jitterBreakSeconds > 0.0f)
		{
			data.jitterBreakSeconds += commandSeconds;
			if (data.jitterBreakSeconds > spinBreakAllowance)
			{
				data.jitterSeconds = 0.0f;
				data.jitterBreakSeconds = 0.0f;
			}
			else
			{
				jitterEpisodeActive = true;
			}
		}

		data.jitterActive = jitterEpisodeActive;
		const int jitterDebugBucket = static_cast<int>(data.jitterSeconds / requiredJitterSeconds * 10.0f);
		if (jitterPeriod != 0 && jitterDebugBucket > data.jitterDebugBucket)
		{
			data.jitterDebugBucket = jitterDebugBucket;
			ANTIAIM_DEBUG("%s sustained an exact %d-way yaw pattern; progress %d%%.\n", player->GetName(), jitterPeriod,
						  (std::min)(jitterDebugBucket * 10, 100));
		}
		if (data.jitterSeconds >= requiredJitterSeconds && !data.suppressContinuous)
		{
			AddEvidence(player, data, detectionThreshold, "evidence.antiaim.reason.jitter", "continuous repeating jitter", true);
			data.jitterActive = true;
		}
		else if (!jitterEpisodeActive)
		{
			if (data.jitterDebugBucket >= 0)
			{
				ANTIAIM_DEBUG("%s repeating jitter stopped; progress was reset.\n", player->GetName());
			}
			data.jitterDebugBucket = -1;
		}
	}

	void AntiAimModule::EvaluatePendingShot(MovementPlayer *player, AntiAimPlayerData &data, int currentTick)
	{
		if (data.pendingShot < 0)
		{
			return;
		}
		if (data.pendingShot == (std::numeric_limits<int>::max)())
		{
			data.pendingShot = -1;
			data.pendingShotTick = -1;
			return;
		}
		auto find = [&](int commandNumber) -> AntiAimCommand *
		{
			auto command = std::find_if(data.commands.rbegin(), data.commands.rend(),
										[&](AntiAimCommand &candidate) { return candidate.commandNumber == commandNumber && candidate.simulated; });
			return command == data.commands.rend() ? nullptr : &*command;
		};
		AntiAimCommand *previous = find(data.pendingShot - 1);
		AntiAimCommand *shot = find(data.pendingShot);
		AntiAimCommand *next = find(data.pendingShot + 1);
		if (!next)
		{
			if (static_cast<std::int64_t>(currentTick) - data.pendingShotTick > 1
				|| (!data.commands.empty() && static_cast<std::int64_t>(data.commands.back().commandNumber) - data.pendingShot > 1))
			{
				ANTIAIM_DEBUG("%s attack-return expired without the next adjacent simulated command.\n", player->GetName());
				data.pendingShot = -1;
				data.pendingShotTick = -1;
			}
			return;
		}
		data.pendingShot = -1;
		data.pendingShotTick = -1;
		if (!previous || !shot || !IsFinite(previous->baseAngles) || !IsFinite(shot->baseAngles) || !IsFinite(next->baseAngles)
			|| static_cast<std::int64_t>(shot->clientTick) - previous->clientTick != 1
			|| static_cast<std::int64_t>(next->clientTick) - shot->clientTick != 1
			|| static_cast<std::int64_t>(shot->serverTick) - previous->serverTick != 1
			|| static_cast<std::int64_t>(next->serverTick) - shot->serverTick != 1)
		{
			ANTIAIM_DEBUG("%s attack-return rejected because its surrounding simulated commands are incomplete.\n", player->GetName());
			return;
		}

		float surrounding = AngularDistance(previous->baseAngles, next->baseAngles);
		float snap = AngularDistance(previous->baseAngles, shot->baseAngles);
		ANTIAIM_DEBUG("%s shot command %d: base/history yaw %.2f, surrounding %.2f, snap %.2f, mouse %d/%d, subtick %.2f/%.2f.\n", player->GetName(),
					  shot->commandNumber, shot->historyYawDifference, surrounding, snap, shot->mouseX, shot->mouseY, shot->subtickPitch,
					  shot->subtickYaw);
		if (std::isfinite(surrounding) && std::isfinite(snap) && surrounding < 10.0f && snap > minimumAttackReturnAngle && snap > surrounding * 5.0f)
		{
			AddEvidence(player, data, 20.0f, "evidence.antiaim.reason.attack_return", "one-command attack return", false);
		}
		else
		{
			ANTIAIM_DEBUG("%s attack-return rejected because the shot did not sharply leave and return to the surrounding angle.\n",
						  player->GetName());
		}
	}

	void AntiAimModule::OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick)
	{
		if (!player || player->index < 1 || player->index > MAXPLAYERS)
		{
			return;
		}
		auto &data = playerData[player->index];
		if (!command || !player->GetController() || !player->GetPlayerPawn() || !player->IsAlive() || player->IsFakeClient() || player->IsCSTV())
		{
			data.commands.clear();
			data.pendingShot = -1;
			data.pendingShotTick = -1;
			data.lastMismatchEvidenceCommand = -1;
			data.invalidActive = false;
			data.inconsistencyActive = false;
			data.suppressContinuous = false;
			ResetMotion(data);
			return;
		}
		ApplyDecay(player, data);
		auto found = std::find_if(data.commands.rbegin(), data.commands.rend(),
								  [&](AntiAimCommand &stored) { return stored.commandNumber == command->cmdNum; });
		if (found == data.commands.rend())
		{
			data.pendingShot = -1;
			data.pendingShotTick = -1;
			ResetMotion(data);
			return;
		}
		found->simulated = true;
		found->serverTick = currentTick;

		auto *cs2acPlayer = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(player->index));
		if (cs2acPlayer && cs2acPlayer->JustTeleported(2.0f))
		{
			if (!data.teleportGrace)
			{
				ANTIAIM_DEBUG("%s is inside the two-second spawn or teleport grace period.\n", player->GetName());
			}
			data.teleportGrace = true;
			data.invalidActive = data.inconsistencyActive = data.spinActive = data.jitterActive = false;
			data.pendingShot = -1;
			data.pendingShotTick = -1;
			ResetMotion(data);
			return;
		}
		data.teleportGrace = false;

		bool wasInconsistent = data.inconsistencyActive;
		bool historyMismatch = !found->attack && found->hasHistoryAngles && std::isfinite(found->historyYawDifference)
							   && found->historyYawDifference >= commandYawMismatchAngle;
		data.inconsistencyActive = found->problems != 0 || historyMismatch;
		if (found->problems != 0 && !data.suppressContinuous)
		{
			ANTIAIM_DEBUG("%s command %d is inconsistent: %s.\n", player->GetName(), found->commandNumber, ProblemName(found->problems));
			AddEvidence(player, data, 1.0f, "evidence.antiaim.reason.inconsistent_command", "an inconsistent angle command", true);
		}
		else if (historyMismatch && !data.suppressContinuous
				 && (data.lastMismatchEvidenceCommand < 0
					 || static_cast<std::int64_t>(found->commandNumber) - data.lastMismatchEvidenceCommand >= commandMismatchSpacing))
		{
			data.lastMismatchEvidenceCommand = found->commandNumber;
			ANTIAIM_DEBUG("%s command %d base/input-history yaw mismatch is %.2f degrees.\n", player->GetName(), found->commandNumber,
						  found->historyYawDifference);
			// Fast legitimate mouse movement can create this difference, so it contributes only short-lived supporting evidence.
			AddEvidence(player, data, 1.0f, "evidence.antiaim.reason.history_mismatch", "a repeated base and input-history mismatch", true, true);
		}
		else if (!data.inconsistencyActive && wasInconsistent)
		{
			ANTIAIM_DEBUG("%s angle commands returned to a consistent state.\n", player->GetName());
		}

		bool wasInvalid = data.invalidActive;
		data.invalidActive =
			IsFinite(found->baseAngles) && (std::abs(found->baseAngles.x) > invalidPitch || std::abs(found->baseAngles.z) > invalidRoll);
		if (data.invalidActive && (!wasInvalid || !data.suppressContinuous))
		{
			ANTIAIM_DEBUG("%s command %d has invalid pitch/roll %.2f/%.2f.\n", player->GetName(), found->commandNumber, found->baseAngles.x,
						  found->baseAngles.z);
			AddEvidence(player, data, 2.0f, "evidence.antiaim.reason.invalid_angles", "invalid pitch or roll", true);
		}

		EvaluateMotion(player, data, *found);
		EvaluatePendingShot(player, data, currentTick);

		if (data.suppressContinuous && !data.invalidActive && !data.inconsistencyActive && !data.spinActive && !data.jitterActive)
		{
			data.suppressContinuous = false;
			ANTIAIM_DEBUG("%s continuous detection episode ended.\n", player->GetName());
		}
	}

	void AntiAimModule::OnGameFrame(int currentTick)
	{
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto &data = playerData[index];
			if (data.pendingShot < 0 || static_cast<std::int64_t>(currentTick) - data.pendingShotTick <= 1)
			{
				continue;
			}
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			if (IsEligibleHuman(player))
			{
				EvaluatePendingShot(player, data, currentTick);
			}
			else
			{
				data.pendingShot = -1;
				data.pendingShotTick = -1;
			}
		}
	}

	void AntiAimModule::OnWeaponFire(MovementPlayer *player, const ShotRecord &shot)
	{
		if (!IsEligibleHuman(player) || shot.playerIndex != player->index)
		{
			return;
		}
		auto &data = playerData[player->index];
		auto command = std::find_if(data.commands.rbegin(), data.commands.rend(),
									[&](const AntiAimCommand &candidate)
									{
										return candidate.commandNumber == shot.commandNumber && candidate.attack && candidate.simulated
											   && candidate.serverTick == shot.serverTick;
									});
		if (command == data.commands.rend())
		{
			ANTIAIM_DEBUG("%s correlated fire did not match its captured AntiAim command.\n", player->GetName());
			return;
		}
		data.pendingShot = shot.commandNumber;
		data.pendingShotTick = shot.serverTick;
		ANTIAIM_DEBUG("%s weapon_fire matched command %d.\n", player->GetName(), shot.commandNumber);
		EvaluatePendingShot(player, data, shot.fireTick);
	}

	void AntiAimModule::OnGameEvent(IGameEvent *event, MovementPlayer *player)
	{
		if (!event)
		{
			return;
		}
		if (CS2AC_STREQ(event->GetName(), "player_spawn") && player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}

	void AntiAimModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}
} // namespace detection
