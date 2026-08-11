#include "gameevents.pb.h"
#include "usermessages.pb.h"
#include "igameevents.h"
#include "public/networksystem/inetworkmessages.h"
#include "igameeventsystem.h"
#include "localization.h"
#include "sdk/datatypes.h"
#include "sdk/recipientfilters.h"
#include "settings.h"
#include "utils/ctimer.h"
#include "utils.h"

#include <cctype>

static_function char ConvertColorStringToByte(const char *str, size_t length)
{
	switch (length)
	{
		case 3:
			if (!V_memcmp(str, "red", length))
			{
				return 7;
			}
			break;
		case 4:
			if (!V_memcmp(str, "lime", length))
			{
				return 6;
			}
			if (!V_memcmp(str, "grey", length))
			{
				return 8;
			}
			if (!V_memcmp(str, "blue", length))
			{
				return 12;
			}
			break;
		case 7:
			if (!V_memcmp(str, "default", length))
			{
				return 1;
			}
			break;
	}
	return 0;
}

enum CFormatResult
{
	CFORMAT_NOT_US,
	CFORMAT_OK,
	CFORMAT_OUT_OF_SPACE,
};

struct CFormatContext
{
	const char *current;
	char *result;
	char *resultEnd;
};

static_function bool HasEnoughSpace(const CFormatContext *context, uintptr_t space)
{
	return static_cast<uintptr_t>(context->resultEnd - context->result) > space;
}

static_function CFormatResult EscapeChars(CFormatContext *context)
{
	if (*context->current != '{' || *(context->current + 1) != '{')
	{
		return CFORMAT_NOT_US;
	}
	if (!HasEnoughSpace(context, 1))
	{
		return CFORMAT_OUT_OF_SPACE;
	}
	context->current += 2;
	*context->result++ = '{';
	return CFORMAT_OK;
}

static_function CFormatResult ParseColors(CFormatContext *context)
{
	const char *current = context->current;
	if (*current != '{')
	{
		return CFORMAT_NOT_US;
	}
	const char *start = ++current;
	while (*current && *current != '}')
	{
		++current;
	}
	if (*current != '}')
	{
		return CFORMAT_NOT_US;
	}
	const char color = ConvertColorStringToByte(start, current - start);
	if (!color)
	{
		return CFORMAT_NOT_US;
	}
	if (!HasEnoughSpace(context, 1))
	{
		return CFORMAT_OUT_OF_SPACE;
	}
	*context->result++ = color;
	context->current = current + 1;
	return CFORMAT_OK;
}

bool utils::CFormat(char *buffer, u64 bufferSize, const char *text)
{
	assert(bufferSize != 0);
	CFormatContext context {text, buffer, buffer + bufferSize};
	if (!HasEnoughSpace(&context, 1))
	{
		return false;
	}
	*context.result++ = ' ';
	while (*context.current)
	{
		const CFormatResult escaped = EscapeChars(&context);
		if (escaped == CFORMAT_OK)
		{
			continue;
		}
		if (escaped == CFORMAT_OUT_OF_SPACE)
		{
			return false;
		}

		const CFormatResult color = ParseColors(&context);
		if (color == CFORMAT_OK)
		{
			continue;
		}
		if (color == CFORMAT_OUT_OF_SPACE || !HasEnoughSpace(&context, 1))
		{
			return false;
		}
		*context.result++ = *context.current++;
	}
	if (!HasEnoughSpace(&context, 1))
	{
		return false;
	}
	*context.result = '\0';
	return true;
}

void utils::ClientPrintFilter(IRecipientFilter *filter, int destination, const char *message, const char *param1, const char *param2,
							  const char *param3, const char *param4)
{
	auto *networkMessage = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	auto *textMessage = networkMessage->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	textMessage->set_dest(destination);
	textMessage->add_param(message);
	textMessage->add_param(param1);
	textMessage->add_param(param2);
	textMessage->add_param(param3);
	textMessage->add_param(param4);
	interfaces::pGameEventSystem->PostEventAbstract(0, false, filter, networkMessage, textMessage, 0);
	delete textMessage;
}

