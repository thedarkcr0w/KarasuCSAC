#pragma once

// Karasu platform integration: per-player corroboration ledger.
//
// A detection on its own is not a ban. This tracks what each connected player has
// tripped so far and decides whether the plugin should recommend a ban, a kick, or
// nothing at all.
//
// Deliberately advisory. The plugin's ledger dies with the session - disconnect,
// map change and config reload all clear detector evidence upstream - so a
// reconnect would otherwise be a free reset. The durable, cross-session ledger
// lives on the Karasu API, which re-evaluates the same rules over its own history
// and is the only thing that writes a ban. When the two disagree, the API wins.

#include "karasu_policy.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace karasu
{
	using Clock = std::chrono::steady_clock;

	struct Verdict
	{
		Action action {Action::Alert};
		// Effective confidence of the detection that produced this verdict.
		int confidence {0};
		// How many distinct Tier B detector types currently corroborate each other.
		int corroboratingUnits {0};
		// The other detector type that corroborated this one, when there is one.
		DetectionType corroboratingType {DetectionType::Count};
		bool hasCorroboratingType {false};
		// Which rule fired, for the console log and the relay payload.
		const char *rule {"none"};
	};

	// One player's detection history for the current session.
	//
	// Fixed storage indexed by DetectionType - no allocation on the detection path.
	class PlayerVerdict
	{
	public:
		void Reset();

		// Records one detection and returns the resulting recommendation.
		//
		// windowSeconds bounds how long a detection stays eligible to corroborate
		// another one. minConfidence is the floor a Tier B detection must clear to
		// count as a corroboration unit.
		Verdict Feed(DetectionType detection, Clock::time_point now, int windowSeconds, int minConfidence);

		// Effective confidence currently recorded for a detector, 0 when unseen.
		int ConfidenceFor(DetectionType detection) const;
		int FiresFor(DetectionType detection) const;

	private:
		struct Entry
		{
			Clock::time_point last {};
			int fires {};
			int confidence {};
		};

		static constexpr std::size_t kSlots = static_cast<std::size_t>(DetectionType::Count);

		bool IsLive(const Entry &entry, Clock::time_point now, int windowSeconds) const;

		std::array<Entry, kSlots> entries {};
	};
} // namespace karasu
