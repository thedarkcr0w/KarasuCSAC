#pragma once

#include "detection/detection_system.h"
#include "clientcvar/client_cvar_value.h"
#include "common.h"
#include "karasu/karasu_relay.h"
#include "utils/utils.h"
#include "version_gen.h"

class WebhookService;

class CS2ACPlugin final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
	void AllPluginsLoaded() override;
	bool QueryRunning(char *error, size_t maxlen) override;
	bool Unload(char *error, size_t maxlen) override;
	bool Pause(char *error, size_t maxlen) override;
	bool Unpause(char *error, size_t maxlen) override;

	const char *GetAuthor() override
	{
		return PLUGIN_AUTHOR;
	}

	const char *GetName() override
	{
		return PLUGIN_DISPLAY_NAME;
	}

	const char *GetDescription() override
	{
		return PLUGIN_DESCRIPTION;
	}

	const char *GetURL() override
	{
		return PLUGIN_URL;
	}

	const char *GetLicense() override
	{
		return PLUGIN_LICENSE;
	}

	const char *GetVersion() override
	{
		return PLUGIN_FULL_VERSION;
	}

	const char *GetDate() override
	{
		return __DATE__;
	}

	const char *GetLogTag() override
	{
		return PLUGIN_LOGTAG;
	}

	void OnLevelInit(char const *, char const *, char const *, char const *, bool, bool) override;
	void OnLevelShutdown() override;
	void OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands);
	void OnSetupMove(MovementPlayer *player, PlayerCommand *command);
	void OnGameFrame(bool simulating);
	void OnGameEvent(IGameEvent *event, MovementPlayer *player);
	void HandleDetection(const char *detection, MovementPlayer *player, std::string_view evidence = {}, bool kickOnly = false);
	void OnClientFullyConnect(CPlayerSlot slot);
	void OnClientSettingsChanged(CPlayerSlot slot);
	void OnClientDisconnect(CPlayerSlot slot);
	MovementPlayer *ResolveImpactShooter(int truncatedUserId) const;
	void PrintConfigSummary(bool reloaded) const;
	void PrintStatus() const;
	void PrintHelp() const;
	void ReloadConfig();
	void OnConfigLoaded();
	void CheckConfig() const;
	void TestAnnouncement() const;
	void TestKarasuRelay(const char *detection);
	void TestWebhook();
	void ReportConfigLoadTimeout();

	bool IsLoaded() const
	{
		return loaded;
	}

	bool simulatingPhysics {};
	CGlobalVars serverGlobals {};

private:
	enum class PunishmentLevel : std::uint8_t
	{
		None,
		Kick,
		Ban,
	};

	// Result of running the Karasu policy for one detection. When handled is set,
	// Karasu owns enforcement for this detection and the upstream punishment command
	// path is skipped entirely, so a server can never both kick locally and run an
	// admin-plugin ban for the same event.
	struct KarasuOutcome
	{
		bool handled {};
		utils::DetectionOutcome outcome {};
	};

	bool Activate(char *error, size_t maxlen, bool late);
	KarasuOutcome EvaluateKarasuPolicy(const char *detection, MovementPlayer *player, std::uint64_t steamId, std::string_view evidence);
	void ResetRuntime();
	void CleanupRuntime();
	bool loaded {};
	bool activationPending {};
	bool convarsRegistered {};
	bool svCheatsWatcherInstalled {};
	bool configLoaded {};
	bool configReloadPending {};
	bool configLoadFailed {};
	std::chrono::steady_clock::time_point lastConfigLoad;
	std::string activationError;
	detection::DetectionSystem detectionSystem;
	WebhookService *webhook {};
	std::array<PunishmentLevel, MAXPLAYERS + 1> punishmentLevels {};
	// Per-session corroboration ledger, one per slot. Cleared on disconnect so a
	// slot cannot inherit the previous occupant's evidence. The durable,
	// cross-session ledger lives on the Karasu API.
	std::array<karasu::PlayerVerdict, MAXPLAYERS + 1> karasuVerdicts {};
};

extern CS2ACPlugin g_CS2AC;
extern IClientCvarValue *g_pClientCvarValue;
