// Detects client settings that are unsafe, impossible, or protected by sv_cheats.
#include "common.h"
#include "convar.h"
#include "clientcvar/iclientcvarvalue.h"
#include "movement_analysis/detection/movement_detection.h"
#include "settings.h"
#include "utils/ctimer.h"
#include "tier1/random.h"
#include <chrono>

CConVarRef<bool> sv_cheats("sv_cheats");
CConVar<bool> cs2ac_allow_sv_cheats_testing("cs2ac_allow_sv_cheats_testing", FCVAR_NONE, "Keep CS2AC detections enabled while sv_cheats is enabled",
											false);
CConVar<bool> cs2ac_invalid_cvar_debug("cs2ac_invalid_cvar_debug", FCVAR_NONE, "Show Invalid CVar replies, latch transitions, and unavailable checks",
									   false);

#define INVALID_CVAR_DEBUG(...) \
	do \
	{ \
		if (cs2ac_invalid_cvar_debug.GetBool()) \
			Msg("[CS2AC Invalid CVar] " __VA_ARGS__); \
	} while (0)

// Give replicated cheat-protected settings time to return to normal after sv_cheats is disabled.

bool MovementDetectionService::ShouldRunDetections() const
{
	if (!settings::IsPluginEnabled())
	{
		return false;
	}
	if (!sv_cheats.IsValidRef() || !sv_cheats.IsConVarDataAvailable())
	{
		return true;
	}
	return !sv_cheats.Get() || cs2ac_allow_sv_cheats_testing.Get();
}

bool MovementDetectionService::IsSvCheatsTestingAllowed()
{
	return cs2ac_allow_sv_cheats_testing.Get();
}

bool MovementDetectionService::ShouldCheckClientCvars() const
{
	return ShouldRunDetections() && settings::IsDetectionEnabled(DetectionType::InvalidCvar);
}

extern IClientCvarValue *g_pClientCvarValue;

#define INTEGRITY_CHECK_MIN_INTERVAL 1.0f
#define INTEGRITY_CHECK_MAX_INTERVAL 5.0f
#define MINIMUM_FPS_MAX              64.0f
#define MAXIMUM_M_YAW                0.3f

static constexpr auto SV_CHEATS_MAX_PROPAGATION_DELAY = std::chrono::seconds(30);

static_function localization::Text InvalidNumber(const char *cvar)
{
	return localization::Format("evidence.invalid_cvar.invalid_number", "The client returned an invalid number for {cvar}.", {{"cvar", cvar}});
}

static_function localization::Text AboveMaximum(const char *cvar, double value, const char *maximum)
{
	return localization::Format("evidence.invalid_cvar.above_maximum", "{cvar} is {value}, but the maximum allowed value is {maximum}.",
								{{"cvar", cvar}, {"value", tfm::format("%.6g", value)}, {"maximum", maximum}});
}

static_function localization::Text OutsideRange(const char *cvar, double value, const char *minimum, const char *maximum)
{
	return localization::Format("evidence.invalid_cvar.outside_range", "{cvar} is {value}, but it must be between {minimum} and {maximum}.",
								{{"cvar", cvar}, {"value", tfm::format("%.6g", value)}, {"minimum", minimum}, {"maximum", maximum}});
}

static_function localization::Text MustEqual(const char *cvar, double value, const char *expected)
{
	return localization::Format("evidence.invalid_cvar.must_equal", "{cvar} is {value}, but it must be {expected}.",
								{{"cvar", cvar}, {"value", tfm::format("%.6g", value)}, {"expected", expected}});
}

static_global const char *cvarNames[] = {
	// "m_yaw",       // Disabled: this was kick-only, but legitimate turn binds temporarily use high values.
	"fps_max",        // Expected to stay at or above the server tick rate.
	"sv_cheats",      // replicated
	"sensitivity",    // capped (0.0001f => 8.0f)
	"cl_showpos",     // cheat (default 0)
	"cam_showangles", // cheat (default 0)
	"cl_drawhud",     // cheat (default 1)
	"cl_pitchdown",   // should always be 89.0f
	"cl_pitchup",     // should always be 89.0f
	"cl_yawspeed",    // should always be 210.0f
	"fov_cs_debug"    // cheat (default 0)
};

