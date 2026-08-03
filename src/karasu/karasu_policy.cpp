#include "karasu_policy.h"

#include "../common.h"

namespace
{
	// Order must match DetectionType exactly - the static_assert below only catches a
	// missing or extra entry, not a reordered one, so keep these together.
	constexpr const char *detectionNames[] = {
		"AIMBOT",    "AIMLOCK",     "ANTIAIM",          "AUTOSTRAFE",   "BHOP",          "DLL INJECTION",      "DESUBTICKING",
		"DOUBLETAP", "HYPERSCROLL", "INHUMAN ACCURACY", "INVALID CVAR", "INVALID INPUT", "IRREGULAR BEHAVIOR", "NAMECHANGER",
		"NULLS",     "SILENTAIM",   "SUBTICK SPAM",     "TRIGGERBOT",
	};
	static_assert(CS2AC_ARRAYSIZE(detectionNames) == static_cast<std::size_t>(DetectionType::Count));
} // namespace

const char *karasu::DetectionDisplayName(DetectionType detection)
{
	const auto index = static_cast<std::size_t>(detection);
	return index < CS2AC_ARRAYSIZE(detectionNames) ? detectionNames[index] : "UNKNOWN";
}

bool karasu::ResolveDetectionType(const char *name, DetectionType &detection)
{
	if (!name || !*name)
	{
		return false;
	}
	for (std::size_t index = 0; index < CS2AC_ARRAYSIZE(detectionNames); ++index)
	{
		if (CS2AC_STREQI(name, detectionNames[index]))
		{
			detection = static_cast<DetectionType>(index);
			return true;
		}
	}
	return false;
}
