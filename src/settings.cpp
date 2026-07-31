#include "settings.h"

#include "convar.h"
#include "eiface.h"
#include "utils/interfaces.h"

#include <algorithm>
#include <charconv>
#include <new>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::size_t rejectedWhitelistEntries {};
	std::size_t duplicateWhitelistEntries {};
	std::uint64_t settingsRevision {1};
	std::uint64_t detectionMask {};
	bool detectionMaskDirty {true};
	bool pluginEnabled {};

	// Defaults for the Karasu knobs, used before the config has been executed and
	// whenever a value in cs2ac.cfg does not parse.
	constexpr int karasuDefaultMinConfidence = 72;
	constexpr int karasuDefaultCorroborationWindow = 1800;
	// Keep in step with karasu::kDefaultSoloBanConfidence in src/karasu/karasu_policy.h.
	constexpr int karasuDefaultSoloBanConfidence = 55;

	std::vector<std::uint64_t> &WhitelistedSteamIds()
	{
		static std::vector<std::uint64_t> steamIds;
		return steamIds;
	}

	void BumpRevision()
	{
		if (++settingsRevision == 0)
		{
			settingsRevision = 1;
		}
	}

	void OnDetectionSettingChanged(CConVar<bool> *, CSplitScreenSlot, const bool *, const bool *)
	{
		detectionMaskDirty = true;
		BumpRevision();
	}

	void OnWhitelistChanged(CConVar<CUtlString> *, CSplitScreenSlot, const CUtlString *newValue, const CUtlString *)
	{
		auto &steamIds = WhitelistedSteamIds();
		steamIds.clear();
		rejectedWhitelistEntries = 0;
		duplicateWhitelistEntries = 0;
		std::string value = newValue ? newValue->Get() : "";
		std::replace(value.begin(), value.end(), ',', ' ');
		std::replace(value.begin(), value.end(), ';', ' ');
		std::istringstream entries(value);
		for (std::string entry; entries >> entry;)
		{
			std::uint64_t steamId = 0;
			const auto parsed = std::from_chars(entry.data(), entry.data() + entry.size(), steamId);
			if (parsed.ec == std::errc() && parsed.ptr == entry.data() + entry.size() && steamId != 0)
			{
				steamIds.push_back(steamId);
			}
			else
			{
				++rejectedWhitelistEntries;
			}
		}
		std::sort(steamIds.begin(), steamIds.end());
		const std::size_t parsedEntries = steamIds.size();
		steamIds.erase(std::unique(steamIds.begin(), steamIds.end()), steamIds.end());
		duplicateWhitelistEntries = parsedEntries - steamIds.size();
		BumpRevision();
	}

	struct Configuration
	{
		CConVar<bool> enabled {"cs2ac_enabled", FCVAR_NONE, "Enable or disable CS2AC", true, OnDetectionSettingChanged};
		CConVar<bool> aimbotEnabled {"cs2ac_aimbot_enabled", FCVAR_NONE, "Detect damaging visible aim snaps", true, OnDetectionSettingChanged};
		CConVar<bool> aimlockEnabled {"cs2ac_aimlock_enabled", FCVAR_NONE, "Detect unnaturally precise target tracking", true,
									  OnDetectionSettingChanged};
		CConVar<bool> antiaimEnabled {"cs2ac_antiaim_enabled", FCVAR_NONE, "Detect impossible or manipulated view angles", true,
									  OnDetectionSettingChanged};
		CConVar<bool> autostrafeEnabled {"cs2ac_autostrafe_enabled", FCVAR_NONE, "Detect automated air strafing", true, OnDetectionSettingChanged};
		CConVar<bool> bhopEnabled {"cs2ac_bhop_enabled", FCVAR_NONE, "Detect automated bunny hopping", true, OnDetectionSettingChanged};
		CConVar<bool> dllInjectionEnabled {"cs2ac_dll_injection_enabled", FCVAR_NONE, "Detect suspicious client event subscriptions", true,
										   OnDetectionSettingChanged};
		CConVar<bool> desubtickingEnabled {"cs2ac_desubticking_enabled", FCVAR_NONE, "Detect commands that remove normal subtick timing", true,
										   OnDetectionSettingChanged};
		CConVar<bool> doubletapEnabled {"cs2ac_doubletap_enabled", FCVAR_NONE, "Detect impossible rapid fire", true, OnDetectionSettingChanged};
		CConVar<bool> hyperscrollEnabled {"cs2ac_hyperscroll_enabled", FCVAR_NONE, "Detect automated jump-input frequency", true,
										  OnDetectionSettingChanged};
		CConVar<bool> inhumanAccuracyEnabled {"cs2ac_inhuman_accuracy_enabled", FCVAR_NONE, "Detect sustained near-perfect accuracy", true,
											  OnDetectionSettingChanged};
		CConVar<bool> invalidCvarEnabled {"cs2ac_invalid_cvar_enabled", FCVAR_NONE, "Detect unsafe client settings", true, OnDetectionSettingChanged};
		CConVar<bool> invalidInputEnabled {"cs2ac_invalid_input_enabled", FCVAR_NONE,
										   "Detect movement button changes without matching subtick records", true, OnDetectionSettingChanged};
		CConVar<bool> irregularBehaviorEnabled {"cs2ac_irregular_behavior_enabled", FCVAR_NONE,
												"Detect repeated success with unusually difficult shots", true, OnDetectionSettingChanged};
		CConVar<bool> namechangerEnabled {"cs2ac_namechanger_enabled", FCVAR_NONE, "Detect repeated player name changes", true,
										  OnDetectionSettingChanged};
		CConVar<bool> nullsEnabled {"cs2ac_nulls_enabled", FCVAR_NONE, "Detect mechanically perfect airborne opposite-direction switches", true,
									OnDetectionSettingChanged};
		CConVar<bool> silentaimEnabled {"cs2ac_silentaim_enabled", FCVAR_NONE, "Detect damaging shots that disagree with the visible aim", true,
										OnDetectionSettingChanged};
		CConVar<bool> subtickSpamEnabled {"cs2ac_subtick_spam_enabled", FCVAR_NONE,
										  "Detect repeated same-time button aliases carrying pitch or yaw changes", true, OnDetectionSettingChanged};
		CConVar<bool> chatAnnouncements {"cs2ac_chat_announcements", FCVAR_NONE, "Show CS2AC detections in public chat", true};
		CConVar<bool> centerAnnouncements {"cs2ac_center_announcements", FCVAR_NONE, "Show CS2AC detections in the center of the screen", true};
		CConVar<CUtlString> punishmentCommand {"cs2ac_punishment_command", FCVAR_NONE, "Command run for permanent-ban detections",
											   CUtlString("css_addban {steamid64} 0 CS2AC: {detection}")};
		CConVar<CUtlString> kickCommand {"cs2ac_kick_command", FCVAR_NONE, "Command run for kick-only detections",
										 CUtlString("css_kick #{userid} CS2AC: {detection}")};
		CConVar<CUtlString> webhookUrl {"cs2ac_webhook_url", FCVAR_PROTECTED, "Discord webhook URL for detection reports", CUtlString("")};
		CConVar<CUtlString> webhookRoleId {"cs2ac_webhook_role_id", FCVAR_NONE, "Discord role ID mentioned in detection reports", CUtlString("")};
		CConVar<CUtlString> webhookServerAddress {"cs2ac_webhook_server_address", FCVAR_NONE, "Public server address shown in Discord reports",
												  CUtlString("")};
		CConVar<CUtlString> webhookLogoUrl {"cs2ac_webhook_logo_url", FCVAR_NONE, "Public HTTPS URL for the logo shown in Discord reports",
											CUtlString("")};
		CConVar<CUtlString> language {"cs2ac_language", FCVAR_NONE, "Language used for public messages and Discord reports", CUtlString("en")};
		CConVar<CUtlString> whitelist {"cs2ac_whitelist", FCVAR_NONE, "SteamID64s that CS2AC may detect but never punish", CUtlString(""),
									   OnWhitelistChanged};

		// --- Karasu platform integration ---------------------------------------
		// The numeric knobs below are declared as strings deliberately. Every other
		// convar in this plugin is CConVar<bool> or CConVar<CUtlString>, so those two
		// instantiations are known to build against the pinned SDK; a numeric one is
		// not exercised anywhere in the tree. From a cs2ac.cfg author's point of view
		// nothing changes - "cs2ac_karasu_enforce 2" still reads as a number - and
		// ParseBoundedInt below clamps whatever arrives into a safe range.
		CConVar<bool> karasuRelay {"cs2ac_karasu_relay", FCVAR_NONE, "Relay detections to the Karasu CS2 plugin", true};
		CConVar<CUtlString> karasuRelayCommand {"cs2ac_karasu_relay_command", FCVAR_NONE,
												"Server command the Karasu CS2 plugin listens on for detection reports",
												CUtlString("karasu_anticheat_report")};
		CConVar<CUtlString> karasuEnforce {"cs2ac_karasu_enforce", FCVAR_NONE,
										   "Karasu enforcement: 0 report only, 1 kick locally, 2 kick and ask the platform to ban", CUtlString("2")};
		CConVar<CUtlString> karasuKickCommand {"cs2ac_karasu_kick_command", FCVAR_NONE,
											   "Command used to remove a player the Karasu policy has judged a cheater",
											   CUtlString("kickid {userid} Karasu Anti-Cheat")};
		CConVar<CUtlString> karasuMinConfidence {"cs2ac_karasu_min_confidence", FCVAR_NONE,
												 "Confidence a detection must reach to count toward corroboration (0-100)", CUtlString("72")};
		CConVar<CUtlString> karasuSoloBanConfidence {"cs2ac_karasu_solo_ban_confidence", FCVAR_NONE,
													 "Confidence at which one detection is enough to ban on its own (0-100)", CUtlString("55")};
		CConVar<CUtlString> karasuCorroborationWindow {"cs2ac_karasu_corroboration_window", FCVAR_NONE,
													   "Seconds a detection stays eligible to corroborate another one", CUtlString("1800")};
	};

	int ParseBoundedInt(const char *value, int fallback, int lowest, int highest)
	{
		if (!value || !*value)
		{
			return fallback;
		}
		std::string text = value;
		// Tolerate the surrounding whitespace a hand-edited cfg tends to pick up.
		const auto first = text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
		{
			return fallback;
		}
		const auto last = text.find_last_not_of(" \t\r\n");
		text = text.substr(first, last - first + 1);

		int parsed = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
		if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		{
			return fallback;
		}
		if (parsed < lowest)
		{
			return lowest;
		}
		if (parsed > highest)
		{
			return highest;
		}
		return parsed;
	}

	Configuration *configuration {};

	bool DetectionSetting(DetectionType detection)
	{
		if (!configuration)
		{
			return false;
		}
		switch (detection)
		{
			case DetectionType::Aimbot:
				return configuration->aimbotEnabled.GetBool();
			case DetectionType::Aimlock:
				return configuration->aimlockEnabled.GetBool();
			case DetectionType::AntiAim:
				return configuration->antiaimEnabled.GetBool();
			case DetectionType::Autostrafe:
				return configuration->autostrafeEnabled.GetBool();
			case DetectionType::Bhop:
				return configuration->bhopEnabled.GetBool();
			case DetectionType::DllInjection:
				return configuration->dllInjectionEnabled.GetBool();
			case DetectionType::Desubticking:
				return configuration->desubtickingEnabled.GetBool();
			case DetectionType::Doubletap:
				return configuration->doubletapEnabled.GetBool();
			case DetectionType::Hyperscroll:
				return configuration->hyperscrollEnabled.GetBool();
			case DetectionType::InhumanAccuracy:
				return configuration->inhumanAccuracyEnabled.GetBool();
			case DetectionType::InvalidCvar:
				return configuration->invalidCvarEnabled.GetBool();
			case DetectionType::InvalidInput:
				return configuration->invalidInputEnabled.GetBool();
			case DetectionType::IrregularBehavior:
				return configuration->irregularBehaviorEnabled.GetBool();
			case DetectionType::NameChanger:
				return configuration->namechangerEnabled.GetBool();
			case DetectionType::Nulls:
				return configuration->nullsEnabled.GetBool();
			case DetectionType::SilentAim:
				return configuration->silentaimEnabled.GetBool();
			case DetectionType::SubtickSpam:
				return configuration->subtickSpamEnabled.GetBool();
			case DetectionType::Count:
				return false;
		}
		return false;
	}
} // namespace

