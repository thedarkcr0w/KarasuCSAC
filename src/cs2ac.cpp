#include "cs2ac.h"

#include "localization.h"
#include "movement_analysis/detection/movement_detection.h"
#include "movement_analysis/player_context.h"
#include "movement_analysis/settings/movement_settings.h"
#include "sdk/cgameresourceserviceserver.h"
#include "sdk/navphysicsinterface.h"
#include "settings.h"
#include "updater.h"
#include "webhook.h"
#include "utils/addresses.h"
#include "utils/ctimer.h"
#include "utils/detours.h"
#include "utils/gameconfig.h"
#include "utils/hooks.h"
#include "utils/interfaces.h"
#include "utils/schema.h"
#include "utils/utils.h"
#include "networksystem/inetworkmessages.h"
#include "cs_gameevents.pb.h"
#include "gameevents.pb.h"
#include "igameevents.h"

CS2ACPlugin g_CS2AC;
IClientCvarValue *g_pClientCvarValue {};

PLUGIN_EXPOSE(CS2ACPlugin, g_CS2AC);

namespace
{
	static constexpr const char *detectionNames[] = {
		"AIMBOT",    "AIMLOCK",     "ANTIAIM",          "AUTOSTRAFE",   "BHOP",          "DLL INJECTION",      "DESUBTICKING",
		"DOUBLETAP", "HYPERSCROLL", "INHUMAN ACCURACY", "INVALID CVAR", "INVALID INPUT", "IRREGULAR BEHAVIOR", "NAMECHANGER",
		"NULLS",     "SILENTAIM",   "SUBTICK SPAM",     "TRIGGERBOT",
	};
	static_assert(CS2AC_ARRAYSIZE(detectionNames) == static_cast<size_t>(DetectionType::Count));

	void HandleDetectionCallback(const char *detection, MovementPlayer *player, const localization::Text &evidence)
	{
		g_CS2AC.HandleDetection(detection, player, evidence);
	}

	void HandleNetworkVetoDetectionCallback(const char *detection, MovementPlayer *player, const localization::Text &evidence)
	{
		g_CS2AC.HandleDetection(detection, player, evidence, false, true);
	}

	bool IsKickOnlyDetection(const char *detection)
	{
		return CS2AC_STREQI(detection, "DESUBTICKING") || CS2AC_STREQI(detection, "NULLS") || CS2AC_STREQI(detection, "SUBTICK SPAM");
	}

	void ReplaceAll(std::string &value, const std::string &placeholder, const std::string &replacement)
	{
		for (size_t position = 0; (position = value.find(placeholder, position)) != std::string::npos; position += replacement.size())
		{
			value.replace(position, placeholder.size(), replacement);
		}
	}

	std::string SanitizeConsoleText(const char *value)
	{
		std::string safe = value ? value : "<unknown>";
		for (char &character : safe)
		{
			const auto byte = static_cast<unsigned char>(character);
			if (byte < 32 || byte == 127)
			{
				character = '?';
			}
		}
		return safe;
	}

	int CheckCommandTemplate(const char *settingName, const char *commandTemplate, bool requireSteamId)
	{
		if (!commandTemplate || !*commandTemplate)
		{
			Msg("[CS2AC] Review %s: it is empty, so this punishment is disabled.\n", settingName);
			return 1;
		}

		int findings = 0;
		bool hasSteamId = false;
		bool hasUserId = false;
		const std::string command = commandTemplate;
		if (command.front() == '"' || command.back() == '"')
		{
			Msg("[CS2AC] Review %s: the command itself starts or ends with a quote. Keep only the cfg value's outer quotes.\n", settingName);
			++findings;
		}
		for (size_t position = 0; position < command.size();)
		{
			const size_t open = command.find('{', position);
			const size_t close = command.find('}', position);
			if (close != std::string::npos && (open == std::string::npos || close < open))
			{
				Msg("[CS2AC] Review %s: it contains an unmatched closing brace.\n", settingName);
				++findings;
				position = close + 1;
				continue;
			}
			if (open == std::string::npos)
			{
				break;
			}
			const size_t placeholderEnd = command.find('}', open + 1);
			if (placeholderEnd == std::string::npos)
			{
				Msg("[CS2AC] Review %s: it contains an unmatched opening brace.\n", settingName);
				++findings;
				break;
			}

			const std::string placeholder = command.substr(open, placeholderEnd - open + 1);
			hasSteamId |= placeholder == "{steamid64}";
			hasUserId |= placeholder == "{userid}";
			if (placeholder != "{steamid64}" && placeholder != "{userid}" && placeholder != "{detection}")
			{
				Msg("[CS2AC] Review %s: %s is not a supported placeholder.\n", settingName, SanitizeConsoleText(placeholder.c_str()).c_str());
				++findings;
			}
			position = placeholderEnd + 1;
		}

		if (requireSteamId && !hasSteamId)
		{
			Msg("[CS2AC] Review %s: permanent bans need the {steamid64} placeholder.\n", settingName);
			++findings;
		}
		if (!requireSteamId && !hasSteamId && !hasUserId)
		{
			Msg("[CS2AC] Review %s: kicks need either {userid} or {steamid64}.\n", settingName);
			++findings;
		}

		std::string expanded = command;
		ReplaceAll(expanded, "{steamid64}", "18446744073709551615");
		ReplaceAll(expanded, "{userid}", "2147483647");
		ReplaceAll(expanded, "{detection}", "IRREGULAR BEHAVIOR");
		if (expanded.size() >= static_cast<size_t>(CCommand::MaxCommandLength()))
		{
			Msg("[CS2AC] Review %s: its expanded command can exceed the engine limit.\n", settingName);
			++findings;
		}
		return findings;
	}

