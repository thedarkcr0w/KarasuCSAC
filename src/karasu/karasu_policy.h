#pragma once

// Karasu platform integration: per-detector enforcement policy.
//
// CS2AC upstream splits detections into "ban" and "kick-only" by comparing the
// detection's display name against three string literals. That is fine when the
// punishment is a server-local admin command, but Karasu turns a detection into a
// platform-wide account ban, so the classification needs to be keyed on the enum
// (a renamed literal must not silently change ban behaviour) and it needs a middle
// tier for detectors that are strong evidence but not proof on their own.
//
// Tier A - the client sent something a conformant client cannot produce.
// Tier B - very unlikely for a human, but measured from behaviour.
// Tier C - real evidence with a worse false-positive profile.
//
// The tier feeds the severity reported to the platform and the family grouping used
// for corroboration. What actually decides a ban is the CONFIDENCE below, against
// cs2ac_karasu_solo_ban_confidence - so every detector is bannable and the operator
// sets how much evidence a ban takes. A detector below the solo threshold still bans
// once a second, independent detector corroborates it, or once it fires again.

#include "../settings.h"

#include <cstdint>

namespace karasu
{
	enum class Tier : std::uint8_t
	{
		A,
		B,
		C,
	};

	enum class Family : std::uint8_t
	{
		Aim,
		Movement,
		Protocol,
		Config,
	};

	enum class Action : std::uint8_t
	{
		Alert,
		Kick,
		Ban,
	};

	struct DetectorPolicy
	{
		Tier tier {Tier::C};
		Family family {Family::Config};
		// How much this detector's firing bar is worth, 0-100.
		//
		// This is a property of the detector, not of the individual event: every
		// CS2AC module already accumulates 3-15 internal incidents before it fires
		// once, so "it fired" is the signal. A repeat fire in the same session
		// raises the effective confidence (see PlayerVerdict::Feed). The API then
		// re-derives its own confidence across sessions from the durable ledger and
		// its decision is the one that bans.
		int baseConfidence {0};
	};

	constexpr DetectorPolicy PolicyFor(DetectionType detection)
	{
		switch (detection)
		{
			// --- Tier A: structurally impossible from a conformant client -----------
			// VerifyCommand proves every button transition has a matching subtick
			// record. A real client cannot desync those two fields.
			case DetectionType::InvalidInput:
				return {Tier::A, Family::Protocol, 95};
			// >=90% of subtick moves arriving with when() == 0.0f exactly. Genuine
			// subtick timings are continuous floats; all-zero is a stripped client.
			case DetectionType::Desubticking:
				return {Tier::A, Family::Protocol, 93};
			// Repeated press/release pairs at an identical (button, when) tuple that
			// carry pitch/yaw deltas - angle changes smuggled through button aliases.
			case DetectionType::SubtickSpam:
				return {Tier::A, Family::Protocol, 93};

			// --- Tier B: strong, but corroboration required -------------------------
			// Sustained on-target coverage across five latency hypotheses. Strongest
			// aim signal in the plugin, but its tolerance derives from the client's
			// own cl_interp_ratio, so the bar is partly negotiable by the subject.
			case DetectionType::Aimlock:
				return {Tier::B, Family::Aim, 88};
			// Bullet impact disagrees with the visible firing angle. Near-deterministic
			// for AWP/SSG (2 degrees) but the SMG threshold is 22, so one detector
			// spans a wide confidence range.
			case DetectionType::SilentAim:
				return {Tier::B, Family::Aim, 86};
			// Spin/jitter/impossible pitch. Physically impossible, held at B only
			// because a physics glitch on spawn could in principle produce a sample.
			case DetectionType::AntiAim:
				return {Tier::B, Family::Protocol, 84};
			// Damaging angle snaps converging on an enemy. A genuine flick that
			// overshoots and corrects can match the same shape.
			case DetectionType::Aimbot:
				return {Tier::B, Family::Aim, 82};
			// Consecutive perfect counter-strafes. The threshold adapts to the
			// client's reported framerate, and an honest low-FPS player gets the
			// lowest bar - which is the false-positive direction.
			case DetectionType::Nulls:
				return {Tier::B, Family::Movement, 80};
			// Exact-value client setting checks. Effectively deterministic, but a
			// Valve default change would implicate the entire player base at once,
			// which is historically the most common cause of mass-ban incidents.
			case DetectionType::InvalidCvar:
				return {Tier::B, Family::Config, 80};
			// Frame-perfect hops / jump-input frequency a physical wheel cannot
			// sustain. Scripts, which Karasu treats as a lesser offence than aim.
			case DetectionType::Bhop:
				return {Tier::B, Family::Movement, 78};
			case DetectionType::Hyperscroll:
				return {Tier::B, Family::Movement, 78};
			// Two disjoint detectors share this type and one of them emits no
			// numeric evidence, so it sits at the bottom of Tier B.
			case DetectionType::Autostrafe:
				return {Tier::B, Family::Movement, 76};

			// --- Tier C: weaker evidence, lowest confidence -------------------------
			// Fires on ANY ONE of 118 hardcoded client event subscriptions with zero
			// accumulation and no latch - and that list includes player_jump,
			// door_open and gc_connected, so a legitimate overlay or a Valve client
			// change can trip it. Highest false-positive risk in the codebase, and
			// the reason 55 is the lowest confidence that bans on its own by default:
			// raising cs2ac_karasu_solo_ban_confidence above 55 is the single change
			// that stops this detector banning anyone by itself.
			case DetectionType::DllInjection:
				return {Tier::C, Family::Config, 55};
			// The incident counter has no time window: three fire-pairs spread across
			// a whole map still trip it. Promote only once it has an evidence window.
			case DetectionType::Doubletap:
				return {Tier::C, Family::Aim, 50};
			// A strong human on close-range SMG duels reaches this hit rate, and the
			// weapon whitelist includes p90 and mac10. "Review this player", not proof.
			case DetectionType::InhumanAccuracy:
				return {Tier::C, Family::Aim, 45};
			// Airborne and no-scope kills are legitimate, if rare, human plays.
			case DetectionType::IrregularBehavior:
				return {Tier::C, Family::Aim, 45};
			// Detects annoyance, not cheating. Belongs in conduct moderation.
			case DetectionType::NameChanger:
				return {Tier::C, Family::Config, 30};

			case DetectionType::Count:
				break;
		}
		return {Tier::C, Family::Config, 0};
	}

