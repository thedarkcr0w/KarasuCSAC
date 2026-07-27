#pragma once

// Karasu platform integration: detection relay.
//
// CS2AC does not talk to the Karasu API. It has no HTTP client for it, no
// credentials, and no match context - it does not know the match id, the server id
// or the per-match report token, and it must never hold the platform's shared
// secret in a plaintext server config file.
//
// Instead it writes one line to the server console:
//
//     karasu_anticheat_report <base64url-json>
//
// The Karasu CS2 plugin (CounterStrikeSharp) already registers that exact command.
// It decodes the payload, attaches the match context and report token, signs the
// request and POSTs it to the platform, which makes the actual ban decision.
//
// This keeps the whole credential and API surface out of an AGPL-licensed repo, and
// keeps the plugin's hot path free of sockets and TLS: emitting a relay is a string
// build plus a console command, which the engine queues.

#include "karasu_verdict.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace karasu
{
	// How the player's SteamID64 was obtained. The platform refuses to ban on
	// anything but an authenticated identity, because the plugin can otherwise fall
	// back to an unvalidated xuid and ban the wrong account.
	enum class IdentitySource : std::uint8_t
	{
		Authenticated,
		Controller,
		Unauthenticated,
	};

	const char *IdentitySourceName(IdentitySource source);

	struct RelayReport
	{
		std::uint64_t steamId {};
		int userId {-1};
		const char *detectionName {""};
		const char *playerName {""};
		DetectionType detection {DetectionType::Count};
		DetectorPolicy policy {};
		Verdict verdict {};
		// What the plugin actually did on this server, which may be less than what
		// it recommends: the platform still decides whether an account is banned.
		Action localAction {Action::Alert};
		IdentitySource identity {IdentitySource::Unauthenticated};
		std::string_view evidence;
		int serverTick {};
	};

	namespace relay
	{
		// Clears the sequence counter and re-seeds the per-boot nonce.
		void Reset();

		// Builds and emits one relay line. Returns false when the relay is disabled,
		// the engine is unavailable, or the payload could not be made to fit.
		bool Emit(const RelayReport &report);

		std::uint64_t EmittedCount();
		std::uint64_t DroppedCount();
		std::uint64_t TruncatedCount();

		// The JSON of the most recent report, before base64. Kept so an operator can
		// see exactly what was put on the wire without having to decode a console
		// line by hand - the payload is length-budgeted and silently trims its
		// evidence prose, so "what actually got sent" is a real question.
		const std::string &LastPayload();
		std::size_t LastEncodedSize();
	} // namespace relay
} // namespace karasu