static_global const char *userInfoCvarNames[] = {
	"sensitivity",
	// "m_yaw", // Disabled with the queried check above; keep its validation code in case the check returns later.
};

static_global auto cheatCvarCheckerGraceUntil = (std::chrono::steady_clock::time_point::min)();

static_function bool ShouldEnforceCheatCvars()
{
	assert(sv_cheats.IsValidRef() && sv_cheats.IsConVarDataAvailable());
	if (sv_cheats.Get())
	{
		return false;
	}
	return std::chrono::steady_clock::now() > cheatCvarCheckerGraceUntil;
}

static_function void ResetTransientDetectionStateForAllPlayers()
{
	if (!g_pCS2ACPlayerManager)
	{
		return;
	}
	for (u32 i = 1; i <= MAXPLAYERS; ++i)
	{
		CS2ACPlayer *player = g_pCS2ACPlayerManager->ToPlayer(i);
		if (player && player->movementDetection)
		{
			player->movementDetection->ResetTransientDetectionState();
		}
	}
}

static_global void OnCvarChanged(ConVarRefAbstract *ref, CSplitScreenSlot, const char *pNewValue, const char *, void *)
{
	if (!ref)
	{
		return;
	}
	const bool svCheatsChanged = CS2AC_STREQI(ref->GetName(), "sv_cheats");
	if (!svCheatsChanged && !CS2AC_STREQI(ref->GetName(), "cs2ac_allow_sv_cheats_testing"))
	{
		return;
	}
	if (svCheatsChanged)
	{
		assert(sv_cheats.IsValidRef() && sv_cheats.IsConVarDataAvailable());
		const bool enabled = pNewValue ? CS2AC_STREQI(pNewValue, "true") || (utils::IsNumeric(pNewValue) && atof(pNewValue) != 0.0) : sv_cheats.Get();
		if (!enabled)
		{
			cheatCvarCheckerGraceUntil = std::chrono::steady_clock::now() + SV_CHEATS_MAX_PROPAGATION_DELAY;
		}
	}
	ResetTransientDetectionStateForAllPlayers();
}

void MovementDetectionService::InitSvCheatsWatcher()
{
	sv_cheats = CConVarRef<bool>(ConVarRefAbstract("sv_cheats", true));
	g_pCVar->InstallGlobalChangeCallback(OnCvarChanged);
	assert(sv_cheats.IsValidRef() && sv_cheats.IsConVarDataAvailable());
	if (!sv_cheats.Get())
	{
		cheatCvarCheckerGraceUntil = std::chrono::steady_clock::now() + SV_CHEATS_MAX_PROPAGATION_DELAY;
	}
}

void MovementDetectionService::CleanupSvCheatsWatcher()
{
	g_pCVar->RemoveGlobalChangeCallback(OnCvarChanged);
	cheatCvarCheckerGraceUntil = (std::chrono::steady_clock::time_point::min)();
}

