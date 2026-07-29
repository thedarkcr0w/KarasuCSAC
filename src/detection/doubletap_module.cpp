#include "detection/detection_system.h"

#include "inetchannelinfo.h"
#include "igameevents.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/usercmd.h"
#include "utils/interfaces.h"

#include <algorithm>
#include <cmath>

CConVar<bool> cs2ac_doubletap_debug("cs2ac_doubletap_debug", FCVAR_NONE, "Show Doubletap tick spacing and network safety checks", false);

namespace
{
	constexpr int detectionThreshold = 3;
	constexpr float maximumPingMilliseconds = 150.0f;
	constexpr float maximumJitterMilliseconds = 25.0f;
	constexpr float maximumLoss = 0.02f;
	constexpr float maximumChoke = 0.02f;
	constexpr auto networkWindow = std::chrono::seconds(5);
	constexpr auto networkSampleInterval = std::chrono::milliseconds(100);

	constexpr bool ShouldVetoNetwork(float ping, float jitter, float incomingLoss, float outgoingLoss, float incomingChoke, float outgoingChoke,
									 int commandGaps, int unavailableSamples)
	{
		return ping >= maximumPingMilliseconds || jitter >= maximumJitterMilliseconds || incomingLoss >= maximumLoss || outgoingLoss >= maximumLoss
			   || incomingChoke >= maximumChoke || outgoingChoke >= maximumChoke || commandGaps > 0 || unavailableSamples > 0;
	}

	static_assert(!ShouldVetoNetwork(149.9f, 24.9f, 0.019f, 0.019f, 0.019f, 0.019f, 0, 0));
	static_assert(ShouldVetoNetwork(150.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0));
	static_assert(ShouldVetoNetwork(0.0f, 25.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0));
	static_assert(ShouldVetoNetwork(0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0, 0));
	static_assert(ShouldVetoNetwork(0.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0, 0));
	static_assert(ShouldVetoNetwork(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1, 0));
	static_assert(ShouldVetoNetwork(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1));

	void PruneNetworkHistory(detection::DoubletapState &state, detection::Clock::time_point now)
	{
		const auto cutoff = now - networkWindow;
		while (!state.networkSamples.empty() && state.networkSamples.front().time < cutoff)
		{
			state.networkSamples.pop_front();
		}
		while (!state.commandGaps.empty() && state.commandGaps.front() < cutoff)
		{
			state.commandGaps.pop_front();
		}
	}

	detection::DoubletapState::NetworkSample CaptureNetworkSample(MovementPlayer *player, detection::Clock::time_point now)
	{
		detection::DoubletapState::NetworkSample sample;
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

	detection::DoubletapState::NetworkEvidence EvaluateNetwork(detection::DoubletapState &state, detection::Clock::time_point now)
	{
		PruneNetworkHistory(state, now);
		detection::DoubletapState::NetworkEvidence evidence;
		evidence.commandGaps = static_cast<int>(state.commandGaps.size());

		float previousPing = 0.0f;
		float totalVariation = 0.0f;
		int validSamples = 0;
		int variationSamples = 0;
		for (const auto &sample : state.networkSamples)
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

		evidence.vetoed = ShouldVetoNetwork(evidence.pingMilliseconds, evidence.jitterMilliseconds, evidence.incomingLoss, evidence.outgoingLoss,
											evidence.incomingChoke, evidence.outgoingChoke, evidence.commandGaps, evidence.unavailableSamples);
		return evidence;
	}

	void MergeNetworkEvidence(detection::DoubletapState::NetworkEvidence &combined, const detection::DoubletapState::NetworkEvidence &current)
	{
		combined.pingMilliseconds = (std::max)(combined.pingMilliseconds, current.pingMilliseconds);
		combined.jitterMilliseconds = (std::max)(combined.jitterMilliseconds, current.jitterMilliseconds);
		combined.incomingLoss = (std::max)(combined.incomingLoss, current.incomingLoss);
		combined.outgoingLoss = (std::max)(combined.outgoingLoss, current.outgoingLoss);
		combined.incomingChoke = (std::max)(combined.incomingChoke, current.incomingChoke);
		combined.outgoingChoke = (std::max)(combined.outgoingChoke, current.outgoingChoke);
		combined.commandGaps = (std::max)(combined.commandGaps, current.commandGaps);
		combined.unavailableSamples = (std::max)(combined.unavailableSamples, current.unavailableSamples);
		combined.vetoed = combined.vetoed || current.vetoed;
	}
} // namespace

#define DOUBLETAP_DEBUG(...) \
	do \
	{ \
		if (cs2ac_doubletap_debug.GetBool()) \
			Msg("[CS2AC Doubletap] " __VA_ARGS__); \
	} while (0)

namespace detection
{
	void DoubletapModule::Load(AnnounceCallback announceCallback, AnnounceCallback networkVetoCallback)
	{
		announce = announceCallback;
		announceNetworkVeto = networkVetoCallback;
	}

	void DoubletapModule::Unload()
	{
		Reset();
		announce = nullptr;
		announceNetworkVeto = nullptr;
	}

	void DoubletapModule::Reset()
	{
		playerData = {};
	}

	void DoubletapModule::OnGameFrame()
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