	constexpr const char *TierName(Tier tier)
	{
		switch (tier)
		{
			case Tier::A:
				return "A";
			case Tier::B:
				return "B";
			case Tier::C:
				break;
		}
		return "C";
	}

	constexpr const char *FamilyName(Family family)
	{
		switch (family)
		{
			case Family::Aim:
				return "aim";
			case Family::Movement:
				return "movement";
			case Family::Protocol:
				return "protocol";
			case Family::Config:
				break;
		}
		return "config";
	}

	constexpr const char *ActionName(Action action)
	{
		switch (action)
		{
			case Action::Ban:
				return "ban";
			case Action::Kick:
				return "kick";
			case Action::Alert:
				break;
		}
		return "alert";
	}

	// Severity string the Karasu API already accepts (info|low|medium|high|critical).
	constexpr const char *SeverityForTier(Tier tier)
	{
		switch (tier)
		{
			case Tier::A:
				return "critical";
			case Tier::B:
				return "high";
			case Tier::C:
				break;
		}
		return "medium";
	}

	// Canonical uppercase display name for a detector, indexed by DetectionType.
	// This is the single source of truth: the announcement text, the status listing
	// and the relay payload all read it, so a rename stays consistent everywhere and
	// can never drift from the tier table above (which is keyed on the enum).
	const char *DetectionDisplayName(DetectionType detection);

	// Case-insensitive reverse lookup of a display name. Returns false when the name
	// is not one of the known detectors.
	bool ResolveDetectionType(const char *name, DetectionType &detection);

	// Any detector whose confidence reaches this bans on a single detection, whatever
	// its tier. This is the main dial (cs2ac_karasu_solo_ban_confidence).
	//
	// At the default of 55 that is every detector except DOUBLETAP (50),
	// INHUMAN ACCURACY (45), IRREGULAR BEHAVIOR (45) and NAMECHANGER (30), which
	// still need a second signal - either a different detector from a different
	// family, or the same detector firing again.
	//
	// Lower it to 30 and literally every detector bans on its own; raise it to 76
	// and only the aim/movement/protocol detectors do; raise it to 90 and only the
	// three structurally-impossible ones do. Nothing here is compiled in.
	inline constexpr int kDefaultSoloBanConfidence = 55;
	// A detection has to clear this before it may act as a corroboration unit.
	inline constexpr int kDefaultTierBConfidence = 72;
	// The same detector firing repeatedly may stand in for a second family, but only
	// well above the corroboration floor.
	inline constexpr int kRepeatConfidenceFloor = 85;
} // namespace karasu