bool settings::Initialize()
{
	if (!configuration)
	{
		configuration = new (std::nothrow) Configuration;
		detectionMaskDirty = true;
	}
	return configuration != nullptr;
}

void settings::Shutdown()
{
	delete configuration;
	configuration = nullptr;
	WhitelistedSteamIds().clear();
	rejectedWhitelistEntries = 0;
	duplicateWhitelistEntries = 0;
	detectionMask = 0;
	detectionMaskDirty = true;
	pluginEnabled = false;
}

bool settings::IsPluginEnabled()
{
	GetDetectionMask();
	return pluginEnabled;
}

bool settings::IsDetectionEnabled(DetectionType detection)
{
	const auto index = static_cast<std::uint8_t>(detection);
	return index < static_cast<std::uint8_t>(DetectionType::Count) && (GetDetectionMask() & (std::uint64_t {1} << index)) != 0;
}

bool settings::IsPlayerWhitelisted(std::uint64_t steamId)
{
	const auto &steamIds = WhitelistedSteamIds();
	return steamId != 0 && std::binary_search(steamIds.begin(), steamIds.end(), steamId);
}

std::size_t settings::GetWhitelistCount()
{
	return WhitelistedSteamIds().size();
}

std::size_t settings::GetRejectedWhitelistCount()
{
	return rejectedWhitelistEntries;
}

