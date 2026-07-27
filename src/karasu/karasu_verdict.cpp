#include "karasu_verdict.h"

namespace
{
	// Each repeat fire of the same detector in one session is worth this much extra
	// confidence. A detector that has already required 3-15 internal incidents to
	// fire once, firing again, is meaningfully stronger evidence than a single fire.
	constexpr int kRepeatBonus = 5;
	// Never reach 100 from repeats alone - 100 should mean "proved", and a repeated
	// statistical detector has not proved anything.
	constexpr int kRepeatCeiling = 99;
} // namespace

void karasu::PlayerVerdict::Reset()
{
	entries = {};
}

bool karasu::PlayerVerdict::IsLive(const Entry &entry, Clock::time_point now, int windowSeconds) const
{
	if (entry.fires <= 0)
	{
		return false;
	}
	if (windowSeconds <= 0)
	{
		return true;
	}
	return now - entry.last <= std::chrono::seconds(windowSeconds);
}

int karasu::PlayerVerdict::ConfidenceFor(DetectionType detection) const
{
	const auto slot = static_cast<std::size_t>(detection);
	return slot < kSlots ? entries[slot].confidence : 0;
}

int karasu::PlayerVerdict::FiresFor(DetectionType detection) const
{
	const auto slot = static_cast<std::size_t>(detection);
	return slot < kSlots ? entries[slot].fires : 0;
}

karasu::Verdict karasu::PlayerVerdict::Feed(DetectionType detection, Clock::time_point now, int windowSeconds, int minConfidence,
											int soloBanConfidence)
{
	Verdict verdict;

	const auto slot = static_cast<std::size_t>(detection);
	if (slot >= kSlots)
	{
		return verdict;
	}

	const DetectorPolicy policy = PolicyFor(detection);

	// A detection that has gone stale starts its count over, so a player cannot
	// accumulate one trip per hour across a long session into a ban.
	Entry &entry = entries[slot];
	if (!IsLive(entry, now, windowSeconds))
	{
		entry.fires = 0;
	}

	++entry.fires;
	entry.last = now;
	entry.confidence = policy.baseConfidence + kRepeatBonus * (entry.fires - 1);
	if (entry.confidence > kRepeatCeiling)
	{
		entry.confidence = kRepeatCeiling;
	}

	verdict.confidence = entry.confidence;

	// R1 - the detector is confident enough to stand on its own. Tier does not gate
	// this; confidence does, and the threshold is an operator setting. Every
	// detector can reach a ban, the strong ones just get there in one step.
	if (entry.confidence >= soloBanConfidence)
	{
		verdict.action = Action::Ban;
		verdict.rule = "solo_confidence";
		verdict.corroboratingUnits = 1;
		return verdict;
	}

	// Below the solo bar a detection still counts, it just needs a second signal.
	if (entry.confidence < minConfidence)
	{
		verdict.rule = "below_threshold";
		return verdict;
	}

	// R2 - a second, still-live detector from a different family. Requiring a
	// different family stops two detectors that measure the same underlying thing
	// (bhop and autostrafe are both movement) from corroborating each other.
	int units = 1;
	for (std::size_t other = 0; other < kSlots; ++other)
	{
		if (other == slot)
		{
			continue;
		}
		const Entry &candidate = entries[other];
		if (!IsLive(candidate, now, windowSeconds) || candidate.confidence < minConfidence)
		{
			continue;
		}
		const DetectorPolicy otherPolicy = PolicyFor(static_cast<DetectionType>(other));
		if (otherPolicy.family == policy.family)
		{
			continue;
		}
		++units;
		if (!verdict.hasCorroboratingType)
		{
			verdict.corroboratingType = static_cast<DetectionType>(other);
			verdict.hasCorroboratingType = true;
		}
	}

	verdict.corroboratingUnits = units;
	if (units >= 2)
	{
		verdict.action = Action::Ban;
		verdict.rule = "corroborated";
		return verdict;
	}

	// R3 - the same detector tripping more than once, well above the floor.
	if (entry.fires >= 2 && entry.confidence >= kRepeatConfidenceFloor)
	{
		verdict.action = Action::Ban;
		verdict.rule = "repeat";
		return verdict;
	}

	verdict.rule = "awaiting_corroboration";
	return verdict;
}