void utils::CPrintChatAll(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	char plain[512];
	vsnprintf(plain, sizeof(plain), format, arguments);
	va_end(arguments);

	char colored[512];
	if (!CFormat(colored, sizeof(colored), plain))
	{
		return;
	}
	CBroadcastRecipientFilter filter;
	ClientPrintFilter(&filter, HUD_PRINTTALK, colored, "", "", "", "");
}

static std::string EscapeChatMarkup(const char *text)
{
	std::string escaped;
	for (const unsigned char *current = reinterpret_cast<const unsigned char *>(text ? text : ""); *current; ++current)
	{
		if (*current < 0x20 || *current == 0x7f)
		{
			escaped += ' ';
		}
		else
		{
			escaped += static_cast<char>(*current);
		}
	}
	return escaped;
}

static std::string EscapeHtml(const char *text)
{
	std::string escaped;
	for (const unsigned char *current = reinterpret_cast<const unsigned char *>(text ? text : ""); *current; ++current)
	{
		switch (*current)
		{
			case '&':
				escaped += "&amp;";
				break;
			case '<':
				escaped += "&lt;";
				break;
			case '>':
				escaped += "&gt;";
				break;
			case '"':
				escaped += "&quot;";
				break;
			case '\'':
				escaped += "&#39;";
				break;
			default:
				if (*current < 0x20 || *current == 0x7f)
				{
					escaped += ' ';
				}
				else
				{
					escaped += static_cast<char>(*current);
				}
				break;
		}
	}
	return escaped;
}

static void ReplaceAll(std::string &value, std::string_view placeholder, std::string_view replacement)
{
	for (std::size_t position = 0; (position = value.find(placeholder, position)) != std::string::npos; position += replacement.size())
	{
		value.replace(position, placeholder.size(), replacement);
	}
}

static std::string detectionHtml;
static std::chrono::steady_clock::time_point detectionHtmlExpires;
static bool detectionHtmlTimerRunning;
static bool detectionHtmlIsDetection;
static bool detectionHtmlAlwaysVisible;
static int detectionHtmlBroadcasts;
static int detectionHtmlDuration = 5;
static constexpr int maximumDetectionHtmlBroadcasts = 51;

static void SendDetectionHtml(const char *message, int duration, IRecipientFilter *recipientFilter = nullptr)
{
	if (!interfaces::pGameEventManager || !interfaces::pGameEventSystem || !g_pNetworkMessages)
	{
		return;
	}
	auto *event = interfaces::pGameEventManager->CreateEvent("show_survival_respawn_status");
	auto *networkMessage = g_pNetworkMessages->FindNetworkMessageById(GE_Source1LegacyGameEvent);
	if (!event || !networkMessage)
	{
		if (event)
		{
			interfaces::pGameEventManager->FreeEvent(event);
		}
		return;
	}

	event->SetString("loc_token", message);
	event->SetInt("duration", duration);
	event->SetInt("userid", -1);

	auto *data = networkMessage->AllocateMessage()->ToPB<CMsgSource1LegacyGameEvent>();
	CBroadcastRecipientFilter broadcastFilter;
	IRecipientFilter *filter = recipientFilter ? recipientFilter : &broadcastFilter;
	interfaces::pGameEventManager->SerializeEvent(event, data);
	interfaces::pGameEventSystem->PostEventAbstract(-1, false, filter, networkMessage, data, 0);
	delete data;
	interfaces::pGameEventManager->FreeEvent(event);
}

static f64 ResendDetectionHtml()
{
	const auto now = std::chrono::steady_clock::now();
	if (!detectionHtmlAlwaysVisible && !settings::ShowCenterAnnouncements())
	{
		detectionHtml.clear();
		detectionHtmlTimerRunning = false;
		detectionHtmlIsDetection = false;
		detectionHtmlAlwaysVisible = false;
		SendDetectionHtml("<font></font>", 1);
		return 0.0;
	}
	if (detectionHtml.empty())
	{
		detectionHtmlTimerRunning = false;
		return 0.0;
	}
	if (now >= detectionHtmlExpires)
	{
		detectionHtml.clear();
		detectionHtmlTimerRunning = false;
		detectionHtmlIsDetection = false;
		detectionHtmlAlwaysVisible = false;
		SendDetectionHtml("<font></font>", 1);
		return 0.0;
	}
	if (detectionHtmlBroadcasts < maximumDetectionHtmlBroadcasts)
	{
		SendDetectionHtml(detectionHtml.c_str(), detectionHtmlDuration);
		++detectionHtmlBroadcasts;
	}
	return 0.1;
}

