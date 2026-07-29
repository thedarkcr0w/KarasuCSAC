#include "detection/detection_system.h"

#include "movement/movement.h"
#include "movement_analysis/player_context.h"

CConVar<bool> cs2ac_namechanger_debug("cs2ac_namechanger_debug", FCVAR_NONE, "Show Namechanger baselines, rolling counts, and evidence expiry",
									  false);

#define NAMECHANGER_DEBUG(...) \
	do \
	{ \
		if (cs2ac_namechanger_debug.GetBool()) \
			Msg("[CS2AC Namechanger] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr size_t detectionThreshold = 5;
	constexpr auto evidenceWindow = std::chrono::seconds(60);
} // namespace

namespace detection
{
	void NameChangerModule::Load(AnnounceCallback announceCallback)
	{
		announce = announceCallback;
		Reset();
	}

	void NameChangerModule::Unload()
	{
		playerData = {};
		announce = nullptr;
	}

	void NameChangerModule::Reset()
	{
		playerData = {};
		if (!g_pCS2ACPlayerManager)
		{
			return;
		}
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			OnClientReady(g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index)));
		}
	}

	void NameChangerModule::OnClientReady(MovementPlayer *player)
	{
		if (!IsEligibleHuman(player) || !player->IsInGame())
		{
			return;
		}
		auto &data = playerData[player->index];
		data = {};
		const char *name = player->GetClient()->GetClientName();
		if (name)
		{
			data.lastName = name;
			data.initialized = true;
			NAMECHANGER_DEBUG("%s baseline stored for player slot %d.\n", name, player->index);
		}
	}

	void NameChangerModule::OnClientSettingsChanged(MovementPlayer *player)
	{
		if (!IsEligibleHuman(player) || !player->IsInGame())
		{
			return;
		}
		const char *currentName = player->GetClient()->GetClientName();
		if (!currentName)
		{
			return;
		}

		auto &data = playerData[player->index];
		if (!data.initialized)
		{
			data.lastName = currentName;
			data.initialized = true;
			return;
		}
		if (data.lastName == currentName)
		{
			return;
		}
		data.lastName = currentName;

		const auto now = Clock::now();
		size_t expired = 0;
		while (!data.changes.empty() && now - data.changes.front() >= evidenceWindow)
		{
			data.changes.pop_front();
			++expired;
		}
		data.changes.push_back(now);
		NAMECHANGER_DEBUG("%s changed names: %zu of %zu changes remain in the rolling minute%s.\n", currentName, data.changes.size(),
						  detectionThreshold, expired ? tfm::format("; %zu expired", expired).c_str() : "");
		if (data.changes.size() >= detectionThreshold)
		{
			if (announce)
			{
				announce("NAMECHANGER", player,
						 localization::Format("evidence.namechanger", "{changes} visible name changes occurred within one minute.",
											  {{"changes", tfm::format("%zu", data.changes.size())}}));
			}
			data.changes.clear();
		}
	}

	void NameChangerModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
		}
	}
} // namespace detection
