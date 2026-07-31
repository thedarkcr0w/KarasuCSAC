#include "webhook.h"

#include "localization.h"
#include "movement_analysis/player_context.h"
#include "settings.h"
#include "utils/utils.h"
#include "version_gen.h"

#include <charconv>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
	constexpr std::size_t maximumQueueSize = 64;
	constexpr std::size_t maximumAvatarCacheSize = 256;
	constexpr uint32 maximumProfileSize = 64 * 1024;

	std::string JsonEscape(std::string_view value)
	{
		std::string result;
		result.reserve(value.size() + 8);
		for (const unsigned char character : value)
		{
			switch (character)
			{
				case '"':
					result += "\\\"";
					break;
				case '\\':
					result += "\\\\";
					break;
				case '\b':
					result += "\\b";
					break;
				case '\f':
					result += "\\f";
					break;
				case '\n':
					result += "\\n";
					break;
				case '\r':
					result += "\\r";
					break;
				case '\t':
					result += "\\t";
					break;
				default:
					if (character < 32 || character == 127)
					{
						result += '?';
					}
					else
					{
						result += static_cast<char>(character);
					}
			}
		}
		return result;
	}

	std::string MarkdownEscape(std::string_view value)
	{
		std::string result;
		result.reserve(value.size() + 8);
		for (const char character : value)
		{
			if (strchr("\\`*_{}[]()<>#+-.!|~", character))
			{
				result += '\\';
			}
			result += character;
		}
		return result;
	}

	std::string Limit(std::string value, std::size_t maximum)
	{
		if (value.size() <= maximum)
		{
			return value;
		}
		value.resize(maximum - 3);
		while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0) == 0x80)
		{
			value.pop_back();
		}
		value += "...";
		return value;
	}

	std::string OutcomeText(utils::DetectionOutcome outcome)
	{
		switch (outcome)
		{
			case utils::DetectionOutcome::PunishmentSent:
				return localization::Get("webhook.outcome.punishment_sent", "The configured punishment command was sent.");
			case utils::DetectionOutcome::Whitelisted:
				return localization::Get("webhook.outcome.whitelisted", "The player is whitelisted, so no punishment command was sent.");
			case utils::DetectionOutcome::PunishmentDisabled:
				return localization::Get("webhook.outcome.punishment_disabled",
										 "No punishment was sent because this punishment command is disabled.");
			case utils::DetectionOutcome::IdentityUnavailable:
				return localization::Get("webhook.outcome.identity_unavailable",
										 "No punishment was sent because the player's identity was not ready.");
			case utils::DetectionOutcome::AlreadyPunished:
				return localization::Get("webhook.outcome.already_punished", "The player was already punished, so no duplicate command was sent.");
			case utils::DetectionOutcome::CommandTooLong:
				return localization::Get("webhook.outcome.command_too_long", "No punishment was sent because the configured command is too long.");
			case utils::DetectionOutcome::CommandServiceUnavailable:
				return localization::Get("webhook.outcome.command_service_unavailable",
										 "No punishment was sent because the server command service is unavailable.");
			case utils::DetectionOutcome::NetworkUnstable:
				return localization::Get("webhook.outcome.network_unstable",
										 "No punishment was sent because the player's connection exceeded the safe network limits.");
		}
		return localization::Get("webhook.outcome.unavailable", "No punishment outcome was available.");
	}

	std::string CompleteSentence(std::string value)
	{
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
		{
			value.pop_back();
		}
		if (!value.empty() && !strchr(".!?", value.back()))
		{
			value += '.';
		}
		return value;
	}

	std::string AvatarFromProfileXml(std::string_view xml)
	{
		constexpr std::string_view opening = "<avatarFull><![CDATA[";
		constexpr std::string_view closing = "]]></avatarFull>";
		const std::size_t start = xml.find(opening);
		if (start == std::string_view::npos)
		{
			return "";
		}
		const std::size_t valueStart = start + opening.size();
		const std::size_t end = xml.find(closing, valueStart);
		if (end == std::string_view::npos)
		{
			return "";
		}
		const std::string url(xml.substr(valueStart, end - valueStart));
		const bool steamStatic = url.rfind("https://avatars.", 0) == 0 && url.find(".steamstatic.com/") != std::string::npos;
		const bool steamCdn = url.rfind("https://steamcdn-a.akamaihd.net/steamcommunity/public/images/avatars/", 0) == 0;
		return steamStatic || steamCdn ? url : "";
	}

	std::string UtcTimestamp()
	{
		const std::time_t now = std::time(nullptr);
		std::tm utc {};
#ifdef _WIN32
		gmtime_s(&utc, &now);
#else
		gmtime_r(&now, &utc);
#endif
		std::ostringstream output;
		output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
		return output.str();
	}

	std::string ConVarString(const char *name, const char *fallback)
	{
		ConVarRefAbstract value(name, true);
		return value.IsValidRef() && value.IsConVarDataAvailable() ? value.GetString().Get() : fallback;
	}

	int ConVarInt(const char *name, int fallback)
	{
		ConVarRefAbstract value(name, true);
		return value.IsValidRef() && value.IsConVarDataAvailable() ? value.GetInt() : fallback;
	}
} // namespace