	f64 ConfigLoadTimeout()
	{
		g_CS2AC.ReportConfigLoadTimeout();
		return 0.0;
	}

	void AddOffsetRequirement(const char *key, const char *plainName, std::vector<std::string> &missing)
	{
		if (!g_pGameConfig || g_pGameConfig->GetOffset(key) < 0)
		{
			missing.emplace_back(std::string("The ") + plainName + " offset is missing from the CS2AC game data.");
		}
	}

	struct SchemaRequirement
	{
		const char *className;
		const char *fieldName;
	};

	void AddSchemaRequirements(std::vector<std::string> &missing)
	{
		if (!g_pSchemaSystem || !modules::schemasystem || !modules::schemasystem->m_hModule || !modules::schemasystem->m_base
			|| !modules::schemasystem->m_size)
		{
			return;
		}
		static constexpr SchemaRequirement requirements[] = {
			{"CBaseEntity", "m_CBodyComponent"},
			{"CBaseEntity", "m_fFlags"},
			{"CBaseEntity", "m_hGroundEntity"},
			{"CBaseEntity", "m_flActualGravityScale"},
			{"CBaseEntity", "m_flGravityScale"},
			{"CBaseEntity", "m_flWaterLevel"},
			{"CBaseEntity", "m_hOwnerEntity"},
			{"CBaseEntity", "m_iTeamNum"},
			{"CBaseEntity", "m_lifeState"},
			{"CBaseEntity", "m_MoveType"},
			{"CBaseEntity", "m_nActualMoveType"},
			{"CBaseEntity", "m_nSubclassID"},
			{"CBaseEntity", "m_pCollision"},
			{"CBaseEntity", "m_vecAbsVelocity"},
			{"CBaseEntity", "m_vecBaseVelocity"},
			{"CBaseModelEntity", "m_Collision"},
			{"CBaseModelEntity", "m_vecViewOffset"},
			{"CBasePlayerController", "m_bIsHLTV"},
			{"CBasePlayerController", "m_iszPlayerName"},
			{"CBasePlayerController", "m_steamID"},
			{"CBasePlayerPawn", "m_hController"},
			{"CBasePlayerPawn", "m_pMovementServices"},
			{"CBasePlayerPawn", "m_pWeaponServices"},
			{"CBasePlayerPawn", "v_angle"},
			{"CBodyComponent", "m_pSceneNode"},
			{"CCollisionProperty", "m_collisionAttribute"},
			{"CCollisionProperty", "m_CollisionGroup"},
			{"CCSPlayer_MovementServices", "m_bDucked"},
			{"CCSPlayer_MovementServices", "m_ModernJump"},
			{"CCSPlayer_MovementServices", "m_vecLadderNormal"},
			{"CCSPlayerController", "m_hPlayerPawn"},
			{"CCSPlayerModernJump", "m_flLastLandedFrac"},
			{"CCSPlayerModernJump", "m_flLastUsableJumpPressFrac"},
			{"CCSPlayerModernJump", "m_nLastLandedTick"},
			{"CCSPlayerModernJump", "m_nLastUsableJumpPressTick"},
			{"CCSPlayerPawn", "m_angEyeAngles"},
			{"CCSPlayerPawn", "m_bOnGroundLastTick"},
			{"CCSPlayerPawn", "m_bIsScoped"},
			{"CCSPlayerPawn", "m_entitySpottedState"},
			{"CCSPlayerPawn", "m_ignoreLadderJumpTime"},
			{"CCSWeaponBaseVData", "m_flCycleTime"},
			{"CGameSceneNode", "m_vecAbsOrigin"},
			{"EntitySpottedState_t", "m_bSpottedByMask"},
			{"CPlayer_MovementServices", "m_nButtons"},
			{"CPlayer_MovementServices", "m_vecLastMovementImpulses"},
			{"CPlayer_MovementServices_Humanoid", "m_flSurfaceFriction"},
			{"CPlayer_WeaponServices", "m_hActiveWeapon"},
			{"VPhysicsCollisionAttribute_t", "m_nHierarchyId"},
			{"VPhysicsCollisionAttribute_t", "m_nInteractsWith"},
		};
		for (const auto &requirement : requirements)
		{
			if (!schema::HasField(requirement.className, requirement.fieldName))
			{
				missing.emplace_back(std::string("The required player data field ") + requirement.className + "." + requirement.fieldName
									 + " is unavailable.");
			}
		}
	}

	std::string JoinReasons(const std::vector<std::string> &missing)
	{
		std::string message = "[CS2AC] CS2AC did not load.";
		for (const auto &reason : missing)
		{
			message += " ";
			message += reason;
		}
		return message;
	}
} // namespace

CON_COMMAND(cs2ac_status, "Show the current CS2AC status")
{
	(void)args;
	g_CS2AC.PrintStatus();
}

CON_COMMAND(cs2ac_help, "Show the available CS2AC administrator commands")
{
	(void)args;
	g_CS2AC.PrintHelp();
}

CON_COMMAND(cs2ac_reload, "Reload and validate cs2ac.cfg")
{
	(void)args;
	g_CS2AC.ReloadConfig();
}