std::size_t settings::GetDuplicateWhitelistCount()
{
	return duplicateWhitelistEntries;
}

std::size_t settings::GetEnabledDetectionCount()
{
	const std::uint64_t mask = GetDetectionMask();
	std::size_t count = 0;
	for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index)
	{
		count += (mask >> index) & 1;
	}
	return count;
}

std::uint64_t settings::GetDetectionMask()
{
	if (!detectionMaskDirty)
	{
		return detectionMask;
	}

	detectionMask = 0;
	pluginEnabled = configuration && configuration->enabled.GetBool();
	if (pluginEnabled)
	{
		for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index)
		{
			if (DetectionSetting(static_cast<DetectionType>(index)))
			{
				detectionMask |= std::uint64_t {1} << index;
			}
		}
	}
	detectionMaskDirty = false;
	return detectionMask;
}

std::uint64_t settings::GetRevision()
{
	return settingsRevision;
}

bool settings::ShowChatAnnouncements()
{
	return configuration && configuration->chatAnnouncements.GetBool();
}

bool settings::ShowCenterAnnouncements()
{
	return configuration && configuration->centerAnnouncements.GetBool();
}

const char *settings::GetPunishmentCommand()
{
	return configuration ? configuration->punishmentCommand.Get().Get() : "";
}

