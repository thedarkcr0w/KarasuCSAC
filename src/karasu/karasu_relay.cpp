#include "karasu_relay.h"

#include "../common.h"
#include "../settings.h"
#include "../utils/interfaces.h"
// Generated into the build's versioning/ directory at configure time, so this
// resolves through the include path rather than relative to this file.
#include "version_gen.h"

#include "convar.h"
#include "eiface.h"

#include <chrono>

namespace
{
	constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

	// Evidence prose is the only unbounded field in the payload, so it is the first
	// thing sacrificed when the console command length cap is in danger.
	constexpr std::size_t kEvidenceBudget = 720;
	constexpr std::size_t kEvidenceMinimum = 120;

	std::uint64_t sequence {};
	std::uint64_t bootNonce {};
	std::uint64_t emitted {};
	std::uint64_t dropped {};
	std::uint64_t truncated {};

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
					if (character < 0x20)
					{
						char escape[7] {};
						V_snprintf(escape, sizeof(escape), "\\u%04x", character);
						result += escape;
					}
					else
					{
						result += static_cast<char>(character);
					}
					break;
			}
		}
		return result;
	}

	// RFC 4648 section 5, padding stripped: the payload travels as a single console
	// argument, so it must not contain '+', '/', '=' or whitespace.
	std::string Base64Url(std::string_view value)
	{
		std::string result;
		result.reserve(((value.size() + 2) / 3) * 4);

		std::size_t index = 0;
		while (index + 2 < value.size())
		{
			const auto first = static_cast<unsigned char>(value[index]);
			const auto second = static_cast<unsigned char>(value[index + 1]);
			const auto third = static_cast<unsigned char>(value[index + 2]);
			const std::uint32_t triple = (static_cast<std::uint32_t>(first) << 16) | (static_cast<std::uint32_t>(second) << 8) | third;
			result += kBase64Alphabet[(triple >> 18) & 0x3F];
			result += kBase64Alphabet[(triple >> 12) & 0x3F];
			result += kBase64Alphabet[(triple >> 6) & 0x3F];
			result += kBase64Alphabet[triple & 0x3F];
			index += 3;
		}

		const std::size_t remaining = value.size() - index;
		if (remaining == 1)
		{
			const auto first = static_cast<unsigned char>(value[index]);
			const std::uint32_t triple = static_cast<std::uint32_t>(first) << 16;
			result += kBase64Alphabet[(triple >> 18) & 0x3F];
			result += kBase64Alphabet[(triple >> 12) & 0x3F];
		}
		else if (remaining == 2)
		{
			const auto first = static_cast<unsigned char>(value[index]);
			const auto second = static_cast<unsigned char>(value[index + 1]);
			const std::uint32_t triple = (static_cast<std::uint32_t>(first) << 16) | (static_cast<std::uint32_t>(second) << 8);
			result += kBase64Alphabet[(triple >> 18) & 0x3F];
			result += kBase64Alphabet[(triple >> 12) & 0x3F];
			result += kBase64Alphabet[(triple >> 6) & 0x3F];
		}

		return result;
	}

	std::string Hex64(std::uint64_t value)
	{
		char buffer[17] {};
		V_snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
		return buffer;
	}

	// Stable within a boot, unique across boots. Combined with the sequence number
	// this gives the platform an idempotency key, so a relay that gets re-sent
	// cannot produce a second ban.
	void EnsureNonce()
	{
		if (bootNonce != 0)
		{
			return;
		}
		const auto now = std::chrono::system_clock::now().time_since_epoch();
		const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
		bootNonce = static_cast<std::uint64_t>(nanoseconds);
		// Mix in an address so two servers booting in the same nanosecond still differ.
		bootNonce ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&sequence)) * 0x9E3779B97F4A7C15ull;
		if (bootNonce == 0)
		{
			bootNonce = 0x4B41524153552121ull;
		}
	}

	std::string BuildPayload(const karasu::RelayReport &report, const std::string &eventId, std::size_t evidenceBudget, bool &didTruncate)
	{
		std::string evidence = JsonEscape(report.evidence);
		didTruncate = false;
		if (evidence.size() > evidenceBudget)
		{
			// Cut on the escaped string, then step back off a trailing backslash so a
			// truncated escape sequence cannot corrupt the JSON.
			evidence.resize(evidenceBudget);
			while (!evidence.empty() && evidence.back() == '\\')
			{
				evidence.pop_back();
			}
			didTruncate = true;
		}

		const karasu::Verdict &verdict = report.verdict;

		std::string payload;
		payload.reserve(evidence.size() + 640);
		payload += "{\"source\":\"karasu-cs2ac\"";
		payload += ",\"pluginVersion\":\"karasucsac:";
		payload += JsonEscape(PLUGIN_FULL_VERSION);
		payload += "\",\"steamId\":\"";
		payload += std::to_string(report.steamId);
		payload += "\",\"userid\":";
		payload += std::to_string(report.userId);
		payload += ",\"detectionType\":\"cs2ac:";
		payload += JsonEscape(report.detectionName);
		payload += "\",\"severity\":\"";
		payload += karasu::SeverityForTier(report.policy.tier);
		payload += "\",\"detectorTier\":\"";
		payload += karasu::TierName(report.policy.tier);
		payload += "\",\"detectorFamily\":\"";
		payload += karasu::FamilyName(report.policy.family);
		payload += "\",\"confidence\":";
		payload += std::to_string(verdict.confidence);
		payload += ",\"deterministic\":";
		payload += report.policy.tier == karasu::Tier::A ? "true" : "false";
		payload += ",\"recommendedAction\":\"";
		payload += karasu::ActionName(verdict.action);
		payload += "\",\"action\":\"";
		payload += karasu::ActionName(report.localAction);
		payload += "\",\"identitySource\":\"";
		payload += karasu::IdentitySourceName(report.identity);
		payload += "\",\"idempotencyKey\":\"";
		payload += eventId;
		payload += "\",\"rule\":\"";
		payload += verdict.rule;
		payload += "\",\"corroboratingUnits\":";
		payload += std::to_string(verdict.corroboratingUnits);
		if (verdict.hasCorroboratingType)
		{
			payload += ",\"corroboratingType\":\"cs2ac:";
			payload += JsonEscape(karasu::DetectionDisplayName(verdict.corroboratingType));
			payload += "\"";
		}
		payload += ",\"serverTick\":";
		payload += std::to_string(report.serverTick);
		payload += ",\"playerName\":\"";
		payload += JsonEscape(report.playerName);
		payload += "\",\"evidence\":\"";
		payload += evidence;
		payload += "\"}";
		return payload;
	}
} // namespace