CON_COMMAND(cs2ac_check_config, "Check the current CS2AC settings without changing them")
{
	(void)args;
	g_CS2AC.CheckConfig();
}

CON_COMMAND(cs2ac_test_announcement, "Show a harmless CS2AC chat and center-screen test")
{
	(void)args;
	g_CS2AC.TestAnnouncement();
}

CON_COMMAND(cs2ac_webhook_test, "Send a harmless Discord webhook test")
{
	(void)args;
	g_CS2AC.TestWebhook();
}

CON_COMMAND(cs2ac_config_loaded, "Confirm that cs2ac.cfg finished loading")
{
	(void)args;
	g_CS2AC.OnConfigLoaded();
}

bool CS2ACPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	UpdaterService::ApplyPendingUpdate();
	if (!settings::Initialize())
	{
		if (error && maxlen)
		{
			snprintf(error, maxlen, "CS2AC could not reserve memory for its settings.");
		}
		return false;
	}
	webhook = new (std::nothrow) WebhookService;
	if (!webhook)
	{
		settings::Shutdown();
		if (error && maxlen)
		{
			snprintf(error, maxlen, "CS2AC could not reserve memory for Discord reports.");
		}
		return false;
	}
	updater = new (std::nothrow) UpdaterService;
	if (!updater)
	{
		delete webhook;
		webhook = nullptr;
		settings::Shutdown();
		if (error && maxlen)
		{
			snprintf(error, maxlen, "CS2AC could not reserve memory for automatic updates.");
		}
		return false;
	}
	if (late)
	{
		if (!Activate(error, maxlen, true))
		{
			return false;
		}
		ismm->AddListener(this, this);
	}
	else
	{
		// The game DLL finishes registering its messages and events after this early callback.
		activationPending = true;
	}
	return true;
}

void CS2ACPlugin::AllPluginsLoaded()
{
	if (!activationPending)
	{
		return;
	}

	activationPending = false;
	char error[1024] {};
	if (!Activate(error, sizeof(error), false))
	{
		activationError = error[0] ? error : "CS2AC could not finish loading after the game became ready.";
		Msg("[CS2AC] CS2AC is not running. %s\n", activationError.c_str());
		return;
	}
	g_SMAPI->AddListener(this, this);
}

bool CS2ACPlugin::QueryRunning(char *error, size_t maxlen)
{
	if (loaded)
	{
		return true;
	}
	const char *reason = activationError.empty() ? "CS2AC is waiting for the game to finish starting." : activationError.c_str();
	if (error && maxlen)
	{
		snprintf(error, maxlen, "%s", reason);
	}
	return false;
}