static void ShowCenterMessage(std::string message, bool isDetection = false, bool alwaysVisible = false, int duration = 5)
{
	const auto now = std::chrono::steady_clock::now();
	if (!isDetection && detectionHtmlIsDetection && now < detectionHtmlExpires)
	{
		return;
	}
	detectionHtml = std::move(message);
	detectionHtmlExpires = now + std::chrono::seconds(duration);
	detectionHtmlIsDetection = isDetection;
	detectionHtmlAlwaysVisible = alwaysVisible;
	detectionHtmlDuration = duration;
	SendDetectionHtml(detectionHtml.c_str(), detectionHtmlDuration);
	detectionHtmlBroadcasts = 1;
	if (!detectionHtmlTimerRunning)
	{
		detectionHtmlTimerRunning = true;
		StartTimer(ResendDetectionHtml, 0.1, false, true);
	}
}

static std::string DetectionOutcomeText(utils::DetectionOutcome outcome)
{
	switch (outcome)
	{
		case utils::DetectionOutcome::PunishmentSent:
			return localization::Get("announcement.outcome.punishment_sent", " and sent the punishment.");
		case utils::DetectionOutcome::Whitelisted:
			return localization::Get("announcement.outcome.whitelisted", ", but they are whitelisted.");
		case utils::DetectionOutcome::PunishmentDisabled:
			return localization::Get("announcement.outcome.punishment_disabled", ", but punishment is disabled.");
		case utils::DetectionOutcome::IdentityUnavailable:
			return localization::Get("announcement.outcome.identity_unavailable", ", but their identity is not ready yet.");
		case utils::DetectionOutcome::AlreadyPunished:
			return localization::Get("announcement.outcome.already_punished", " and the punishment was already sent.");
		case utils::DetectionOutcome::CommandTooLong:
			return localization::Get("announcement.outcome.command_too_long", ", but the punishment command is too long.");
		case utils::DetectionOutcome::CommandServiceUnavailable:
			return localization::Get("announcement.outcome.command_service_unavailable", ", but the server command service is unavailable.");
		case utils::DetectionOutcome::NetworkUnstable:
			return localization::Get("announcement.outcome.network_unstable", ", but no punishment was sent because their connection was unstable.");
	}
	return ".";
}

// The tag players read in chat and on the center screen. It is deliberately not the
// tag the console Msg() lines use: the Karasu platform parses "[CS2AC]" out of the
// server console log to drive detections and autobans, so the console keeps upstream's
// tag while everything a player actually sees is branded as ours.
static constexpr const char *inGameChatTag = "{red}[KarasuAC]{default} ";
static constexpr const char *inGameCenterOpen = "<span class='fontSize-l'><span color='#FF0000'>[KarasuAC]</span> <span color='#FFFFFF'>";
static constexpr const char *inGameCenterClose = "</span></span>";

