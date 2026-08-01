#pragma once

#include "common.h"
#include "utils/interfaces.h"

class CGameConfig;
class CServerSideClient;
class IGameEventListener2;
class IRecipientFilter;

using GetLegacyGameEventListener_t = IGameEventListener2 *(CPlayerSlot slot);

class CS2ACUtils
{
public:
	CGlobalVars *GetGlobals();
	const CGlobalVars *GetServerGlobals();
	CUtlVector<CServerSideClient *> *GetClientList();
	CServerSideClient *GetClientBySlot(CPlayerSlot slot);
	IGameEventListener2 *GetLegacyGameEventListener(CPlayerSlot slot);
};

extern CS2ACUtils *g_pCS2ACUtils;
extern CGameConfig *g_pGameConfig;

namespace utils
{
	enum class DetectionOutcome : u8
	{
		PunishmentSent,
		Whitelisted,
		PunishmentDisabled,
		IdentityUnavailable,
		AlreadyPunished,
		CommandTooLong,
		CommandServiceUnavailable,
		NetworkUnstable,
	};

	void Initialize(std::vector<std::string> &missing);
	void Cleanup();

	f32 NormalizeDeg(f32 value);
	f32 GetAngleDifference(f32 source, f32 target, f32 circle, bool relative = false);

	inline bool IsNumeric(const char *str)
	{
		if (!str || !*str)
		{
			return false;
		}
		char *end = nullptr;
		const double value = strtod(str, &end);
		return end && *end == '\0' && std::isfinite(value);
	}

	bool CFormat(char *buffer, u64 bufferSize, const char *text);
	void ClientPrintFilter(IRecipientFilter *filter, int destination, const char *message, const char *param1, const char *param2, const char *param3,
						   const char *param4);
	void CPrintChatAll(const char *format, ...);
	void AnnounceDetection(const char *detection, const char *playerName, DetectionOutcome outcome);
	void AnnounceTest();
	void AnnounceWatermarkTo(CPlayerSlot slot, bool centerOnly);
	void ClearWatermarkFor(CPlayerSlot slot);
	bool IsDetectionAnnouncementActive();
	void ResetDetectionAnnouncement();
} // namespace utils

CGameEntitySystem *GameEntitySystem();