bool CS2ACPlugin::Activate(char *error, size_t maxlen, bool late)
{
	std::vector<std::string> missing;
	if (!g_SHPtr)
	{
		missing.emplace_back("Metamod's hook service is unavailable.");
	}

	modules::Initialize(missing);
	interfaces::Initialize(g_SMAPI, missing);
	utils::Initialize(missing);

	if (g_pGameConfig)
	{
		AddOffsetRequirement("GameEntitySystem", "game entity system", missing);
		AddOffsetRequirement("Teleport", "player teleport", missing);
		AddOffsetRequirement("IsEntityPawn", "player pawn check", missing);
		AddOffsetRequirement("IsEntityController", "player controller check", missing);
		AddOffsetRequirement("ClientOffset", "player list", missing);
	}

	if (g_pNetworkMessages && !g_pNetworkMessages->FindNetworkMessagePartial("TextMsg"))
	{
		missing.emplace_back("The chat message used for detection announcements is unavailable.");
	}
	if (g_pNetworkMessages && !g_pNetworkMessages->FindNetworkMessageById(GE_Source1LegacyGameEvent))
	{
		missing.emplace_back("The center-screen event message used for detection announcements is unavailable.");
	}
	if (g_pNetworkMessages && !g_pNetworkMessages->FindNetworkMessageById(GE_FireBulletsId))
	{
		missing.emplace_back("The exact weapon firing data used by Silentaim is unavailable.");
	}
	if (g_pCVar)
	{
		movement_settings::Validate(missing);
		ConVarRefAbstract svCheats("sv_cheats", true);
		if (!svCheats.IsValidRef() || !svCheats.IsConVarDataAvailable())
		{
			missing.emplace_back("The server variable sv_cheats is not available.");
		}
	}
	if (modules::server && modules::server->m_hModule && !modules::server->FindVirtualTable("CNavPhysicsInterface"))
	{
		missing.emplace_back("The movement trace interface could not be found.");
	}
	if (modules::server && modules::server->m_hModule && !modules::server->FindVirtualTable("CTraceFilterPlayerMovementCS"))
	{
		missing.emplace_back("The player movement trace filter could not be found.");
	}
	AddSchemaRequirements(missing);
	movement::ValidateDetours(missing);

	char clientCvarError[512] {};
	if (!g_ClientCvarValue.Validate(interfaces::pEngine, g_pNetworkMessages, clientCvarError, sizeof(clientCvarError)))
	{
		missing.emplace_back(clientCvarError[0] ? clientCvarError : "Player setting checks are unavailable.");
	}

	if (!missing.empty())
	{
		const std::string message = JoinReasons(missing);
		for (const auto &reason : missing)
		{
			Msg("[CS2AC] %s\n", reason.c_str());
		}
		if (error && maxlen)
		{
			snprintf(error, maxlen, "%s", message.c_str());
		}
		CleanupRuntime();
		return false;
	}

	bool clientCvarsReady =
		g_ClientCvarValue.Load(interfaces::pEngine, g_pNetworkMessages, interfaces::pGameEventSystem, clientCvarError, sizeof(clientCvarError));
	if (!clientCvarsReady)
	{
		missing.emplace_back(clientCvarError[0] ? clientCvarError : "Player setting checks could not be started.");
	}
	else
	{
		g_pClientCvarValue = g_ClientCvarValue.GetInterface();
	}
	if (!clientCvarsReady)
	{
		const std::string message = JoinReasons(missing);
		for (const auto &reason : missing)
		{
			Msg("[CS2AC] %s\n", reason.c_str());
		}
		if (error && maxlen)
		{
			snprintf(error, maxlen, "%s", message.c_str());
		}
		CleanupRuntime();
		return false;
	}

	ConVar_Register();
	convarsRegistered = true;
	MovementDetectionService::InitSvCheatsWatcher();
	svCheatsWatcherInstalled = true;
	detectionSystem.Load(HandleDetectionCallback, HandleNetworkVetoDetectionCallback);
	hooks::Initialize(missing);

	if (!missing.empty())
	{
		const std::string message = JoinReasons(missing);
		for (const auto &reason : missing)
		{
			Msg("[CS2AC] %s\n", reason.c_str());
		}
		if (error && maxlen)
		{
			snprintf(error, maxlen, "%s", message.c_str());
		}
		CleanupRuntime();
		return false;
	}

	// Install raw detours last so no later startup failure can leave one behind.
	if (!movement::InitDetours(missing))
	{
		const std::string message = JoinReasons(missing);
		for (const auto &reason : missing)
		{
			Msg("[CS2AC] %s\n", reason.c_str());
		}
		if (error && maxlen)
		{
			snprintf(error, maxlen, "%s", message.c_str());
		}
		CleanupRuntime();
		return false;
	}

	loaded = true;
	updater->Start();
	ResetRuntime();
	configReloadPending = true;
	configLoadFailed = false;
	settings::ExecuteConfig();
	StartTimer(ConfigLoadTimeout, 5.0, true, true);
	if (late)
	{
		for (i32 i = 1; i <= MAXPLAYERS; ++i)
		{
			auto *player = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(i));
			if (player && player->IsInGame())
			{
				g_pCS2ACPlayerManager->OnClientActive(player->GetPlayerSlot(), player->GetSteamId64(false));
				g_pCS2ACPlayerManager->OnClientFullyConnect(player->GetPlayerSlot());
				g_ClientCvarValue.OnClientFullyConnected(player->GetPlayerSlot(), player->IsFakeClient());
				OnClientFullyConnect(player->GetPlayerSlot());
			}
		}
		hooks::HookActivePlayers();
	}
	Msg("[CS2AC] CS2AC %s loaded successfully. Waiting for cs2ac.cfg to finish.\n", PLUGIN_FULL_VERSION);
	Msg("[CS2AC] Support development: buymeacoffee.com/karola3vax\n");
	return true;
}

bool CS2ACPlugin::Unload(char *error, size_t maxlen)
{
	if (!FlushAllDetours())
	{
		const char *reason = "CS2AC could not unload because one of its server hooks could not be removed safely.";
		if (error && maxlen)
		{
			snprintf(error, maxlen, "%s", reason);
		}
		Msg("[CS2AC] %s\n", reason);
		return false;
	}
	CleanupRuntime();
	Msg("[CS2AC] CS2AC unloaded successfully.\n");
	return true;
}

bool CS2ACPlugin::Pause(char *error, size_t maxlen)
{
	const char *reason = "CS2AC cannot be paused safely. Unload it instead.";
	if (error && maxlen)
	{
		snprintf(error, maxlen, "%s", reason);
	}
	return false;
}

bool CS2ACPlugin::Unpause(char *error, size_t maxlen)
{
	const char *reason = "CS2AC is not paused.";
	if (error && maxlen)
	{
		snprintf(error, maxlen, "%s", reason);
	}
	return false;
}

void CS2ACPlugin::OnLevelInit(char const *, char const *, char const *, char const *, bool, bool)
{
	if (loaded)
	{
		ResetRuntime();
		Msg("[CS2AC] A new map is ready. Detection evidence started fresh.\n");
	}
}

void CS2ACPlugin::OnLevelShutdown()
{
	if (loaded)
	{
		ResetRuntime();
		Msg("[CS2AC] The map ended. Detection evidence was cleared.\n");
	}
}

void CS2ACPlugin::OnProcessUsercmds(MovementPlayer *player, PlayerCommand *commands, int numCommands)
{
	detectionSystem.OnProcessUsercmds(player, commands, numCommands);
}

void CS2ACPlugin::OnSetupMove(MovementPlayer *player, PlayerCommand *command)
{
	auto *globals = g_pCS2ACUtils->GetServerGlobals();
	detectionSystem.OnSetupMove(player, command, globals ? globals->tickcount : 0);
}

void CS2ACPlugin::OnGameFrame(bool simulating)
{
	auto *globals = g_pCS2ACUtils->GetServerGlobals();
	if (simulating)
	{
		detectionSystem.OnGameFrame(globals ? globals->tickcount : 0);
	}
	if (webhook)
	{
		webhook->OnGameFrame();
	}
	if (updater)
	{
		updater->OnGameFrame();
	}
	ProcessJoinWatermarks();
}

