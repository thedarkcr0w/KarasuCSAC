#pragma once

#include <cstddef>
#include <cstdint>

enum class DetectionType : std::uint8_t
{
	Aimbot,
	Aimlock,
	AntiAim,
	Autostrafe,
	Bhop,
	DllInjection,
	Desubticking,
	Doubletap,
	Hyperscroll,
	InhumanAccuracy,
	InvalidCvar,
	InvalidInput,
	IrregularBehavior,
	NameChanger,
	Nulls,
	SilentAim,
	SubtickSpam,
	Triggerbot,
	Count,
};

namespace settings
{
	bool Initialize();
	void Shutdown();
	bool IsPluginEnabled();
	bool IsDetectionEnabled(DetectionType detection);
	bool IsPlayerWhitelisted(std::uint64_t steamId);
	std::size_t GetWhitelistCount();
	std::size_t GetRejectedWhitelistCount();
	std::size_t GetDuplicateWhitelistCount();
	std::size_t GetEnabledDetectionCount();
	std::uint64_t GetDetectionMask();
	std::uint64_t GetRevision();
	bool ShowChatAnnouncements();
	bool ShowCenterAnnouncements();
	bool AutomaticUpdatesEnabled();
	const char *GetPunishmentCommand();
	const char *GetKickCommand();
	const char *GetWebhookUrl();
	const char *GetWebhookRoleId();
	const char *GetWebhookServerAddress();
	const char *GetWebhookLogoUrl();
	const char *GetLanguage();

	// --- Karasu platform integration -------------------------------------------
	// Relay detections to the Karasu CS2 plugin, which owns the match context, the
	// report token and the API credentials. CS2AC itself holds no secret.
	bool IsKarasuRelayEnabled();
	const char *GetKarasuRelayCommand();
	// 0 = report only, 1 = kick locally, 2 = kick and ask the platform to ban.
	int GetKarasuEnforceLevel();
	const char *GetKarasuKickCommand();
	// Floor a detection must clear to count toward corroboration, 0-100.
	int GetKarasuMinConfidence();
	// Confidence at which a single detection bans on its own, 0-100.
	int GetKarasuSoloBanConfidence();
	// How long, in seconds, a detection stays eligible to corroborate another.
	int GetKarasuCorroborationWindow();

	void MarkConfigReloaded();
	void ExecuteConfig();
} // namespace settings
