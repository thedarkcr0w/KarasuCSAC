#include "detection/detection_system.h"

#include "inetchannelinfo.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/usercmd.h"
#include "utils/interfaces.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float maximumPingMilliseconds = 150.0f;
	constexpr float maximumJitterMilliseconds = 25.0f;
	constexpr float maximumLoss = 0.02f;
	constexpr float maximumChoke = 0.02f;
	constexpr auto networkWindow = std::chrono::seconds(5);
	constexpr auto networkSampleInterval = std::chrono::milliseconds(100);

	constexpr bool ShouldVeto(float ping, float jitter, float incomingLoss, float outgoingLoss, float incomingChoke, float outgoingChoke,
							  int unavailableSamples)
	{
		return ping >= maximumPingMilliseconds || jitter >= maximumJitterMilliseconds || incomingLoss >= maximumLoss || outgoingLoss >= maximumLoss
			   || incomingChoke >= maximumChoke || outgoingChoke >= maximumChoke || unavailableSamples > 0;
	}

	static_assert(!ShouldVeto(149.9f, 24.9f, 0.019f, 0.019f, 0.019f, 0.019f, 0));
	static_assert(ShouldVeto(150.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0));
	static_assert(ShouldVeto(0.0f, 25.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0));
	static_assert(ShouldVeto(0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0));
	static_assert(ShouldVeto(0.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0));
	static_assert(ShouldVeto(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1));

	detection::NetworkSafetySample CaptureSample(MovementPlayer *player, detection::Clock::time_point now)
	{
		detection::NetworkSafetySample sample;
		sample.time = now;
		if (!player || !interfaces::pEngine)
		{
			return sample;
		}

		auto *network = interfaces::pEngine->GetPlayerNetInfo(player->GetPlayerSlot());
		if (!network)
		{
			return sample;
		}

		sample.pingMilliseconds = network->GetEngineLatency() * 1000.0f;
		sample.incomingLoss = network->GetAvgLoss(FLOW_INCOMING);
		sample.outgoingLoss = network->GetAvgLoss(FLOW_OUTGOING);
		sample.incomingChoke = network->GetAvgChoke(FLOW_INCOMING);
		sample.outgoingChoke = network->GetAvgChoke(FLOW_OUTGOING);
		sample.valid = std::isfinite(sample.pingMilliseconds) && sample.pingMilliseconds >= 0.0f && std::isfinite(sample.incomingLoss)
					   && sample.incomingLoss >= 0.0f && sample.incomingLoss <= 1.0f && std::isfinite(sample.outgoingLoss)
					   && sample.outgoingLoss >= 0.0f && sample.outgoingLoss <= 1.0f && std::isfinite(sample.incomingChoke)
					   && sample.incomingChoke >= 0.0f && sample.incomingChoke <= 1.0f && std::isfinite(sample.outgoingChoke)
					   && sample.outgoingChoke >= 0.0f && sample.outgoingChoke <= 1.0f;
		return sample;
	}
} // namespace

namespace detection
{
	localization::Text AddNetworkSafetyDetails(localization::Text details, const NetworkSafetyEvidence &evidence)
	{
		const auto network = localization::Format(
			"evidence.network_check",
			"The five-second network check was not stable enough for punishment. It measured {ping} ms ping, {jitter} ms jitter, "
			"{incoming_loss}/{outgoing_loss}% incoming/outgoing loss, {incoming_choke}/{outgoing_choke}% incoming/outgoing choke, "
			"{gaps} command gaps, and {unavailable} unavailable samples.",
			{{"ping", tfm::format("%.1f", evidence.pingMilliseconds)},
			 {"jitter", tfm::format("%.1f", evidence.jitterMilliseconds)},
			 {"incoming_loss", tfm::format("%.1f", evidence.incomingLoss * 100.0f)},
			 {"outgoing_loss", tfm::format("%.1f", evidence.outgoingLoss * 100.0f)},
			 {"incoming_choke", tfm::format("%.1f", evidence.incomingChoke * 100.0f)},
			 {"outgoing_choke", tfm::format("%.1f", evidence.outgoingChoke * 100.0f)},
			 {"gaps", tfm::format("%d", evidence.commandGaps)},
			 {"unavailable", tfm::format("%d", evidence.unavailableSamples)}});
		return {details.english + " " + network.english, details.localized + " " + network.localized};
	}

	void NetworkSafetyMonitor::Reset()
	{
		playerData = {};
	}