bool WebhookService::IsValidUrl(const char *url)
{
	if (!url || !*url)
	{
		return true;
	}
	const std::string_view value(url);
	const std::string_view currentPrefix = "https://discord.com/api/webhooks/";
	const std::string_view legacyPrefix = "https://discordapp.com/api/webhooks/";
	return ((value.size() >= currentPrefix.size() && value.substr(0, currentPrefix.size()) == currentPrefix)
			|| (value.size() >= legacyPrefix.size() && value.substr(0, legacyPrefix.size()) == legacyPrefix))
		   && value.find_first_of(" \t\r\n") == std::string_view::npos;
}

bool WebhookService::IsValidRoleId(const char *roleId)
{
	if (!roleId || !*roleId)
	{
		return true;
	}
	const std::string_view value(roleId);
	return std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isdigit(character); });
}

bool WebhookService::IsValidLogoUrl(const char *url)
{
	if (!url || !*url)
	{
		return true;
	}
	const std::string_view value(url);
	constexpr std::string_view prefix = "https://";
	return value.size() > prefix.size() && value.substr(0, prefix.size()) == prefix && value.find_first_of(" \t\r\n") == std::string_view::npos;
}

bool WebhookService::IsConfigured() const
{
	const char *url = settings::GetWebhookUrl();
	return url && *url;
}

void WebhookService::Reload()
{
	CancelRequest();
	queue.clear();
	avatarCache.clear();
	disabled = false;
	overflowWarned = false;
	httpUnavailableWarned = false;
	nextAttempt = {};
	if (!IsConfigured())
	{
		return;
	}
	if (!IsValidUrl(settings::GetWebhookUrl()))
	{
		Disable("cs2ac_webhook_url is not a Discord webhook URL.");
		return;
	}
	if (!IsValidRoleId(settings::GetWebhookRoleId()))
	{
		Disable("cs2ac_webhook_role_id must contain only numbers.");
		return;
	}
	if (!IsValidLogoUrl(settings::GetWebhookLogoUrl()))
	{
		Disable("cs2ac_webhook_logo_url must be an HTTPS URL.");
		return;
	}
}

void WebhookService::Unload()
{
	CancelRequest();
	queue.clear();
	avatarCache.clear();
	http = nullptr;
	steamContext.Clear();
	disabled = false;
	overflowWarned = false;
	httpUnavailableWarned = false;
	nextAttempt = {};
}

void WebhookService::CancelRequest()
{
	callResult.Cancel();
	if (request != INVALID_HTTPREQUEST_HANDLE && http)
	{
		http->ReleaseHTTPRequest(request);
	}
	request = INVALID_HTTPREQUEST_HANDLE;
	requestKind = RequestKind::None;
}