void CS2ACPlugin::OnGameEvent(IGameEvent *event, MovementPlayer *player)
{
	auto *globals = g_pCS2ACUtils->GetServerGlobals();
	detectionSystem.OnGameEvent(event, player, globals ? globals->tickcount : 0);
}

void CS2ACPlugin::OnFireBullets(const CMsgTEFireBullets &event)
{
	auto *globals = g_pCS2ACUtils->GetServerGlobals();
	detectionSystem.OnFireBullets(event, globals ? globals->tickcount : 0);
}

void CS2ACPlugin::OnPlayerBulletHit(const CMsgPlayerBulletHit &event)
{
	auto *globals = g_pCS2ACUtils->GetServerGlobals();
	detectionSystem.OnPlayerBulletHit(event, globals ? globals->tickcount : 0);
}

void CS2ACPlugin::HandleDetection(const char *detection, MovementPlayer *player, const localization::Text &evidence, bool kickOnly,
								  bool networkVetoed)
{
	if (!detection || !*detection || !player || player->index < 1 || player->index > MAXPLAYERS)
	{
		return;
	}

	const std::string playerName = SanitizeConsoleText(player->GetName());
	const std::uint64_t steamId = player->GetSteamId64(false);
	const auto finish = [&](utils::DetectionOutcome outcome)
	{
		utils::AnnounceDetection(detection, player->GetName(), outcome);
		if (webhook)
		{
			webhook->Report(detection, player, evidence.localized, outcome);
		}
	};
	if (steamId)
	{
		Msg("[CS2AC] Detected %s on %s (SteamID64 %llu).\n", detection, playerName.c_str(), static_cast<unsigned long long>(steamId));
	}
	else
	{
		Msg("[CS2AC] Detected %s on %s (SteamID64 unavailable).\n", detection, playerName.c_str());
	}
	if (!evidence.english.empty())
	{
		Msg("[CS2AC] Evidence: %s\n", SanitizeConsoleText(evidence.english.c_str()).c_str());
	}
	if (networkVetoed)
	{
		finish(utils::DetectionOutcome::NetworkUnstable);
		Msg("[CS2AC] No punishment was sent because %s's connection exceeded the safe network limits.\n", playerName.c_str());
		return;
	}
	if (!steamId)
	{
		finish(utils::DetectionOutcome::IdentityUnavailable);
		Msg("[CS2AC] No punishment was sent because %s's SteamID64 is not ready yet.\n", playerName.c_str());
		return;
	}

	const bool whitelisted = settings::IsPlayerWhitelisted(steamId);
	if (whitelisted)
	{
		finish(utils::DetectionOutcome::Whitelisted);
		Msg("[CS2AC] No punishment was sent because %s is whitelisted.\n", playerName.c_str());
		return;
	}

	const PunishmentLevel requested = kickOnly || IsKickOnlyDetection(detection) ? PunishmentLevel::Kick : PunishmentLevel::Ban;
	auto &issued = punishmentLevels[player->index];
	if (issued >= requested)
	{
		finish(utils::DetectionOutcome::AlreadyPunished);
		Msg("[CS2AC] No duplicate punishment was sent for %s.\n", playerName.c_str());
		return;
	}

	const char *commandTemplate = requested == PunishmentLevel::Kick ? settings::GetKickCommand() : settings::GetPunishmentCommand();
	if (!commandTemplate || !*commandTemplate)
	{
		finish(utils::DetectionOutcome::PunishmentDisabled);
		Msg("[CS2AC] No punishment was sent because the matching command is empty.\n");
		return;
	}
	if (!interfaces::pEngine)
	{
		finish(utils::DetectionOutcome::CommandServiceUnavailable);
		Msg("[CS2AC] No punishment was sent because the server command service is unavailable.\n");
		return;
	}

	const int userId = interfaces::pEngine->GetPlayerUserId(player->GetPlayerSlot()).Get();
	std::string command = commandTemplate;
	if (userId < 0 && command.find("{userid}") != std::string::npos)
	{
		finish(utils::DetectionOutcome::IdentityUnavailable);
		Msg("[CS2AC] No punishment was sent because %s's user ID is not ready yet.\n", playerName.c_str());
		return;
	}
	ReplaceAll(command, "{steamid64}", std::to_string(steamId));
	ReplaceAll(command, "{userid}", std::to_string(userId));
	ReplaceAll(command, "{detection}", detection);
	if (command.size() >= static_cast<size_t>(CCommand::MaxCommandLength()))
	{
		finish(utils::DetectionOutcome::CommandTooLong);
		Msg("[CS2AC] No punishment was sent because the configured command is too long.\n");
		return;
	}
	finish(utils::DetectionOutcome::PunishmentSent);
	command.push_back('\n');
	interfaces::pEngine->ServerCommand(command.c_str());
	command.pop_back();
	Msg("[CS2AC] Sent: %s\n", SanitizeConsoleText(command.c_str()).c_str());
	issued = requested;
}

void CS2ACPlugin::OnClientFullyConnect(CPlayerSlot slot)
{
	auto *player = g_pCS2ACPlayerManager->ToPlayer(slot);
	detectionSystem.OnClientReady(player);
	const int index = slot.Get() + 1;
	if (player && index > 0 && index <= MAXPLAYERS && !player->IsFakeClient() && !player->IsCSTV() && !joinWatermarks[index].shown
		&& !joinWatermarks[index].pending)
	{
		joinWatermarks[index].showAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		joinWatermarks[index].pending = true;
	}
}