void utils::AnnounceDetection(const char *detection, const char *playerName, DetectionOutcome outcome)
{
	if (!detection || !playerName)
	{
		return;
	}

	const std::string outcomeText = DetectionOutcomeText(outcome);
	std::string displayDetection = detection;
	for (char &character : displayDetection)
	{
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	}
	if (settings::ShowChatAnnouncements())
	{
		const std::string safeChatDetection = EscapeChatMarkup(displayDetection.c_str());
		const std::string safeChatPlayerName = std::string(1, '\x08') + EscapeChatMarkup(playerName);
		const std::string chatBody = localization::Format("announcement.detected", "detected {detection} on {player}{outcome}",
														  {{"detection", "{lime}%s1{default}"}, {"player", "{grey}%s2{default}"}, {"outcome", "%s3"}})
										 .localized;
		const std::string chatTemplate = inGameChatTag + chatBody;
		char coloredChat[512];
		if (CFormat(coloredChat, sizeof(coloredChat), chatTemplate.c_str()))
		{
			CBroadcastRecipientFilter filter;
			ClientPrintFilter(&filter, HUD_PRINTTALK, coloredChat, safeChatDetection.c_str(), safeChatPlayerName.c_str(), outcomeText.c_str(), "");
		}
	}

	if (!settings::ShowCenterAnnouncements())
	{
		return;
	}

	const std::string safeDetection = EscapeHtml(displayDetection.c_str());
	const std::string safePlayerName = EscapeHtml(playerName);
	std::string centerBody = EscapeHtml(localization::Get("announcement.detected", "detected {detection} on {player}{outcome}").c_str());
	ReplaceAll(centerBody, "{detection}", "<span color='#00FF00'>" + safeDetection + "</span>");
	ReplaceAll(centerBody, "{player}", "<span color='#B0B0B0'>" + safePlayerName + "</span>");
	ReplaceAll(centerBody, "{outcome}", EscapeHtml(outcomeText.c_str()));
	ShowCenterMessage(inGameCenterOpen + centerBody + inGameCenterClose, true);
}

void utils::AnnounceTest()
{
	static constexpr const char *text = "This is a test. No player was detected or punished.";
	const std::string localized = localization::Get("announcement.test", text);
	if (settings::ShowChatAnnouncements())
	{
		const std::string chatTemplate = inGameChatTag + localized;
		char coloredChat[256];
		if (CFormat(coloredChat, sizeof(coloredChat), chatTemplate.c_str()))
		{
			CBroadcastRecipientFilter filter;
			ClientPrintFilter(&filter, HUD_PRINTTALK, coloredChat, "", "", "", "");
		}
	}
	else
	{
		Msg("[CS2AC] The chat test was skipped because chat announcements are disabled.\n");
	}

	if (settings::ShowCenterAnnouncements())
	{
		ShowCenterMessage(inGameCenterOpen + EscapeHtml(localized.c_str()) + inGameCenterClose);
	}
	else
	{
		Msg("[CS2AC] The center-screen test was skipped because center announcements are disabled.\n");
	}
	Msg("[CS2AC] Test announcement finished: %s\n", text);
}

void utils::AnnounceWatermarkTo(CPlayerSlot slot, bool centerOnly)
{
	CSingleRecipientFilter filter(slot);
	if (!centerOnly)
	{
		const auto printChat = [&](const std::string &body, const char *param = "")
		{
			const std::string message = inGameChatTag + body;
			char colored[256];
			if (CFormat(colored, sizeof(colored), message.c_str()))
			{
				ClientPrintFilter(&filter, HUD_PRINTTALK, colored, param, "", "", "");
			}
		};
		printChat(localization::Watermark({{"author", "{grey}%s1{default}"}}).localized, "karola3vax");
		printChat(localization::Format("announcement.support", "Support development: {blue}{url}{default}", {{"url", "buymeacoffee.com/karola3vax"}})
					  .localized);
		return;
	}

	std::string centerBody = EscapeHtml(localization::Watermark().localized.c_str());
	ReplaceAll(centerBody, "{author}", "<span color='#B0B0B0'>karola3vax</span>");
	const std::string message = inGameCenterOpen + centerBody + inGameCenterClose;
	SendDetectionHtml(message.c_str(), 3, &filter);
}

void utils::ClearWatermarkFor(CPlayerSlot slot)
{
	CSingleRecipientFilter filter(slot);
	SendDetectionHtml("<font></font>", 1, &filter);
}

bool utils::IsDetectionAnnouncementActive()
{
	return detectionHtmlIsDetection && std::chrono::steady_clock::now() < detectionHtmlExpires;
}

void utils::ResetDetectionAnnouncement()
{
	if (!detectionHtml.empty())
	{
		SendDetectionHtml("<font></font>", 1);
	}
	detectionHtml.clear();
	detectionHtmlExpires = {};
	detectionHtmlTimerRunning = false;
	detectionHtmlIsDetection = false;
	detectionHtmlAlwaysVisible = false;
	detectionHtmlBroadcasts = 0;
}
