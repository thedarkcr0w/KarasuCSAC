#include "karasu_relay.h"

#include "../common.h"
#include "../settings.h"
#include "../utils/interfaces.h"

#include "convar.h"
#include "eiface.h"

#include <chrono>

namespace
{
	constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

	// Evidence prose is the only unbounded field in the payload, so it is the first
	// thing sacrificed when the console command length cap is in danger. The engine
	// allows 511 bytes per command and base64 costs 4/3, so the whole JSON has to
	// fit in roughly 350 bytes — the fixed fields take most of that, which is why
	// the starting budget here is small rather than generous.
	constexpr std::size_t kEvidenceBudget = 128;
	constexpr std::size_t kEvidenceMinimum = 16;

	std::uint64_t sequence {};
	std::uint64_t bootNonce {};
	std::uint64_t emitted {};
	std::uint64_t dropped {};
	std::uint64_t truncated {};
	std::string lastPayload;
	std::size_t lastEncodedSize {};

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

	std::string Hex32(std::uint64_t value)
	{
		char buffer[9] {};
		V_snprintf(buffer, sizeof(buffer), "%08llx", static_cast<unsigned long long>(value) & 0xFFFFFFFFull);
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

		// The whole relay has to survive as ONE console command argument, and the
		// engine caps a command at CCommand::MaxCommandLength() (511) — which base64
		// inflation turns into roughly 350 usable bytes of JSON. So this carries only
		// what the platform cannot reconstruct on its own.
		//
		// Deliberately NOT sent, and why it is safe to leave out:
		//   source, pluginVersion  the C# relay already substitutes "karasu-cs2ac" and
		//                          "karasucsac:unknown" when they are absent
		//   playerName, userid     the relay resolves the player from steamId
		//   deterministic          implied by tier A
		//   rule, corroboratingUnits, corroboratingType, serverTick
		//                          diagnostics; the platform re-derives its own verdict
		//                          from its durable ledger anyway, and its decision wins
		//
		// Everything that remains is load-bearing for the ban decision. If a field ever
		// has to be added here, take the budget out of the evidence prose, not out of
		// these.
		std::string payload;
		payload.reserve(evidence.size() + 320);
		payload += "{\"steamId\":\"";
		payload += std::to_string(report.steamId);
		payload += "\",\"detectionType\":\"cs2ac:";
		payload += JsonEscape(report.detectionName);
		payload += "\",\"severity\":\"";
		payload += karasu::SeverityForTier(report.policy.tier);
		payload += "\",\"detectorTier\":\"";
		payload += karasu::TierName(report.policy.tier);
		payload += "\",\"detectorFamily\":\"";
		payload += karasu::FamilyName(report.policy.family);
		payload += "\",\"confidence\":";
		payload += std::to_string(verdict.confidence);
		payload += ",\"recommendedAction\":\"";
		payload += karasu::ActionName(verdict.action);
		payload += "\",\"action\":\"";
		payload += karasu::ActionName(report.localAction);
		payload += "\",\"identitySource\":\"";
		payload += karasu::IdentitySourceName(report.identity);
		payload += "\",\"idempotencyKey\":\"";
		payload += eventId;
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

const std::string &karasu::relay::LastPayload()
{
	return lastPayload;
}

std::size_t karasu::relay::LastEncodedSize()
{
	return lastEncodedSize;
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
	// 16 hex of per-boot nonce + 8 hex of sequence = 24 chars. Comfortably unique
	// (the platform only needs it to dedupe a re-sent report) and 8 bytes shorter
	// than a full 128-bit key, which is 8 more bytes of evidence prose that fits.
	const std::string eventId = Hex64(bootNonce) + Hex32(++sequence);

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
		// Should be unreachable: the fields that remain after dropping the evidence
		// prose are a fixed set and fit with room to spare. If this ever fires, a
		// field was added to the payload without taking the budget from somewhere,
		// so print the numbers rather than just the fact.
		++dropped;
		Msg("[CS2AC] Karasu relay DROPPED a report for %s: %zu bytes of payload exceeds the %zu that fit in one console "
			"command (engine cap %zu, prefix %zu). This is a bug — the relay payload has outgrown its budget.\n",
			report.playerName ? report.playerName : "<unknown>", payload.size(), maximumPayload, maximumLine, prefix);
		return false;
	}

	if (didTruncate)
	{
		++truncated;
	}

	const std::string encoded = Base64Url(payload);
	lastPayload = payload;
	lastEncodedSize = encoded.size();

	std::string line = command;
	line += ' ';
	line += encoded;
	line += '\n';
	interfaces::pEngine->ServerCommand(line.c_str());
	++emitted;
	return true;
}