static_function void ValidateQueriedCvar(CPlayerSlot nSlot, ECvarValueStatus eStatus, const char *pszCvarName, const char *pszCvarValue)
{
	if (eStatus != ECvarValueStatus::ValueIntact)
	{
		INVALID_CVAR_DEBUG("Player slot %d returned status %d for %s; the reply was ignored.\n", nSlot.Get(), static_cast<int>(eStatus),
						   pszCvarName ? pszCvarName : "an unknown CVar");
		return;
	}
	CS2ACPlayer *player = g_pCS2ACPlayerManager->ToPlayer(nSlot);
	if (!player || !player->IsInGame() || !player->movementDetection->ShouldCheckClientCvars())
	{
		return;
	}
	bool invalid = false;
	auto markInvalid = [&](const localization::Text &reason, bool kickOnly = false)
	{
		if (CS2AC_STREQI(pszCvarName, "m_yaw") || CS2AC_STREQI(pszCvarName, "sensitivity"))
		{
			player->movementDetection->MarkCvarSource(pszCvarName, reason, true, false, kickOnly);
		}
		else
		{
			player->movementDetection->MarkInvalidCvar(pszCvarName, reason, kickOnly);
		}
		invalid = true;
	};
	if (CS2AC_STREQI(pszCvarName, "m_yaw"))
	{
		if (!utils::IsNumeric(pszCvarValue))
		{
			markInvalid(InvalidNumber("m_yaw"));
		}
		else if (const f64 yaw = atof(pszCvarValue); yaw > MAXIMUM_M_YAW)
		{
			markInvalid(AboveMaximum("m_yaw", yaw, "0.3"), true);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "fps_max"))
	{
		if (!utils::IsNumeric(pszCvarValue))
		{
			markInvalid(InvalidNumber("fps_max"));
		}
		else
		{
			const f64 fps = atof(pszCvarValue);
			player->movementDetection->currentMaxFps = fps;
			if (fps > 0.0f && fps < MINIMUM_FPS_MAX)
			{
				markInvalid(localization::Format("evidence.invalid_cvar.fps_max",
												 "fps_max is {value}, but it must be at least 64 or 0 for unlimited.",
												 {{"value", tfm::format("%.6g", fps)}}),
							true);
			}
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "sv_cheats"))
	{
		if (!ShouldEnforceCheatCvars())
		{
			return;
		}
		// Allow time for the server's disabled value to reach the client before checking it.
		const bool numeric = utils::IsNumeric(pszCvarValue);
		const bool disabled = CS2AC_STREQI(pszCvarValue, "false") || (numeric && atof(pszCvarValue) == 0.0);
		if (!disabled)
		{
			markInvalid(localization::Format("evidence.invalid_cvar.sv_cheats",
											 "sv_cheats is enabled or invalid on the client while it is disabled on the server."));
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "sensitivity"))
	{
		const bool numeric = utils::IsNumeric(pszCvarValue);
		const f32 sens = numeric ? atof(pszCvarValue) : 0.0f;
		// The actual upper bound is 8.0f but this is to account for possible future updates.
		if (!numeric || sens < 0.0001f || sens > 20.0f)
		{
			localization::Text reason = numeric ? OutsideRange("sensitivity", sens, "0.0001", "20") : InvalidNumber("sensitivity");
			markInvalid(reason);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "cl_showpos") || CS2AC_STREQI(pszCvarName, "cam_showangles"))
	{
		if (!ShouldEnforceCheatCvars())
		{
			return;
		}
		if (!CS2AC_STREQI(pszCvarValue, "0") && !CS2AC_STREQI(pszCvarValue, "false"))
		{
			localization::Text reason =
				localization::Format("evidence.invalid_cvar.enabled_without_cheats",
									 "{cvar} is enabled on the client despite sv_cheats being disabled on the server.", {{"cvar", pszCvarName}});
			markInvalid(reason);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "cl_drawhud"))
	{
		if (!ShouldEnforceCheatCvars())
		{
			return;
		}
		if (CS2AC_STREQI(pszCvarValue, "0") || CS2AC_STREQI(pszCvarValue, "false"))
		{
			localization::Text reason =
				localization::Format("evidence.invalid_cvar.disabled_without_cheats",
									 "{cvar} is disabled on the client despite sv_cheats being disabled on the server.", {{"cvar", "cl_drawhud"}});
			markInvalid(reason);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "fov_cs_debug"))
	{
		if (!ShouldEnforceCheatCvars())
		{
			return;
		}
		const f64 fovValue = utils::IsNumeric(pszCvarValue) ? atof(pszCvarValue) : NAN;
		if (!std::isfinite(fovValue) || fovValue != 0.0f)
		{
			localization::Text reason = std::isfinite(fovValue) ? MustEqual("fov_cs_debug", fovValue, "0") : InvalidNumber("fov_cs_debug");
			markInvalid(reason);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "cl_pitchdown"))
	{
		const bool numeric = utils::IsNumeric(pszCvarValue);
		const f64 value = numeric ? atof(pszCvarValue) : 0.0;
		if (!numeric || value != 89.0)
		{
			localization::Text reason = numeric ? MustEqual("cl_pitchdown", value, "89") : InvalidNumber("cl_pitchdown");
			markInvalid(reason);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "cl_pitchup"))
	{
		const bool numeric = utils::IsNumeric(pszCvarValue);
		const f64 value = numeric ? atof(pszCvarValue) : 0.0;
		if (!numeric || value != 89.0)
		{
			localization::Text reason = numeric ? MustEqual("cl_pitchup", value, "89") : InvalidNumber("cl_pitchup");
			markInvalid(reason);
		}
	}
	else if (CS2AC_STREQI(pszCvarName, "cl_yawspeed"))
	{
		const bool numeric = utils::IsNumeric(pszCvarValue);
		const f64 value = numeric ? atof(pszCvarValue) : 0.0;
		if (!numeric || value != 210.0)
		{
			localization::Text reason = numeric ? MustEqual("cl_yawspeed", value, "210") : InvalidNumber("cl_yawspeed");
			markInvalid(reason);
		}
	}
	if (!invalid)
	{
		if (CS2AC_STREQI(pszCvarName, "m_yaw") || CS2AC_STREQI(pszCvarName, "sensitivity"))
		{
			player->movementDetection->MarkCvarSource(pszCvarName, {}, false, false);
		}
		else
		{
			player->movementDetection->MarkValidCvar(pszCvarName);
		}
	}
}