const char *karasu::IdentitySourceName(IdentitySource source)
{
	switch (source)
	{
		case IdentitySource::Authenticated:
			return "authenticated";
		case IdentitySource::Controller:
			return "controller";
		case IdentitySource::Unauthenticated:
			break;
	}
	return "unauthenticated";
}

void karasu::relay::Reset()
{
	sequence = 0;
	bootNonce = 0;
	emitted = 0;
	dropped = 0;
	truncated = 0;
}

std::uint64_t karasu::relay::EmittedCount()
{
	return emitted;
}

std::uint64_t karasu::relay::DroppedCount()
{
	return dropped;
}

std::uint64_t karasu::relay::TruncatedCount()
{
	return truncated;
}

bool karasu::relay::Emit(const RelayReport &report)
{
	if (!settings::IsKarasuRelayEnabled())
	{
		return false;
	}

	const char *command = settings::GetKarasuRelayCommand();
	if (!command || !*command)
	{
		++dropped;
		return false;
	}

	if (!interfaces::pEngine)
	{
		++dropped;
		Msg("[CS2AC] Karasu relay skipped: the server command service is unavailable.\n");
		return false;
	}

	EnsureNonce();
	const std::string eventId = Hex64(bootNonce) + Hex64(++sequence);

	// The engine drops a console command longer than its cap, and base64 inflates by
	// 4/3, so the payload has to be measured before it is sent rather than after.
	const std::size_t maximumLine = static_cast<std::size_t>(CCommand::MaxCommandLength());
	const std::size_t prefix = V_strlen(command) + 1;
	if (maximumLine <= prefix + 8)
	{
		++dropped;
		return false;
	}
	const std::size_t maximumEncoded = maximumLine - prefix - 8;
	const std::size_t maximumPayload = (maximumEncoded / 4) * 3;

	bool didTruncate = false;
	std::size_t budget = kEvidenceBudget;
	std::string payload = BuildPayload(report, eventId, budget, didTruncate);
	while (payload.size() > maximumPayload && budget > kEvidenceMinimum)
	{
		budget = budget / 2 < kEvidenceMinimum ? kEvidenceMinimum : budget / 2;
		payload = BuildPayload(report, eventId, budget, didTruncate);
	}
	if (payload.size() > maximumPayload)
	{
		// Last resort: drop the prose entirely. The machine-readable fields - who,
		// what, tier, confidence, recommended action - are what the platform acts on
		// and they must never be the thing that gets cut.
		payload = BuildPayload(report, eventId, 0, didTruncate);
		didTruncate = true;
	}
	if (payload.size() > maximumPayload)
	{
		++dropped;
		Msg("[CS2AC] Karasu relay dropped a report for %s: the payload does not fit in one console command.\n",
			report.playerName ? report.playerName : "<unknown>");
		return false;
	}

	if (didTruncate)
	{
		++truncated;
	}

	std::string line = command;
	line += ' ';
	line += Base64Url(payload);
	line += '\n';
	interfaces::pEngine->ServerCommand(line.c_str());
	++emitted;
	return true;
}