void CS2ACPlugin::OnClientSettingsChanged(CPlayerSlot slot)
{
	detectionSystem.OnClientSettingsChanged(g_pCS2ACPlayerManager->ToPlayer(slot));
}

void CS2ACPlugin::OnClientDisconnect(CPlayerSlot slot)
{
	auto *player = g_pCS2ACPlayerManager->ToPlayer(slot);
	const std::string playerName = player ? SanitizeConsoleText(player->GetName()) : "<unknown>";
	detectionSystem.OnClientDisconnect(player);
	const int index = slot.Get() + 1;
	if (index > 0 && index <= MAXPLAYERS)
	{
		joinWatermarks[index] = {};
	}
	if (player)
	{
		punishmentLevels[player->index] = PunishmentLevel::None;
		Msg("[CS2AC] Cleared %s's detection evidence because they disconnected.\n", playerName.c_str());
	}
}

void CS2ACPlugin::ProcessJoinWatermarks()
{
	const auto now = std::chrono::steady_clock::now();
	for (int index = 1; index <= MAXPLAYERS; ++index)
	{
		auto &state = joinWatermarks[index];
		if (!state.pending && !state.centerPending && !state.centerActive)
		{
			continue;
		}
		auto *player = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index));
		if (!player || !player->IsConnected() || player->IsFakeClient() || player->IsCSTV())
		{
			state = {};
			continue;
		}
		if (state.pending && now >= state.showAt && player->IsInGame())
		{
			state.pending = false;
			state.shown = true;
			state.centerPending = true;
			utils::AnnounceWatermarkTo(player->GetPlayerSlot(), false);
		}
		if (state.centerPending && player->IsInGame() && !utils::IsDetectionAnnouncementActive())
		{
			state.centerPending = false;
			utils::AnnounceWatermarkTo(player->GetPlayerSlot(), true);
			state.centerActive = true;
			state.expires = now + std::chrono::seconds(3);
			state.nextCenterSend = now + std::chrono::milliseconds(100);
			state.centerBroadcasts = 1;
		}
		if (!state.centerActive)
		{
			continue;
		}
		if (utils::IsDetectionAnnouncementActive() || !player->IsInGame())
		{
			state.centerActive = false;
			continue;
		}
		if (now >= state.expires)
		{
			utils::ClearWatermarkFor(player->GetPlayerSlot());
			state.centerActive = false;
			continue;
		}
		if (now >= state.nextCenterSend && state.centerBroadcasts < 31)
		{
			utils::AnnounceWatermarkTo(player->GetPlayerSlot(), true);
			state.nextCenterSend = now + std::chrono::milliseconds(100);
			++state.centerBroadcasts;
		}
	}
}

void CS2ACPlugin::PrintConfigSummary(bool reloaded) const
{
	const size_t enabled = settings::GetEnabledDetectionCount();
	const size_t total = static_cast<size_t>(DetectionType::Count);
	Msg("[CS2AC] Configuration %s: %zu/%zu detectors enabled, %zu whitelisted SteamIDs, %zu rejected entries, %zu duplicates ignored.\n",
		reloaded ? "reloaded" : "loaded", enabled, total, settings::GetWhitelistCount(), settings::GetRejectedWhitelistCount(),
		settings::GetDuplicateWhitelistCount());
	Msg("[CS2AC] Public announcements: chat %s, center screen %s.\n", settings::ShowChatAnnouncements() ? "on" : "off",
		settings::ShowCenterAnnouncements() ? "on" : "off");
	Msg("[CS2AC] Automatic updates: %s.\n", settings::AutomaticUpdatesEnabled() ? "on" : "off");
	Msg("[CS2AC] Punishments: permanent ban %s, kick %s.\n",
		settings::GetPunishmentCommand() && *settings::GetPunishmentCommand() ? "configured" : "disabled",
		settings::GetKickCommand() && *settings::GetKickCommand() ? "configured" : "disabled");
	Msg("[CS2AC] Discord webhook: %s.\n",
		webhook && webhook->IsDiscordConfigured() ? (webhook->IsDiscordDisabled() ? "disabled after an error" : "configured") : "not configured");
	Msg("[CS2AC] JSON webhook: %s.\n",
		webhook && webhook->IsJsonConfigured() ? (webhook->IsJsonDisabled() ? "disabled after an error" : "configured") : "not configured");
}

void CS2ACPlugin::PrintHelp() const
{
	Msg("[CS2AC] Administrator commands:\n");
	Msg("[CS2AC] cs2ac_status - Show the plugin, player, detector, announcement, and punishment status.\n");
	Msg("[CS2AC] cs2ac_reload - Reload cs2ac.cfg and clear transient detector evidence when it finishes.\n");
	Msg("[CS2AC] cs2ac_check_config - Check the current settings without changing them.\n");
	Msg("[CS2AC] cs2ac_test_announcement - Show a harmless chat and center-screen test without punishing anyone.\n");
	Msg("[CS2AC] cs2ac_webhook_test - Send a harmless test report to each configured webhook.\n");
}

void CS2ACPlugin::ReloadConfig()
{
	if (configReloadPending)
	{
		Msg("[CS2AC] A configuration load is already in progress. Please wait for it to finish.\n");
		return;
	}
	if (!interfaces::pEngine)
	{
		Msg("[CS2AC] The configuration could not be reloaded because the server command service is unavailable.\n");
		return;
	}

	configReloadPending = true;
	configLoadFailed = false;
	Msg("[CS2AC] Reloading cs2ac.cfg. CS2AC will confirm when the file reaches its final marker.\n");
	settings::ExecuteConfig();
	StartTimer(ConfigLoadTimeout, 5.0, true, true);
}