const char *settings::GetKickCommand()
{
	return configuration ? configuration->kickCommand.Get().Get() : "";
}

const char *settings::GetWebhookUrl()
{
	return configuration ? configuration->webhookUrl.Get().Get() : "";
}

const char *settings::GetWebhookRoleId()
{
	return configuration ? configuration->webhookRoleId.Get().Get() : "";
}

const char *settings::GetWebhookServerAddress()
{
	return configuration ? configuration->webhookServerAddress.Get().Get() : "";
}

const char *settings::GetWebhookLogoUrl()
{
	return configuration ? configuration->webhookLogoUrl.Get().Get() : "";
}

const char *settings::GetLanguage()
{
	return configuration ? configuration->language.Get().Get() : "en";
}

bool settings::IsKarasuRelayEnabled()
{
	return configuration && configuration->karasuRelay.GetBool();
}

const char *settings::GetKarasuRelayCommand()
{
	return configuration ? configuration->karasuRelayCommand.Get().Get() : "";
}

int settings::GetKarasuEnforceLevel()
{
	if (!configuration)
	{
		return 0;
	}
	return ParseBoundedInt(configuration->karasuEnforce.Get().Get(), 0, 0, 2);
}

const char *settings::GetKarasuKickCommand()
{
	return configuration ? configuration->karasuKickCommand.Get().Get() : "";
}

int settings::GetKarasuMinConfidence()
{
	if (!configuration)
	{
		return karasuDefaultMinConfidence;
	}
	return ParseBoundedInt(configuration->karasuMinConfidence.Get().Get(), karasuDefaultMinConfidence, 0, 100);
}

int settings::GetKarasuSoloBanConfidence()
{
	if (!configuration)
	{
		return karasuDefaultSoloBanConfidence;
	}
	return ParseBoundedInt(configuration->karasuSoloBanConfidence.Get().Get(), karasuDefaultSoloBanConfidence, 0, 100);
}

int settings::GetKarasuCorroborationWindow()
{
	if (!configuration)
	{
		return karasuDefaultCorroborationWindow;
	}
	// A window under a minute makes corroboration unreachable; a day is already
	// longer than any match, and the platform holds the real long-horizon ledger.
	return ParseBoundedInt(configuration->karasuCorroborationWindow.Get().Get(), karasuDefaultCorroborationWindow, 60, 86400);
}

void settings::MarkConfigReloaded()
{
	BumpRevision();
}

void settings::ExecuteConfig()
{
	if (interfaces::pEngine)
	{
		interfaces::pEngine->ServerCommand("exec cs2ac.cfg\n");
	}
}
