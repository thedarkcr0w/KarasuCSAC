#pragma once

#include "detection/detection_system.h"
#include "clientcvar/client_cvar_value.h"
#include "common.h"
#include "localization.h"
#include "version_gen.h"

class WebhookService;
class UpdaterService;
class CMsgTEFireBullets;
class CMsgPlayerBulletHit;

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
	void OnFireBullets(const CMsgTEFireBullets &event);
	void OnPlayerBulletHit(const CMsgPlayerBulletHit &event);
	void HandleDetection(const char *detection, MovementPlayer *player, const localization::Text &evidence, bool kickOnly = false,
						 bool networkVetoed = false);
	void OnClientFullyConnect(CPlayerSlot slot);
	void OnClientSettingsChanged(CPlayerSlot slot);
	void OnClientDisconnect(CPlayerSlot slot);
	void PrintConfigSummary(bool reloaded) const;
	void PrintStatus() const;
	void PrintHelp() const;
	void ReloadConfig();
	void OnConfigLoaded();
	void CheckConfig() const;
	void TestAnnouncement() const;
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

	bool Activate(char *error, size_t maxlen, bool late);
	void ProcessJoinWatermarks();
	void ResetRuntime();
	void CleanupRuntime();

	struct JoinWatermarkState
	{
		std::chrono::steady_clock::time_point showAt;
		std::chrono::steady_clock::time_point expires;
		std::chrono::steady_clock::time_point nextCenterSend;
		int centerBroadcasts {};
		bool pending {};
		bool shown {};
		bool centerPending {};
		bool centerActive {};
	};

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
	UpdaterService *updater {};
	std::array<JoinWatermarkState, MAXPLAYERS + 1> joinWatermarks {};
	std::array<PunishmentLevel, MAXPLAYERS + 1> punishmentLevels {};
};

extern CS2ACPlugin g_CS2AC;
extern IClientCvarValue *g_pClientCvarValue;