void CS2ACPlugin::OnConfigLoaded()
{
	const bool reloaded = configLoaded;
	configLoaded = true;
	configReloadPending = false;
	configLoadFailed = false;
	lastConfigLoad = std::chrono::steady_clock::now();
	settings::MarkConfigReloaded();
	localization::Reload(settings::GetLanguage());
	if (webhook)
	{
		webhook->Reload();
	}
	PrintConfigSummary(reloaded);
	CheckConfig();
}

void CS2ACPlugin::CheckConfig() const
{
	Msg("[CS2AC] Checking the current configuration. Nothing will be changed.\n");
	int findings = 0;
	if (configReloadPending)
	{
		Msg("[CS2AC] Review cs2ac.cfg: it is still loading, so this check may not reflect the whole file yet.\n");
		++findings;
	}
	else if (configLoadFailed)
	{
		Msg("[CS2AC] Review cs2ac.cfg: the last load did not reach its final cs2ac_config_loaded marker.\n");
		++findings;
	}
	if (!settings::IsPluginEnabled())
	{
		Msg("[CS2AC] Review cs2ac_enabled: detection is currently disabled.\n");
		++findings;
	}
	if (settings::GetRejectedWhitelistCount())
	{
		Msg("[CS2AC] Review cs2ac_whitelist: %zu entries were rejected because they are not valid SteamID64 values.\n",
			settings::GetRejectedWhitelistCount());
		++findings;
	}
	if (settings::GetDuplicateWhitelistCount())
	{
		Msg("[CS2AC] Review cs2ac_whitelist: %zu duplicate entries were ignored.\n", settings::GetDuplicateWhitelistCount());
		++findings;
	}
	findings += CheckCommandTemplate("cs2ac_punishment_command", settings::GetPunishmentCommand(), true);
	findings += CheckCommandTemplate("cs2ac_kick_command", settings::GetKickCommand(), false);
	if (!WebhookService::IsValidUrl(settings::GetWebhookUrl()))
	{
		Msg("[CS2AC] Review cs2ac_webhook_url: it is not a Discord webhook URL.\n");
		++findings;
	}
	if (!WebhookService::IsValidJsonUrl(settings::GetJsonWebhookUrl()))
	{
		Msg("[CS2AC] Review cs2ac_json_webhook_url: it must be an HTTPS URL.\n");
		++findings;
	}
	if (!WebhookService::IsValidBearerToken(settings::GetJsonWebhookBearerToken()))
	{
		Msg("[CS2AC] Review cs2ac_json_webhook_bearer_token: it must not contain whitespace and must be 4096 characters or shorter.\n");
		++findings;
	}
	if (!WebhookService::IsValidRoleId(settings::GetWebhookRoleId()))
	{
		Msg("[CS2AC] Review cs2ac_webhook_role_id: it must contain only numbers.\n");
		++findings;
	}
	if (!WebhookService::IsValidLogoUrl(settings::GetWebhookLogoUrl()))
	{
		Msg("[CS2AC] Review cs2ac_webhook_logo_url: it must be an HTTPS URL.\n");
		++findings;
	}
	if (!settings::ShowChatAnnouncements() && !settings::ShowCenterAnnouncements())
	{
		Msg("[CS2AC] Review announcements: chat and center screen are both disabled. Detections will only appear in the server console.\n");
		++findings;
	}
	if (MovementDetectionService::IsSvCheatsTestingAllowed())
	{
		Msg("[CS2AC] Review sv_cheats testing: inherited movement detections are allowed while sv_cheats is on.\n");
		++findings;
	}

	if (findings)
	{
		Msg("[CS2AC] Configuration check found %d %s to review.\n", findings, findings == 1 ? "thing" : "things");
	}
	else
	{
		Msg("[CS2AC] Configuration check passed. Everything is ready.\n");
	}
}

void CS2ACPlugin::TestAnnouncement() const
{
	Msg("[CS2AC] Running a harmless announcement test. No detection or punishment will be created.\n");
	utils::AnnounceTest();
}

void CS2ACPlugin::TestWebhook()
{
	if (webhook)
	{
		webhook->Test();
	}
}

void CS2ACPlugin::ReportConfigLoadTimeout()
{
	if (!configReloadPending)
	{
		return;
	}
	configReloadPending = false;
	configLoadFailed = true;
	Msg("[CS2AC] cs2ac.cfg did not finish loading. Make sure it exists and ends with cs2ac_config_loaded.\n");
	Msg("[CS2AC] Some settings may have changed before loading stopped. Run cs2ac_check_config before continuing.\n");
}

