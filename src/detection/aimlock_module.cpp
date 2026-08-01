#include "detection/detection_system.h"

#include "inetchannelinfo.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/usercmd.h"
#include "utils/interfaces.h"
#include "utils/utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

CConVar<bool> cs2ac_aimlock_debug("cs2ac_aimlock_debug", FCVAR_NONE, "Show Aimlock tracking episodes and evidence", false);

#define AIMLOCK_DEBUG(...) \
	do \
	{ \
		if (cs2ac_aimlock_debug.GetBool()) \
			Msg("[CS2AC Aimlock] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr int trackingTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE * 2.0f);
	constexpr int rearmTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE * 0.5f);
	constexpr int lagSearchRadius = 2;
	constexpr float playerHalfWidth = 16.0f; // The CS2 player hull is 32 units wide; aim is measured from its center.
	constexpr float minimumDistance = 200.0f;
	constexpr float minimumTargetTravel = 128.0f; // Four player widths, converted to degrees at the episode's starting distance.
	constexpr float maximumInterpolationTicks = 19.0f;
	constexpr int detectionThreshold = 3;
	constexpr auto evidenceWindow = std::chrono::minutes(5);
	constexpr float bodyHeights[] = {8.0f, 46.0f, 64.0f};

	constexpr bool MeetsCoverage(int onTarget, int samples)
	{
		return samples > 0 && onTarget * 20 >= samples * 19;
	}

	static_assert(MeetsCoverage(123, 129));
	static_assert(!MeetsCoverage(122, 129));
	static_assert(std::tuple_size<decltype(detection::AimlockTrack::hypotheses)>::value == lagSearchRadius * 2 + 1);

	struct Candidate
	{
		QAngle bearing;
		float error {180.0f};
		int targetIndex {-1};
		int bodyPoint {-1};
		int lagTicks {};
		bool valid {};
	};

	struct LagEstimate
	{
		int ticks {};
		float roundTripMilliseconds {};
		float interpolationTicks {};
		bool valid {};
	};

	QAngle Bearing(const Vector &eye, const Vector &target)
	{
		const Vector delta = target - eye;
		constexpr float radiansToDegrees = static_cast<float>(180.0 / M_PI);
		const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
		return {-std::atan2(delta.z, horizontal) * radiansToDegrees, std::atan2(delta.y, delta.x) * radiansToDegrees, 0.0f};
	}

	void ClearTrack(detection::AimlockPlayerData &data)
	{
		data.track = {};
	}

	void ClearRuntime(detection::AimlockPlayerData &data)
	{
		data = {};
	}

	LagEstimate EstimateVisualLag(MovementPlayer *player)
	{
		LagEstimate estimate;
		if (!player || !interfaces::pEngine)
		{
			return estimate;
		}

		INetChannelInfo *netChannel = interfaces::pEngine->GetPlayerNetInfo(player->GetPlayerSlot());
		const char *interpolationValue = interfaces::pEngine->GetClientConVarValue(player->GetPlayerSlot(), "cl_interp_ratio");
		if (!netChannel || !utils::IsNumeric(interpolationValue))
		{
			return estimate;
		}

		const float roundTripSeconds = netChannel->GetEngineLatency();
		float interpolationTicks = static_cast<float>(std::strtod(interpolationValue, nullptr));
		if (!std::isfinite(roundTripSeconds) || roundTripSeconds < 0.0f || roundTripSeconds > 2.0f || !std::isfinite(interpolationTicks)
			|| interpolationTicks < 0.0f || interpolationTicks > maximumInterpolationTicks)
		{
			return estimate;
		}
		if (interpolationTicks == 0.0f)
		{
			interpolationTicks = 1.0f;
		}

		// The target snapshot travelled to the client before this aim command travelled back to the server, so use the full RTT.
		estimate.ticks = (std::max)(0, static_cast<int>(std::lround(roundTripSeconds * ENGINE_FIXED_TICK_RATE + interpolationTicks)));
		estimate.roundTripMilliseconds = roundTripSeconds * 1000.0f;
		estimate.interpolationTicks = interpolationTicks;
		estimate.valid = true;
		return estimate;
	}

	const detection::AimlockLagHypothesis *BestHypothesis(const detection::AimlockTrack &track, bool requireEvidence)
	{
		const detection::AimlockLagHypothesis *best = nullptr;
		for (int index = 0; index < track.hypothesisCount; ++index)
		{
			const auto &hypothesis = track.hypotheses[index];
			if (!hypothesis.valid
				|| (requireEvidence
					&& (!MeetsCoverage(hypothesis.onTargetSamples, track.samples)
						|| hypothesis.maximumTargetDisplacement < hypothesis.requiredTargetDisplacement)))
			{
				continue;
			}
			if (!best || hypothesis.onTargetSamples > best->onTargetSamples)
			{
				best = &hypothesis;
			}
		}
		return best;
	}

	bool MeasureTarget(const detection::AimlockSample &sample, const detection::TrackedPosition &target, int bodyPoint, const Vector &aimForward,
					   QAngle &bearing, float &error, float &maximumError, float *requiredTargetDisplacement)
	{
		if (bodyPoint < 0 || bodyPoint >= static_cast<int>(std::size(bodyHeights)))
		{
			return false;
		}

		const Vector targetPoint = target.origin + Vector(0.0f, 0.0f, bodyHeights[bodyPoint]);
		Vector direction = targetPoint - sample.eyePosition;
		const float targetDistance = direction.Length();
		if (!detection::IsFinite(targetPoint) || !detection::IsFinite(direction) || !std::isfinite(targetDistance) || targetDistance < EPSILON)
		{
			return false;
		}
		direction.NormalizeInPlace();
		const float dot = std::clamp(DotProduct(aimForward, direction), -1.0f, 1.0f);
		error = static_cast<float>(std::acos(dot) * (180.0 / M_PI));
		maximumError = static_cast<float>(std::atan2(playerHalfWidth, targetDistance) * (180.0 / M_PI));
		if (requiredTargetDisplacement)
		{
			*requiredTargetDisplacement = static_cast<float>(std::atan2(minimumTargetTravel, targetDistance) * (180.0 / M_PI));
		}
		bearing = Bearing(sample.eyePosition, targetPoint);
		return std::isfinite(error) && std::isfinite(maximumError) && (!requiredTargetDisplacement || std::isfinite(*requiredTargetDisplacement))
			   && detection::IsFinite(bearing);
	}

	bool EvaluateTarget(const detection::AimlockSample &sample, const detection::PositionFrame &currentFrame,
						const detection::PositionFrame &historicalFrame, int observerIndex, int targetIndex, int bodyPoint, const Vector &aimForward,
						QAngle &bearing, float &error, float &maximumError, float *requiredTargetDisplacement)
	{
		if (observerIndex < 1 || observerIndex > MAXPLAYERS || targetIndex < 1 || targetIndex > MAXPLAYERS || !detection::IsFinite(sample.eyePosition)
			|| !detection::IsFinite(sample.angles))
		{
			return false;
		}

		const auto &observer = currentFrame.players[observerIndex];
		const auto &currentTarget = currentFrame.players[targetIndex];
		const auto &target = historicalFrame.players[targetIndex];
		if (!observer.valid || !observer.alive || observer.teleported || !currentTarget.valid || !currentTarget.alive || currentTarget.teleported
			|| !detection::AreOpponents(observer.team, currentTarget.team) || !target.valid || !target.alive || target.teleported
			|| !detection::AreOpponents(observer.team, target.team) || (target.origin - sample.eyePosition).Length() < minimumDistance)
		{
			return false;
		}

		return MeasureTarget(sample, target, bodyPoint, aimForward, bearing, error, maximumError, requiredTargetDisplacement);
	}

	Candidate FindCandidate(const detection::ShotCorrelator *shots, const detection::AimlockSample &sample,
							const detection::PositionFrame &currentFrame, int observerIndex, const LagEstimate &estimate)
	{
		Candidate best;
		if (!shots || !estimate.valid || observerIndex < 1 || observerIndex > MAXPLAYERS || !detection::IsFinite(sample.eyePosition)
			|| !detection::IsFinite(sample.angles))
		{
			return best;
		}

		const auto &observer = currentFrame.players[observerIndex];
		if (!observer.valid || !observer.alive || observer.teleported)
		{
			return best;
		}
		const Vector aimForward = detection::AimForward(sample.angles);
		int matchedTarget = -1;
		bool ambiguous = false;
		const int firstLag = (std::max)(0, estimate.ticks - lagSearchRadius);
		const int lastLag = estimate.ticks + lagSearchRadius;
		for (int lagTicks = firstLag; lagTicks <= lastLag; ++lagTicks)
		{
			const detection::PositionFrame *historicalFrame = shots->FindFrame(sample.serverTick - lagTicks);
			if (!historicalFrame)
			{
				continue;
			}
			for (int targetIndex = 1; targetIndex <= MAXPLAYERS; ++targetIndex)
			{
				if (targetIndex == observerIndex)
				{
					continue;
				}
				const auto &currentTarget = currentFrame.players[targetIndex];
				const auto &target = historicalFrame->players[targetIndex];
				if (!currentTarget.valid || !currentTarget.alive || currentTarget.teleported
					|| !detection::AreOpponents(observer.team, currentTarget.team) || !target.valid || !target.alive || target.teleported
					|| !detection::AreOpponents(observer.team, target.team) || (target.origin - sample.eyePosition).Length() < minimumDistance)
				{
					continue;
				}
				for (int bodyPoint = 0; bodyPoint < static_cast<int>(std::size(bodyHeights)); ++bodyPoint)
				{
					QAngle bearing;
					float error = 180.0f;
					float maximumError = 0.0f;
					if (!MeasureTarget(sample, target, bodyPoint, aimForward, bearing, error, maximumError, nullptr) || error > maximumError)
					{
						continue;
					}

					if (matchedTarget < 0)
					{
						matchedTarget = targetIndex;
					}
					else if (matchedTarget != targetIndex)
					{
						ambiguous = true;
					}
					if (error < best.error)
					{
						best = {bearing, error, targetIndex, bodyPoint, lagTicks, true};
					}
				}
			}
		}

		best.valid = best.valid && !ambiguous;
		return best;
	}
} // namespace