			auto &state = playerData[index];
			if (state.nextNetworkSample != Clock::time_point {} && now < state.nextNetworkSample)
			{
				continue;
			}
			state.nextNetworkSample = now + networkSampleInterval;
			state.networkSamples.push_back(CaptureNetworkSample(player, now));
			while (state.networkSamples.size() > 64)
			{
				state.networkSamples.pop_front();
			}
			PruneNetworkHistory(state, now);
		}
	}

	void DoubletapModule::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
	{
		if (!IsEligibleHuman(player) || !commands || numCommands <= 0)
		{
			return;
		}

		auto &state = playerData[player->index];
		const auto now = Clock::now();
		PruneNetworkHistory(state, now);
		bool discontinuity = false;
		for (int index = 0; index < numCommands; ++index)
		{
			auto &command = commands[index];
			if (command.cmdNum <= state.lastCommandNumber)
			{
				discontinuity = discontinuity || command.cmdNum < state.lastCommandNumber;
				continue;
			}

			const int clientTick = command.has_base() ? command.base().client_tick() : -1;
			discontinuity =
				discontinuity || (state.lastCommandNumber >= 0 && command.cmdNum > state.lastCommandNumber + 1)
				|| (clientTick >= 0 && state.lastClientTick >= 0 && (clientTick > state.lastClientTick + 1 || clientTick < state.lastClientTick));
			state.lastCommandNumber = command.cmdNum;
			if (clientTick >= 0)
			{
				state.lastClientTick = clientTick;
			}
		}
		if (discontinuity)
		{
			state.commandGaps.push_back(now);
			while (state.commandGaps.size() > 512)
			{
				state.commandGaps.pop_front();
			}
			DOUBLETAP_DEBUG("%s had a command discontinuity inside the network safety window.\n", player->GetName());
		}
	}

	void DoubletapModule::OnWeaponFire(IGameEvent *event, MovementPlayer *player, int currentTick)
	{
		if (!event || !IsEligibleHuman(player))
		{
			return;
		}

		auto &previous = playerData[player->index];
		const std::string_view weapon = NormalizeWeapon(event->GetString("weapon", ""));
		if (!IsBallisticWeapon(weapon))
		{
			previous.serverTick = -1;
			previous.weapon.clear();
			return;
		}
		if (previous.serverTick < 0)
		{
			DOUBLETAP_DEBUG("%s stored the first %.*s fire at server tick %d.\n", player->GetName(), static_cast<int>(weapon.size()), weapon.data(),
							currentTick);
			previous.serverTick = currentTick;
			previous.weapon.assign(weapon);
			return;
		}

		const std::int64_t delta = static_cast<std::int64_t>(currentTick) - previous.serverTick;
		if (delta < 0 || delta > 1 || previous.weapon != weapon)
		{
			DOUBLETAP_DEBUG("%s fired %.*s %lld server ticks after %s. Rejected.\n", player->GetName(), static_cast<int>(weapon.size()),
							weapon.data(), static_cast<long long>(delta), previous.weapon.c_str());
			previous.serverTick = currentTick;
			previous.weapon.assign(weapon);
			return;
		}

		previous.serverTick = currentTick;
		const int incidents = ++previous.incidents;
		const auto network = EvaluateNetwork(previous, Clock::now());
		MergeNetworkEvidence(previous.networkEvidence, network);
		DOUBLETAP_DEBUG("%s matched pair %d/%d at %lld server tick%s apart.\n", player->GetName(), incidents, detectionThreshold,
						static_cast<long long>(delta), delta == 1 ? "" : "s");
		if (network.vetoed)
		{
			DOUBLETAP_DEBUG(
				"%s's pair crossed a network safety gate: %.1f ms ping, %.1f ms jitter, %.1f/%.1f%% loss, %.1f/%.1f%% choke, %d command gaps, "
				"%d unavailable samples.\n",
				player->GetName(), network.pingMilliseconds, network.jitterMilliseconds, network.incomingLoss * 100.0f, network.outgoingLoss * 100.0f,
				network.incomingChoke * 100.0f, network.outgoingChoke * 100.0f, network.commandGaps, network.unavailableSamples);
		}
		if (incidents < detectionThreshold)
		{
			return;
		}
		previous.incidents = 0;

		const auto networkEvidence = previous.networkEvidence;
		previous.networkEvidence = {};
		if (networkEvidence.vetoed)
		{
			if (announceNetworkVeto)
			{
				announceNetworkVeto(
					"DOUBLETAP", player,
					localization::Format(
						"evidence.doubletap.network_unstable",
						"Three rapid-fire pairs were detected; the latest was {ticks} server ticks apart. Network check: {ping} ms ping, "
						"{jitter} ms jitter, {incoming_loss}/{outgoing_loss}% incoming/outgoing loss, "
						"{incoming_choke}/{outgoing_choke}% incoming/outgoing choke, {gaps} command gaps, and {unavailable} unavailable samples.",
						{{"ticks", tfm::format("%lld", static_cast<long long>(delta))},
						 {"ping", tfm::format("%.1f", networkEvidence.pingMilliseconds)},
						 {"jitter", tfm::format("%.1f", networkEvidence.jitterMilliseconds)},
						 {"incoming_loss", tfm::format("%.1f", networkEvidence.incomingLoss * 100.0f)},
						 {"outgoing_loss", tfm::format("%.1f", networkEvidence.outgoingLoss * 100.0f)},
						 {"incoming_choke", tfm::format("%.1f", networkEvidence.incomingChoke * 100.0f)},
						 {"outgoing_choke", tfm::format("%.1f", networkEvidence.outgoingChoke * 100.0f)},
						 {"gaps", tfm::format("%d", networkEvidence.commandGaps)},
						 {"unavailable", tfm::format("%d", networkEvidence.unavailableSamples)}}));
			}
			return;
		}

		if (announce)
		{
			announce("DOUBLETAP", player,
					 localization::Format(delta == 1 ? "evidence.doubletap.one_tick" : "evidence.doubletap.zero_ticks",
										  delta == 1 ? "Three rapid-fire pairs reached the threshold; the latest pair was 1 server tick apart."
													 : "Three rapid-fire pairs reached the threshold; the latest pair was 0 server ticks apart."));
		}
	}

	void DoubletapModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}
} // namespace detection