void WebhookService::Report(const char *detection, MovementPlayer *player, std::string_view evidence, utils::DetectionOutcome outcome)
{
	if (!IsConfigured() || disabled || !detection || !player)
	{
		return;
	}
	if (queue.size() >= maximumQueueSize)
	{
		queue.erase(queue.begin() + (request == INVALID_HTTPREQUEST_HANDLE ? 0 : 1));
		if (!overflowWarned)
		{
			Msg("[CS2AC] The Discord webhook queue filled up. The oldest report was dropped so gameplay stays unaffected.\n");
			overflowWarned = true;
		}
	}
	ReportData report;
	report.detection = detection;
	report.playerName = player->GetName() ? player->GetName() : localization::Get("webhook.unknown_player", "Unknown player");
	report.evidence = evidence;
	report.steamId = player->GetSteamId64(false);
	report.outcome = outcome;
	const auto cachedAvatar = avatarCache.find(report.steamId);
	if (!report.steamId || cachedAvatar != avatarCache.end())
	{
		report.avatarResolved = true;
		if (cachedAvatar != avatarCache.end())
		{
			report.avatarUrl = cachedAvatar->second;
		}
	}
	queue.push_back(std::move(report));
}

void WebhookService::Test()
{
	if (!IsConfigured())
	{
		Msg("[CS2AC] The Discord webhook test was not sent because cs2ac_webhook_url is empty.\n");
		return;
	}
	if (disabled)
	{
		Msg("[CS2AC] The Discord webhook is disabled after an error. Fix it and run cs2ac_reload before testing again.\n");
		return;
	}
	if (queue.size() >= maximumQueueSize)
	{
		queue.erase(queue.begin() + (request == INVALID_HTTPREQUEST_HANDLE ? 0 : 1));
	}
	ReportData report;
	report.detection = localization::Get("webhook.test_detection", "WEBHOOK TEST");
	report.playerName = localization::Get("webhook.test_player", "Webhook test");
	report.evidence = localization::Get("webhook.test_evidence", "This is a harmless test report. No detection or punishment was created.");
	report.outcome = utils::DetectionOutcome::PunishmentDisabled;
	report.avatarResolved = true;
	queue.push_back(std::move(report));
	Msg("[CS2AC] The Discord webhook test was queued.\n");
}

void WebhookService::OnGameFrame()
{
	if (request == INVALID_HTTPREQUEST_HANDLE && !queue.empty() && std::chrono::steady_clock::now() >= nextAttempt)
	{
		SendNext();
	}
}

void WebhookService::SendNext()
{
	if (!http)
	{
		if (!steamContext.Init() || !(http = steamContext.SteamHTTP()))
		{
			if (!httpUnavailableWarned)
			{
				Msg("[CS2AC] Discord reports are waiting because Steam's HTTP service is not ready yet.\n");
				httpUnavailableWarned = true;
			}
			nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			return;
		}
		httpUnavailableWarned = false;
	}

	if (!queue.front().avatarResolved)
	{
		const auto cachedAvatar = avatarCache.find(queue.front().steamId);
		if (cachedAvatar != avatarCache.end())
		{
			queue.front().avatarResolved = true;
			queue.front().avatarUrl = cachedAvatar->second;
		}
		else if (SendAvatarLookup())
		{
			return;
		}
	}
	if (queue.front().payload.empty())
	{
		queue.front().payload = BuildPayload(queue.front());
	}

	std::string url = settings::GetWebhookUrl();
	url += url.find('?') == std::string::npos ? "?wait=true" : "&wait=true";
	request = http->CreateHTTPRequest(k_EHTTPMethodPOST, url.c_str());
	if (request == INVALID_HTTPREQUEST_HANDLE
		|| !http->SetHTTPRequestRawPostBody(request, "application/json", reinterpret_cast<uint8 *>(queue.front().payload.data()),
											static_cast<uint32>(queue.front().payload.size()))
		|| !http->SetHTTPRequestNetworkActivityTimeout(request, 5) || !http->SetHTTPRequestAbsoluteTimeoutMS(request, 10000)
		|| !http->SetHTTPRequestRequiresVerifiedCertificate(request, true))
	{
		if (request != INVALID_HTTPREQUEST_HANDLE)
		{
			http->ReleaseHTTPRequest(request);
			request = INVALID_HTTPREQUEST_HANDLE;
		}
		RetryOrDrop(0);
		return;
	}

	SteamAPICall_t call {};
	if (!http->SendHTTPRequest(request, &call))
	{
		http->ReleaseHTTPRequest(request);
		request = INVALID_HTTPREQUEST_HANDLE;
		RetryOrDrop(0);
		return;
	}
	requestKind = RequestKind::Discord;
	callResult.SetGameserverFlag();
	callResult.Set(call, this, &WebhookService::OnCompleted);
}