namespace detection
{
	void AimlockModule::Load(AnnounceCallback announceCallback, ShotCorrelator *shotCorrelator)
	{
		announce = announceCallback;
		shots = shotCorrelator;
	}

	void AimlockModule::Unload()
	{
		Reset();
		shots = nullptr;
		announce = nullptr;
	}

	void AimlockModule::Reset()
	{
		playerData = {};
		evidence = {};
	}

	void AimlockModule::OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick)
	{
		if (!IsEligibleHuman(player) || !command)
		{
			return;
		}

		auto &data = playerData[player->index];
		if (currentTick == data.lastProcessedTick)
		{
			return;
		}
		if (!command->has_base() || !command->base().has_viewangles())
		{
			ClearTrack(data);
			data.pending = {};
			return;
		}

		const auto &view = command->base().viewangles();
		const QAngle angles(view.x(), view.y(), view.z());
		Vector eye;
		player->GetEyeOrigin(&eye);
		if (!IsFinite(angles) || !IsFinite(eye))
		{
			ClearTrack(data);
			data.pending = {};
			return;
		}

		// SetupMove is the authoritative gate: this is the last command the server will actually simulate for this tick.
		data.pending = {currentTick, angles, eye, true};
	}

	void AimlockModule::OnGameFrame(int currentTick)
	{
		if (!shots)
		{
			return;
		}

		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto &data = playerData[index];
			auto *player = g_pCS2ACPlayerManager ? g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)) : nullptr;
			if (!IsEligibleHuman(player) || !player->IsAlive())
			{
				ClearRuntime(data);
				continue;
			}
			if (!data.pending.valid)
			{
				continue;
			}

			const AimlockSample sample = data.pending;
			data.pending = {};
			if (sample.serverTick != currentTick || sample.serverTick == data.lastProcessedTick || !shots->FindFrame(sample.serverTick))
			{
				AIMLOCK_DEBUG("%s tracking reset because the simulated command and world snapshot did not share one new tick.\n", player->GetName());
				ClearTrack(data);
				continue;
			}
			data.lastProcessedTick = sample.serverTick;
			Evaluate(player, data, sample);
		}
	}

	void AimlockModule::Evaluate(MovementPlayer *player, AimlockPlayerData &data, const AimlockSample &sample)
	{
		const PositionFrame *currentFrame = shots ? shots->FindFrame(sample.serverTick) : nullptr;
		if (!currentFrame || !IsFinite(sample.angles) || !IsFinite(sample.eyePosition))
		{
			ClearTrack(data);
			return;
		}
		const Vector aimForward = AimForward(sample.angles);

		if (data.latched)
		{
			bool stillLocked = false;
			const LagEstimate estimate = EstimateVisualLag(player);
			if (estimate.valid)
			{
				const int firstLag = (std::max)(0, estimate.ticks - lagSearchRadius);
				const int lastLag = estimate.ticks + lagSearchRadius;
				for (int lagTicks = firstLag; lagTicks <= lastLag; ++lagTicks)
				{
					const PositionFrame *historicalFrame = shots->FindFrame(sample.serverTick - lagTicks);
					if (!historicalFrame)
					{
						continue;
					}
					QAngle bearing;
					float error = 180.0f;
					float maximumError = 0.0f;
					if (EvaluateTarget(sample, *currentFrame, *historicalFrame, player->index, data.latchedTarget, data.latchedBodyPoint, aimForward,
									   bearing, error, maximumError, nullptr)
						&& error <= maximumError)
					{
						stillLocked = true;
						break;
					}
				}
			}
			if (stillLocked)
			{
				data.breakStartTick = -1;
				return;
			}
			if (data.breakStartTick < 0)
			{
				data.breakStartTick = sample.serverTick;
			}
			if (static_cast<std::int64_t>(sample.serverTick) - data.breakStartTick < rearmTicks)
			{
				return;
			}
			data.latched = false;
			data.latchedTarget = -1;
			data.latchedBodyPoint = -1;
			data.breakStartTick = -1;
			AIMLOCK_DEBUG("%s rearmed after a half-second break.\n", player->GetName());
		}

		auto startTrack = [&]()
		{
			const LagEstimate estimate = EstimateVisualLag(player);
			const Candidate candidate = FindCandidate(shots, sample, *currentFrame, player->index, estimate);
			if (!candidate.valid)
			{
				return;
			}

			data.track = {};
			data.track.targetIndex = candidate.targetIndex;
			data.track.bodyPoint = candidate.bodyPoint;
			data.track.startServerTick = sample.serverTick;
			data.track.lastServerTick = sample.serverTick;
			data.track.samples = 1;
			const int firstLag = (std::max)(0, estimate.ticks - lagSearchRadius);
			const int lastLag = estimate.ticks + lagSearchRadius;
			for (int lagTicks = firstLag; lagTicks <= lastLag && data.track.hypothesisCount < static_cast<int>(data.track.hypotheses.size());
				 ++lagTicks)
			{
				const PositionFrame *historicalFrame = shots->FindFrame(sample.serverTick - lagTicks);
				if (!historicalFrame)
				{
					continue;
				}
				QAngle bearing;
				float error = 180.0f;
				float maximumError = 0.0f;
				float requiredTargetDisplacement = 0.0f;
				if (!EvaluateTarget(sample, *currentFrame, *historicalFrame, player->index, candidate.targetIndex, candidate.bodyPoint, aimForward,
									bearing, error, maximumError, &requiredTargetDisplacement))
				{
					continue;
				}
				data.track.hypotheses[data.track.hypothesisCount++] = {bearing, 0.0f, requiredTargetDisplacement, lagTicks, error <= maximumError,
																	   true};
			}
			if (data.track.hypothesisCount == 0)
			{
				ClearTrack(data);
				return;
			}
			AIMLOCK_DEBUG("%s started tracking target %d body point %d across %d fixed visual-delay candidates (%d-%d ticks) "
						  "at %.3f degrees "
						  "(RTT %.1f ms, interpolation %.1f ticks).\n",
						  player->GetName(), candidate.targetIndex, candidate.bodyPoint, data.track.hypothesisCount,
						  data.track.hypotheses.front().lagTicks, data.track.hypotheses[data.track.hypothesisCount - 1].lagTicks, candidate.error,
						  estimate.roundTripMilliseconds, estimate.interpolationTicks);
		};

		if (data.track.targetIndex < 0)
		{
			startTrack();
			return;
		}

		const std::int64_t serverTickDelta = static_cast<std::int64_t>(sample.serverTick) - data.track.lastServerTick;
		if (serverTickDelta != 1)
		{
			AIMLOCK_DEBUG("%s tracking reset because the simulated server ticks were not consecutive (%lld).\n", player->GetName(),
						  static_cast<long long>(serverTickDelta));
			ClearTrack(data);
			startTrack();
			return;
		}

		++data.track.samples;
		int validHypotheses = 0;
		for (int index = 0; index < data.track.hypothesisCount; ++index)
		{
			auto &hypothesis = data.track.hypotheses[index];
			if (!hypothesis.valid)
			{
				continue;
			}

			QAngle bearing;
			float error = 180.0f;
			float maximumError = 0.0f;
			const PositionFrame *historicalFrame = shots->FindFrame(sample.serverTick - hypothesis.lagTicks);
			if (!historicalFrame
				|| !EvaluateTarget(sample, *currentFrame, *historicalFrame, player->index, data.track.targetIndex, data.track.bodyPoint, aimForward,
								   bearing, error, maximumError, nullptr))
			{
				hypothesis.valid = false;
				continue;
			}
			const float displacement = AngularDistance(hypothesis.startBearing, bearing);
			if (!std::isfinite(displacement))
			{
				hypothesis.valid = false;
				continue;
			}

			++validHypotheses;
			hypothesis.onTargetSamples += error <= maximumError;
			hypothesis.maximumTargetDisplacement = (std::max)(hypothesis.maximumTargetDisplacement, displacement);
		}
		if (validHypotheses == 0)
		{
			AIMLOCK_DEBUG("%s tracking reset because the fixed target history was no longer trustworthy.\n", player->GetName());
			ClearTrack(data);
			startTrack();
			return;
		}

		data.track.lastServerTick = sample.serverTick;

		const int elapsedTicks = sample.serverTick - data.track.startServerTick;
		if (elapsedTicks > 0 && elapsedTicks < trackingTicks && elapsedTicks % rearmTicks == 0)
		{
			const AimlockLagHypothesis *best = BestHypothesis(data.track, false);
			if (best)
			{
				AIMLOCK_DEBUG("%s episode: %.1f/2.0 seconds, best fixed delay %d ticks kept %d/%d samples within the target width; "
							  "target moved %.1f/%.1f required degrees.\n",
							  player->GetName(), static_cast<float>(elapsedTicks) / ENGINE_FIXED_TICK_RATE, best->lagTicks, best->onTargetSamples,
							  data.track.samples, best->maximumTargetDisplacement, best->requiredTargetDisplacement);
			}
		}
		if (elapsedTicks < trackingTicks)
		{
			return;
		}

		const AimlockLagHypothesis *passing = BestHypothesis(data.track, true);
		if (!passing)
		{
			const AimlockLagHypothesis *best = BestHypothesis(data.track, false);
			if (best)
			{
				AIMLOCK_DEBUG("%s episode ended without evidence: best fixed delay %d ticks kept %d/%d samples within the target width; "
							  "target moved %.1f/%.1f required degrees.\n",
							  player->GetName(), best->lagTicks, best->onTargetSamples, data.track.samples, best->maximumTargetDisplacement,
							  best->requiredTargetDisplacement);
			}
			ClearTrack(data);
			return;
		}
		AddIncident(player, data, *passing);
	}

	void AimlockModule::AddIncident(MovementPlayer *player, AimlockPlayerData &data, const AimlockLagHypothesis &hypothesis)
	{
		const auto now = Clock::now();
		auto &incidents = evidence[player->index];
		while (!incidents.empty() && now - incidents.front() >= evidenceWindow)
		{
			incidents.pop_front();
		}
		incidents.push_back(now);
		AIMLOCK_DEBUG("%s added evidence %d/%d after fixed delay %d ticks kept %d/%d precise samples and %.1f/%.1f required degrees of target "
					  "movement.\n",
					  player->GetName(), static_cast<int>(incidents.size()), detectionThreshold, hypothesis.lagTicks, hypothesis.onTargetSamples,
					  data.track.samples, hypothesis.maximumTargetDisplacement, hypothesis.requiredTargetDisplacement);
		const bool detected = incidents.size() >= detectionThreshold;
		if (detected)
		{
			if (announce)
			{
				announce(
					"AIMLOCK", player,
					localization::Format(
						"evidence.aimlock",
						"{incidents} precise tracking episodes reached the threshold; the latest stayed on target for {precise}/{samples} samples "
						"while the target moved {movement} degrees against a {required}-degree requirement.",
						{{"incidents", tfm::format("%zu", incidents.size())},
						 {"precise", tfm::format("%d", hypothesis.onTargetSamples)},
						 {"samples", tfm::format("%d", data.track.samples)},
						 {"movement", tfm::format("%.1f", hypothesis.maximumTargetDisplacement)},
						 {"required", tfm::format("%.1f", hypothesis.requiredTargetDisplacement)}}));
			}
			incidents.clear();
			data.latched = true;
			data.latchedTarget = data.track.targetIndex;
			data.latchedBodyPoint = data.track.bodyPoint;
			data.breakStartTick = -1;
		}

		ClearTrack(data);
	}

	void AimlockModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
			evidence[player->index].clear();
		}
	}
} // namespace detection