void CS2ACPlugin::PrintStatus() const
{
	int connected = 0;
	int humans = 0;
	int bots = 0;
	if (g_pCS2ACPlayerManager)
	{
		for (int index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *player = g_pCS2ACPlayerManager->ToPlayer(static_cast<u32>(index));
			if (!player || !player->IsConnected() || player->IsCSTV())
			{
				continue;
			}
			++connected;
			player->IsFakeClient() ? ++bots : ++humans;
		}
	}

	std::string enabledNames;
	for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index)
	{
		if (!settings::IsDetectionEnabled(static_cast<DetectionType>(index)))
		{
			continue;
		}
		if (!enabledNames.empty())
		{
			enabledNames += ", ";
		}
		enabledNames += detectionNames[index];
	}

	Msg("[CS2AC] Status: %s, version %s.\n", loaded ? "running" : "not running", PLUGIN_FULL_VERSION);
	if (configReloadPending)
	{
		Msg("[CS2AC] Configuration: waiting for cs2ac.cfg to finish.\n");
	}
	else if (configLoadFailed)
	{
		Msg("[CS2AC] Configuration: the last load did not finish. Run cs2ac_check_config after fixing the file.\n");
	}
	else if (!configLoaded)
	{
		Msg("[CS2AC] Configuration: not confirmed yet.\n");
	}
	else
	{
		const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastConfigLoad).count();
		Msg("[CS2AC] Configuration: loaded %lld seconds ago.\n", static_cast<long long>(age));
	}
	Msg("[CS2AC] Players: %d connected (%d human, %d bots).\n", connected, humans, bots);
	Msg("[CS2AC] Detectors: %zu/%zu enabled.\n", settings::GetEnabledDetectionCount(), static_cast<size_t>(DetectionType::Count));
	Msg("[CS2AC] Enabled detectors: %s.\n", enabledNames.empty() ? "none" : enabledNames.c_str());
	Msg("[CS2AC] Whitelist: %zu valid entries, %zu rejected, %zu duplicates ignored during the last update.\n", settings::GetWhitelistCount(),
		settings::GetRejectedWhitelistCount(), settings::GetDuplicateWhitelistCount());
	Msg("[CS2AC] Announcements: chat %s, center screen %s.\n", settings::ShowChatAnnouncements() ? "on" : "off",
		settings::ShowCenterAnnouncements() ? "on" : "off");
	Msg("[CS2AC] Punishments: permanent ban %s, kick %s.\n",
		settings::GetPunishmentCommand() && *settings::GetPunishmentCommand() ? "configured" : "disabled",
		settings::GetKickCommand() && *settings::GetKickCommand() ? "configured" : "disabled");
	const size_t webhookQueueSize = webhook ? webhook->QueueSize() : 0;
	Msg("[CS2AC] Discord webhook: %s, %zu queued report%s.\n",
		webhook && webhook->IsDiscordConfigured() ? (webhook->IsDiscordDisabled() ? "disabled after an error" : "configured") : "not configured",
		webhookQueueSize, webhookQueueSize == 1 ? "" : "s");
	Msg("[CS2AC] JSON webhook: %s.\n",
		webhook && webhook->IsJsonConfigured() ? (webhook->IsJsonDisabled() ? "disabled after an error" : "configured") : "not configured");
	Msg("[CS2AC] sv_cheats testing: %s.\n", MovementDetectionService::IsSvCheatsTestingAllowed() ? "allowed" : "not allowed");
	Msg("[CS2AC] Support development: buymeacoffee.com/karola3vax\n");
}

void CS2ACPlugin::ResetRuntime()
{
	if (configReloadPending)
	{
		configReloadPending = false;
		configLoadFailed = true;
		Msg("[CS2AC] The configuration load was interrupted by a map reset. Run cs2ac_reload after the new map is ready.\n");
	}
	simulatingPhysics = false;
	serverGlobals = {};
	hooks::ResetMap();
	utils::ResetDetectionAnnouncement();
	RemoveAllTimers();
	g_ClientCvarValue.OnMapReset();
	detectionSystem.Reset();
	g_pCS2ACPlayerManager->ResetPlayers();
	punishmentLevels.fill(PunishmentLevel::None);
}

void CS2ACPlugin::CleanupRuntime()
{
	// Any exceptional SourceHook that survives manual cleanup must remain inert
	// until Metamod removes all hooks owned by this plugin.
	loaded = false;
	if (webhook)
	{
		webhook->Unload();
		delete webhook;
		webhook = nullptr;
	}
	if (updater)
	{
		updater->Unload();
		delete updater;
		updater = nullptr;
	}
	bool sourceHooksRemoved = hooks::Cleanup();
	utils::ResetDetectionAnnouncement();
	RemoveAllTimers();
	detectionSystem.Unload();
	g_pClientCvarValue = nullptr;
	sourceHooksRemoved = g_ClientCvarValue.Unload() && sourceHooksRemoved;
	if (!sourceHooksRemoved)
	{
		Warning("[CS2AC] Some Metamod hooks are still attached but inert. Metamod will remove them before closing CS2AC.\n");
	}
	if (svCheatsWatcherInstalled)
	{
		MovementDetectionService::CleanupSvCheatsWatcher();
		svCheatsWatcherInstalled = false;
	}
	localization::Shutdown();
	settings::Shutdown();
	if (convarsRegistered)
	{
		ConVar_Unregister();
		convarsRegistered = false;
	}
	if (g_pCS2ACPlayerManager)
	{
		g_pCS2ACPlayerManager->ResetPlayers();
	}
	simulatingPhysics = false;
	serverGlobals = {};
	punishmentLevels.fill(PunishmentLevel::None);
	joinWatermarks.fill({});
	utils::Cleanup();
	modules::Cleanup();
	activationPending = false;
	configLoaded = false;
	configReloadPending = false;
	configLoadFailed = false;
	lastConfigLoad = {};
	activationError.clear();
}

CGameEntitySystem *GameEntitySystem()
{
	return interfaces::pGameResourceServiceServer ? interfaces::pGameResourceServiceServer->GetGameEntitySystem() : nullptr;
}