bool WebhookService::SendAvatarLookup()
{
	// Steam's public XML profile supplies avatarFull without requiring a Web API key.
	const std::string url = tfm::format("https://steamcommunity.com/profiles/%llu?xml=1", static_cast<unsigned long long>(queue.front().steamId));
	request = http->CreateHTTPRequest(k_EHTTPMethodGET, url.c_str());
	if (request == INVALID_HTTPREQUEST_HANDLE || !http->SetHTTPRequestNetworkActivityTimeout(request, 5)
		|| !http->SetHTTPRequestAbsoluteTimeoutMS(request, 5000) || !http->SetHTTPRequestRequiresVerifiedCertificate(request, true))
	{
		if (request != INVALID_HTTPREQUEST_HANDLE)
		{
			http->ReleaseHTTPRequest(request);
			request = INVALID_HTTPREQUEST_HANDLE;
		}
		FinishAvatarLookup("");
		return false;
	}

	SteamAPICall_t call {};
	if (!http->SendHTTPRequest(request, &call))
	{
		http->ReleaseHTTPRequest(request);
		request = INVALID_HTTPREQUEST_HANDLE;
		FinishAvatarLookup("");
		return false;
	}
	requestKind = RequestKind::Avatar;
	callResult.SetGameserverFlag();
	callResult.Set(call, this, &WebhookService::OnCompleted);
	return true;
}

