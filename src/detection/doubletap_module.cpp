#include "detection/detection_system.h"

#include "igameevents.h"
#include "movement/movement.h"

#include <algorithm>

CConVar<bool> cs2ac_doubletap_debug("cs2ac_doubletap_debug", FCVAR_NONE, "Show Doubletap tick spacing and network safety checks", false);

namespace
{
	constexpr int detectionThreshold = 2;

	void MergeNetworkEvidence(detection::NetworkSafetyEvidence &combined, const detection::NetworkSafetyEvidence &current)
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
	void DoubletapModule::Load(AnnounceCallback announceCallback, AnnounceCallback networkVetoCallback, NetworkSafetyMonitor *networkSafetyMonitor)
	{
		announce = announceCallback;
		announceNetworkVeto = networkVetoCallback;
		networkSafety = networkSafetyMonitor;
	}

	void DoubletapModule::Unload()
	{
		Reset();
		announce = nullptr;
		announceNetworkVeto = nullptr;
		networkSafety = nullptr;
	}

	void DoubletapModule::Reset()
	{
		playerData = {};
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
		NetworkSafetyEvidence network;
		if (networkSafety)
		{
			network = networkSafety->Evaluate(player);
		}
		else
		{
			network.unavailableSamples = 1;
			network.vetoed = true;
		}
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
				const auto details = localization::Format("evidence.doubletap.network_unstable",
														  "Three rapid-fire pairs were detected; the latest was {ticks} server ticks apart.",
														  {{"ticks", tfm::format("%lld", static_cast<long long>(delta))}});
				announceNetworkVeto("DOUBLETAP", player, AddNetworkSafetyDetails(details, networkEvidence));
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