	void NetworkSafetyMonitor::Prune(NetworkSafetyPlayerData &data, Clock::time_point now)
	{
		const auto cutoff = now - networkWindow;
		while (!data.samples.empty() && data.samples.front().time < cutoff)
		{
			data.samples.pop_front();
		}
		while (!data.commandGaps.empty() && data.commandGaps.front() < cutoff)
		{
			data.commandGaps.pop_front();
		}
	}

	void NetworkSafetyMonitor::OnGameFrame()
	{
		if (!g_pCS2ACPlayerManager)
		{
			return;
		}

		const auto now = Clock::now();
		for (u32 index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *player = g_pCS2ACPlayerManager->ToPlayer(index);
			if (!player || !player->IsConnected() || !player->IsInGame() || !IsEligibleHuman(player))
			{
				continue;
			}

			auto &data = playerData[index];
			if (data.nextSample != Clock::time_point {} && now < data.nextSample)
			{
				continue;
			}
			data.nextSample = now + networkSampleInterval;
			data.samples.push_back(CaptureSample(player, now));
			while (data.samples.size() > 64)
			{
				data.samples.pop_front();
			}
			Prune(data, now);
		}
	}

	void NetworkSafetyMonitor::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
	{
		if (!IsEligibleHuman(player) || !commands || numCommands <= 0)
		{
			return;
		}

		auto &data = playerData[player->index];
		const auto now = Clock::now();
		Prune(data, now);
		bool discontinuity = false;
		for (int index = 0; index < numCommands; ++index)
		{
			auto &command = commands[index];
			if (command.cmdNum <= data.lastCommandNumber)
			{
				discontinuity = discontinuity || command.cmdNum < data.lastCommandNumber;
				continue;
			}

			const int clientTick = command.has_base() ? command.base().client_tick() : -1;
			discontinuity =
				discontinuity || (data.lastCommandNumber >= 0 && command.cmdNum > data.lastCommandNumber + 1)
				|| (clientTick >= 0 && data.lastClientTick >= 0 && (clientTick > data.lastClientTick + 1 || clientTick < data.lastClientTick));
			data.lastCommandNumber = command.cmdNum;
			if (clientTick >= 0)
			{
				data.lastClientTick = clientTick;
			}
		}
		if (discontinuity)
		{
			data.commandGaps.push_back(now);
			while (data.commandGaps.size() > 512)
			{
				data.commandGaps.pop_front();
			}
		}
	}

	NetworkSafetyEvidence NetworkSafetyMonitor::Evaluate(MovementPlayer *player)
	{
		NetworkSafetyEvidence evidence;
		if (!player || player->index < 1 || player->index > MAXPLAYERS)
		{
			evidence.unavailableSamples = 1;
			evidence.vetoed = true;
			return evidence;
		}

		auto &data = playerData[player->index];
		Prune(data, Clock::now());
		evidence.commandGaps = static_cast<int>(data.commandGaps.size());

		float previousPing = 0.0f;
		float totalVariation = 0.0f;
		int validSamples = 0;
		int variationSamples = 0;
		for (const auto &sample : data.samples)
		{
			if (!sample.valid)
			{
				++evidence.unavailableSamples;
				continue;
			}

			evidence.pingMilliseconds = (std::max)(evidence.pingMilliseconds, sample.pingMilliseconds);
			evidence.incomingLoss = (std::max)(evidence.incomingLoss, sample.incomingLoss);
			evidence.outgoingLoss = (std::max)(evidence.outgoingLoss, sample.outgoingLoss);
			evidence.incomingChoke = (std::max)(evidence.incomingChoke, sample.incomingChoke);
			evidence.outgoingChoke = (std::max)(evidence.outgoingChoke, sample.outgoingChoke);
			if (validSamples > 0)
			{
				totalVariation += std::fabs(sample.pingMilliseconds - previousPing);
				++variationSamples;
			}
			previousPing = sample.pingMilliseconds;
			++validSamples;
		}
		if (validSamples < 2 && evidence.unavailableSamples == 0)
		{
			evidence.unavailableSamples = 1;
		}
		if (variationSamples > 0)
		{
			evidence.jitterMilliseconds = totalVariation / variationSamples;
		}

		// ProcessUsercmds may batch or replay commands, so command discontinuities are useful diagnostics but not proof of a bad connection.
		evidence.vetoed = ShouldVeto(evidence.pingMilliseconds, evidence.jitterMilliseconds, evidence.incomingLoss, evidence.outgoingLoss,
									 evidence.incomingChoke, evidence.outgoingChoke, evidence.unavailableSamples);
		return evidence;
	}

	void NetworkSafetyMonitor::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}
} // namespace detection