void WebhookService::OnCompleted(HTTPRequestCompleted_t *result, bool failed)
{
	if (!result || result->m_hRequest != request)
	{
		return;
	}
	const int status = static_cast<int>(result->m_eStatusCode);
	if (requestKind == RequestKind::Avatar)
	{
		std::string avatarUrl;
		uint32 size {};
		if (!failed && result->m_bRequestSuccessful && status >= 200 && status <= 299 && http->GetHTTPResponseBodySize(request, &size) && size > 0
			&& size <= maximumProfileSize)
		{
			std::vector<uint8> body(size);
			if (http->GetHTTPResponseBodyData(request, body.data(), size))
			{
				avatarUrl = AvatarFromProfileXml(std::string_view(reinterpret_cast<const char *>(body.data()), body.size()));
			}
		}
		http->ReleaseHTTPRequest(request);
		request = INVALID_HTTPREQUEST_HANDLE;
		requestKind = RequestKind::None;
		FinishAvatarLookup(std::move(avatarUrl));
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	double retrySeconds = 1.0;
	if (status == 429)
	{
		uint32 size {};
		if (http->GetHTTPResponseHeaderSize(request, "Retry-After", &size) && size > 0 && size < 64)
		{
			std::vector<uint8> value(size + 1);
			if (http->GetHTTPResponseHeaderValue(request, "Retry-After", value.data(), size))
			{
				value[size] = '\0';
				char *end = nullptr;
				const double parsed = std::strtod(reinterpret_cast<const char *>(value.data()), &end);
				if (end && *end == '\0' && std::isfinite(parsed))
				{
					retrySeconds = std::clamp(parsed, 0.1, 3600.0);
				}
			}
		}
		nextAttempt = now + std::chrono::milliseconds(static_cast<int>(retrySeconds * 1000.0));
	}
	else if (!failed && result->m_bRequestSuccessful && status >= 200 && status <= 299)
	{
		queue.pop_front();
		overflowWarned = false;
		nextAttempt = {};
	}
	else if (status == 401 || status == 403 || status == 404)
	{
		Disable("Discord rejected the webhook. It may be invalid or deleted.");
	}
	else if (!queue.empty() && queue.front().retries++ == 0)
	{
		nextAttempt = now + std::chrono::seconds(2);
	}
	else
	{
		Msg("[CS2AC] A Discord webhook report failed after one retry and was dropped (HTTP %d).\n", status);
		queue.pop_front();
		nextAttempt = {};
	}
	http->ReleaseHTTPRequest(request);
	request = INVALID_HTTPREQUEST_HANDLE;
	requestKind = RequestKind::None;
}

void WebhookService::FinishAvatarLookup(std::string avatarUrl)
{
	if (queue.empty())
	{
		return;
	}
	auto &report = queue.front();
	report.avatarResolved = true;
	report.avatarUrl = std::move(avatarUrl);
	if (avatarCache.size() >= maximumAvatarCacheSize)
	{
		avatarCache.clear();
	}
	avatarCache[report.steamId] = report.avatarUrl;
}

void WebhookService::RetryOrDrop(int status)
{
	if (!queue.empty() && queue.front().retries++ == 0)
	{
		nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		return;
	}
	Msg("[CS2AC] A Discord webhook report failed after one retry and was dropped%s.\n", status ? tfm::format(" (HTTP %d)", status).c_str() : "");
	if (!queue.empty())
	{
		queue.pop_front();
	}
	nextAttempt = {};
}

void WebhookService::Disable(const char *reason)
{
	disabled = true;
	queue.clear();
	Msg("[CS2AC] Discord webhook reports are disabled until the next cs2ac_reload. %s\n", reason);
}

std::string WebhookService::ServerAddress()
{
	const char *overrideAddress = settings::GetWebhookServerAddress();
	if (overrideAddress && *overrideAddress)
	{
		return overrideAddress;
	}
	auto *gameServer = steamContext.SteamGameServer();
	if (!gameServer && steamContext.Init())
	{
		gameServer = steamContext.SteamGameServer();
	}
	if (!gameServer)
	{
		return "";
	}
	const auto ip = gameServer->GetPublicIP();
	if (!ip.IsSet())
	{
		return "";
	}
	if (ip.m_eType != k_ESteamIPTypeIPv4)
	{
		return "";
	}
	const uint32 value = ip.m_unIPv4;
	return tfm::format("%u.%u.%u.%u:%d", (value >> 24) & 255, (value >> 16) & 255, (value >> 8) & 255, value & 255, ConVarInt("hostport", 27015));
}

std::string WebhookService::BuildPayload(const ReportData &report)
{
	const std::string name = Limit(MarkdownEscape(report.playerName), 240);
	const std::string playerValue =
		report.steamId ? tfm::format("[%s](https://steamcommunity.com/profiles/%llu)", name, static_cast<unsigned long long>(report.steamId)) : name;
	const std::string steamIdValue = report.steamId ? tfm::format("||%llu||", static_cast<unsigned long long>(report.steamId))
													: localization::Get("webhook.steamid_unavailable", "SteamID64 is unavailable.");
	const auto *globals = g_pCS2ACUtils ? g_pCS2ACUtils->GetServerGlobals() : nullptr;
	const std::string map =
		globals && globals->mapname.ToCStr() ? globals->mapname.ToCStr() : localization::Get("webhook.unavailable", "Unavailable");
	const std::string role = settings::GetWebhookRoleId();
	const std::string content = role.empty() ? "" : "<@&" + role + ">";
	const std::string allowed = role.empty() ? "{\"parse\":[]}" : "{\"parse\":[],\"roles\":[\"" + role + "\"]}";
	const std::string logo = settings::GetWebhookLogoUrl();
	const std::string thumbnailUrl = report.avatarUrl.empty() ? logo : report.avatarUrl;
	const std::string thumbnail = thumbnailUrl.empty() ? "" : tfm::format(",\"thumbnail\":{\"url\":\"%s\"}", JsonEscape(Limit(thumbnailUrl, 2048)));
	const std::string authorIcon = logo.empty() ? "" : tfm::format(",\"icon_url\":\"%s\"", JsonEscape(Limit(logo, 2048)));
	const std::string footerIcon = logo.empty() ? "" : tfm::format(",\"icon_url\":\"%s\"", JsonEscape(Limit(logo, 2048)));
	const std::string address = ServerAddress();
	const std::string addressField =
		address.empty() ? ""
						: tfm::format(",{\"name\":\"%s\",\"value\":\"`%s`\",\"inline\":true}",
									  JsonEscape(localization::Get("webhook.field.address", "Address")), JsonEscape(Limit(address, 512)));
	const std::string evidenceText = Limit(
		CompleteSentence(report.evidence.empty() ? localization::Get("webhook.default_evidence", "The detector reached its configured threshold")
												 : report.evidence),
		1000);
	const std::string serverName = Limit(ConVarString("hostname", localization::Get("webhook.default_server_name", "CS2 Server").c_str()), 256);
	const std::string footer = tfm::format("CS2AC %s • %s", PLUGIN_FULL_VERSION, localization::Get("webhook.footer", "Detection report"));

	return tfm::format(
		"{\"username\":\"CS2AC\",\"content\":\"%s\",\"allowed_mentions\":%s,"
		"\"embeds\":[{\"author\":{\"name\":\"%s\"%s},\"color\":15548997%s,\"fields\":["
		"{\"name\":\"%s\",\"value\":\"%s\",\"inline\":true},"
		"{\"name\":\"%s\",\"value\":\"%s\",\"inline\":true},"
		"{\"name\":\"%s\",\"value\":\"`%s`\"},"
		"{\"name\":\"%s\",\"value\":\"```text\\n%s\\n```\"},"
		"{\"name\":\"%s\",\"value\":\"%s\"},"
		"{\"name\":\"%s\",\"value\":\"`%s`\",\"inline\":true}%s],"
		"\"timestamp\":\"%s\",\"footer\":{\"text\":\"%s\"%s},"
		"\"image\":{\"url\":\"https://raw.githubusercontent.com/karola3vax/CS2AC/main/docs/cs2ac-logo.png\"}}]}",
		JsonEscape(content), allowed, JsonEscape(serverName), authorIcon, thumbnail, JsonEscape(localization::Get("webhook.field.player", "Player")),
		JsonEscape(playerValue), JsonEscape(localization::Get("webhook.field.steamid64", "STEAMID64")), JsonEscape(steamIdValue),
		JsonEscape(localization::Get("webhook.field.detection", "Detection")),
		JsonEscape(Limit(report.detection.empty() ? localization::Get("webhook.unknown_detection", "UNKNOWN") : report.detection, 256)),
		JsonEscape(localization::Get("webhook.field.evidence", "Evidence")), JsonEscape(evidenceText),
		JsonEscape(localization::Get("webhook.field.punishment", "Punishment")), JsonEscape(OutcomeText(report.outcome)),
		JsonEscape(localization::Get("webhook.field.map", "Map")), JsonEscape(Limit(map, 256)), addressField, UtcTimestamp(), JsonEscape(footer),
		footerIcon);
}