static_function void CheckUserInfoCvars(CS2ACPlayer *player)
{
	for (auto &name : userInfoCvarNames)
	{
		const char *value = interfaces::pEngine->GetClientConVarValue(player->GetPlayerSlot(), name);
		if (value == nullptr || CS2AC_STREQI(value, ""))
		{
			continue;
		}
		if (CS2AC_STREQI(name, "m_yaw"))
		{
			if (!utils::IsNumeric(value))
			{
				player->movementDetection->MarkCvarSource(name, InvalidNumber("m_yaw"), true, true);
			}
			else if (const f64 yaw = atof(value); yaw > MAXIMUM_M_YAW)
			{
				player->movementDetection->MarkCvarSource(name, AboveMaximum("m_yaw", yaw, "0.3"), true, true, true);
			}
			else
			{
				player->movementDetection->MarkCvarSource(name, {}, false, true);
			}
		}
		else if (CS2AC_STREQI(name, "sensitivity"))
		{
			const bool numeric = utils::IsNumeric(value);
			const f32 sens = numeric ? atof(value) : 0.0f;
			// The actual upper bound is 8.0f but this is to account for possible future updates.
			if (!numeric || sens < 0.0001f || sens > 20.0f)
			{
				localization::Text reason = numeric ? OutsideRange("sensitivity", sens, "0.0001", "20") : InvalidNumber("sensitivity");
				player->movementDetection->MarkCvarSource(name, reason, true, true);
			}
			else
			{
				player->movementDetection->MarkCvarSource(name, {}, false, true);
			}
		}
	}
}

static_function f64 CheckClientCvars(CPlayerUserId userID)
{
	CS2ACPlayer *player = g_pCS2ACPlayerManager->ToPlayer(userID);
	if (!player || !player->IsInGame())
	{
		return 0.0f;
	}
	if (!player->movementDetection->ShouldCheckClientCvars())
	{
		player->movementDetection->cvarQueryIndex = 0;
		player->movementDetection->cvarCycleInterval = 0.0;
		return RandomFloat(INTEGRITY_CHECK_MIN_INTERVAL, INTEGRITY_CHECK_MAX_INTERVAL);
	}
	auto *detection = player->movementDetection;
	if (detection->cvarQueryIndex >= std::size(cvarNames))
	{
		detection->cvarQueryIndex = 0;
	}
	if (detection->cvarQueryIndex == 0)
	{
		CheckUserInfoCvars(player);
		detection->cvarCycleInterval = RandomFloat(INTEGRITY_CHECK_MIN_INTERVAL, INTEGRITY_CHECK_MAX_INTERVAL);
	}
	if (g_pClientCvarValue)
	{
		g_pClientCvarValue->QueryCvarValue(player->GetPlayerSlot(), cvarNames[detection->cvarQueryIndex], ValidateQueriedCvar);
	}
	++detection->cvarQueryIndex;
	if (detection->cvarQueryIndex == std::size(cvarNames))
	{
		detection->cvarQueryIndex = 0;
	}
	return detection->cvarCycleInterval / std::size(cvarNames);
}

void MovementDetectionService::InitCvarMonitor()
{
	if (cvarMonitorStarted || this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	cvarMonitorStarted = true;
	StartTimer<CPlayerUserId>(CheckClientCvars, this->player->GetClient()->GetUserID(),
							  RandomFloat(INTEGRITY_CHECK_MIN_INTERVAL, INTEGRITY_CHECK_MAX_INTERVAL), true, true);
}
