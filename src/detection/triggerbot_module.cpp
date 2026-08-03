#include "detection/detection_system.h"

#include "bspflags.h"
#include "gametrace.h"
#include "igameevents.h"
#include "movement_analysis/player_context.h"
#include "movement/movement.h"
#include "sdk/entity/ccsplayerpawn.h"
#include "sdk/navphysicsinterface.h"
#include "sdk/usercmd.h"

#include <algorithm>
#include <array>
#include <cmath>

CConVar<bool> cs2ac_triggerbot_debug("cs2ac_triggerbot_debug", FCVAR_NONE, "Show Triggerbot contacts, reactions, and safety rejections", false);

#define TRIGGERBOT_DEBUG(...) \
	do \
	{ \
		if (cs2ac_triggerbot_debug.GetBool()) \
			Msg("[CS2AC Triggerbot] " __VA_ARGS__); \
	} while (0)

namespace
{
	constexpr size_t historyLimit = 30;
	constexpr size_t pendingLimit = 8;
	constexpr int freshContactTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE);
	constexpr int detectionThreshold = 10;
	constexpr float heldAngleMaximumDegrees = 0.5f;
	constexpr float traceLength = 8192.0f;
	constexpr float smokeHalfExtent = 320.0f;
	constexpr auto evidenceWindow = std::chrono::minutes(5);
	constexpr auto smokeLifetime = std::chrono::milliseconds(22500);

	struct ReactionScore
	{
		int score {};
		int oneTickCount {};
		int twoTickCount {};
		int normalCount {};
	};

	template<size_t N>
	constexpr ReactionScore ScoreReactions(const std::array<int, N> &ticks, size_t count)
	{
		ReactionScore result;
		for (size_t index = 0; index < count && index < N; ++index)
		{
			const int tick = ticks[index];
			if (tick >= 0 && tick <= 1)
			{
				++result.oneTickCount;
				result.score += 2;
			}
			else if (tick == 2)
			{
				++result.twoTickCount;
				++result.score;
			}
			else if (tick >= 3)
			{
				++result.normalCount;
				result.score = (std::max)(0, result.score - 2);
			}
		}
		return result;
	}

	static_assert(ScoreReactions(std::array<int, 5> {0, 1, 0, 1, 0}, 5).score == detectionThreshold);
	static_assert(ScoreReactions(std::array<int, 10> {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, 10).score == detectionThreshold);
	static_assert(ScoreReactions(std::array<int, 4> {1, 2, 3, 4}, 4).score == 0);
	static_assert(ScoreReactions(std::array<int, 4> {10, 20, 30, 40}, 4).score == 0);
	static_assert(ScoreReactions(std::array<int, 3> {3, 3, 1}, 3).score == 2);

	bool SegmentTouchesSmokeBounds(const Vector &start, const Vector &end, const Vector &center)
	{
		const float starts[] = {start.x, start.y, start.z};
		const float ends[] = {end.x, end.y, end.z};
		const float centers[] = {center.x, center.y, center.z};
		float minimumFraction = 0.0f;
		float maximumFraction = 1.0f;
		for (size_t axis = 0; axis < 3; ++axis)
		{
			const float delta = ends[axis] - starts[axis];
			const float minimum = centers[axis] - smokeHalfExtent;
			const float maximum = centers[axis] + smokeHalfExtent;
			if (std::fabs(delta) < EPSILON)
			{
				if (starts[axis] < minimum || starts[axis] > maximum)
				{
					return false;
				}
				continue;
			}
			float first = (minimum - starts[axis]) / delta;
			float second = (maximum - starts[axis]) / delta;
			if (first > second)
			{
				std::swap(first, second);
			}
			minimumFraction = (std::max)(minimumFraction, first);
			maximumFraction = (std::min)(maximumFraction, second);
			if (minimumFraction > maximumFraction)
			{
				return false;
			}
		}
		return true;
	}

	const char *ContactModeName(detection::TriggerContactMode mode)
	{
		return mode == detection::TriggerContactMode::AimDriven ? "aim moved onto target" : "held angle / target entered aim";
	}

	const char *HitGroupName(int group)
	{
		switch (group)
		{
			case HITGROUP_HEAD:
				return "head";
			case HITGROUP_CHEST:
				return "chest";
			case HITGROUP_STOMACH:
				return "stomach";
			case HITGROUP_LEFTARM:
			case HITGROUP_RIGHTARM:
				return "arm";
			case HITGROUP_LEFTLEG:
			case HITGROUP_RIGHTLEG:
				return "leg";
			case HITGROUP_NECK:
				return "neck";
			default:
				return "body";
		}
	}

	struct TraceContact
	{
		Vector point;
		int targetIndex {-1};
	};

	TraceContact TraceCrosshair(MovementPlayer *player, const Vector &eye, const QAngle &angles)
	{
		TraceContact contact;
		auto *pawn = player ? player->GetPlayerPawn() : nullptr;
		if (!pawn || !detection::IsFinite(eye) || !detection::IsFinite(angles))
		{
			return contact;
		}

		const Vector end = eye + detection::AimForward(angles) * traceLength;
		CTraceFilter filter(static_cast<CEntityInstance *>(pawn), pawn->m_hOwnerEntity.Get(),
							pawn->m_Collision().m_collisionAttribute().m_nHierarchyId(), MASK_SHOT, COLLISION_GROUP_DEFAULT, true);
		CGameTrace trace;
		INavPhysicsInterface::TraceLine(eye, end, &filter, &trace);
		if (!trace.DidHit() || !trace.m_pEnt || !trace.m_pHitbox || !g_pCS2ACPlayerManager)
		{
			return contact;
		}

		for (u32 index = 1; index <= MAXPLAYERS; ++index)
		{
			auto *target = g_pCS2ACPlayerManager->ToPlayer(index);
			auto *targetPawn = target ? target->GetPlayerPawn() : nullptr;
			if (targetPawn != trace.m_pEnt)
			{
				continue;
			}
			if (!targetPawn->IsAlive() || !detection::AreOpponents(pawn->GetTeam(), targetPawn->GetTeam()))
			{
				return contact;
			}
			contact.targetIndex = static_cast<int>(index);
			contact.point = trace.m_vEndPos;
			return contact;
		}
		return contact;
	}
} // namespace

namespace detection
{
	void TriggerbotModule::Load(AnnounceCallback announceCallback, NetworkSafetyMonitor *networkSafetyMonitor)
	{
		announce = announceCallback;
		networkSafety = networkSafetyMonitor;
		Reset();
	}

	void TriggerbotModule::Unload()
	{
		Reset();
		announce = nullptr;
		networkSafety = nullptr;
	}

	void TriggerbotModule::Reset()
	{
		playerData = {};
		for (auto &data : playerData)
		{
			data.lastContactTick.fill(-1);
		}
		smokes.clear();
		// A reload cannot inventory smoke that already exists, so remain fail-safe for one full smoke lifetime.
		smokeStateUnknownUntil = Clock::now() + smokeLifetime;
	}

	void TriggerbotModule::OnGameEvent(IGameEvent *event)
	{
		if (!event || !CS2AC_STREQ(event->GetName(), "smokegrenade_detonate"))
		{
			return;
		}
		const Vector center(event->GetFloat("x", 0.0f), event->GetFloat("y", 0.0f), event->GetFloat("z", 0.0f));
		const auto now = Clock::now();
		if (!IsFinite(center))
		{
			smokeStateUnknownUntil = now + smokeLifetime;
			return;
		}
		smokes.push_back({center, now + smokeLifetime});
		TRIGGERBOT_DEBUG("smoke safety volume added at %.1f %.1f %.1f.\n", center.x, center.y, center.z);
	}

	void TriggerbotModule::OnSetupMove(MovementPlayer *player, PlayerCommand *command, int currentTick)
	{
		if (!IsEligibleHuman(player) || !command || !command->has_base() || !command->base().has_viewangles())
		{
			return;
		}
		auto *pawn = player->GetPlayerPawn();
		if (!pawn || !pawn->IsAlive())
		{
			return;
		}

		auto &data = playerData[player->index];
		if (command->cmdNum <= data.lastCommandNumber)
		{
			return;
		}
		if (data.lastCommandNumber >= 0 && command->cmdNum != data.lastCommandNumber + 1)
		{
			data.activeTarget = -1;
			data.contactStartCommand = -1;
		}
		data.lastCommandNumber = command->cmdNum;

		const int attackIndex = command->attack1_start_history_index();
		const bool freshAttack =
			attackIndex >= 0 && attackIndex < command->input_history_size() && command->input_history(attackIndex).has_view_angles();
		const auto &baseView = command->base().viewangles();
		QAngle angles(baseView.x(), baseView.y(), baseView.z());
		if (freshAttack)
		{
			const auto &attackView = command->input_history(attackIndex).view_angles();
			angles = {attackView.x(), attackView.y(), attackView.z()};
		}
		if (!IsFinite(angles))
		{
			data.activeTarget = -1;
			data.contactStartCommand = -1;
			return;
		}

		Vector eye;
		player->GetEyeOrigin(&eye);
		const TraceContact contact = TraceCrosshair(player, eye, angles);
		if (contact.targetIndex != data.activeTarget)
		{
			data.activeTarget = contact.targetIndex;
			data.contactStartCommand = -1;
			if (contact.targetIndex >= 1)
			{
				const int previousContact = data.lastContactTick[static_cast<size_t>(contact.targetIndex)];
				if (previousContact < 0 || static_cast<std::int64_t>(currentTick) - previousContact >= freshContactTicks)
				{
					data.contactStartCommand = command->cmdNum;
					const float aimMovement = data.lastAnglesValid ? AngularDistance(data.lastAngles, angles) : 0.0f;
					data.activeMode = std::isfinite(aimMovement) && aimMovement > heldAngleMaximumDegrees ? TriggerContactMode::AimDriven
																										  : TriggerContactMode::TargetDriven;
					if (previousContact < 0)
					{
						TRIGGERBOT_DEBUG("%s acquired target #%d for the first time (%s).\n", player->GetName(), contact.targetIndex,
										 ContactModeName(data.activeMode));
					}
					else
					{
						TRIGGERBOT_DEBUG("%s acquired target #%d after %lld ticks without contact (%s).\n", player->GetName(), contact.targetIndex,
										 static_cast<long long>(static_cast<std::int64_t>(currentTick) - previousContact),
										 ContactModeName(data.activeMode));
					}
				}
			}
		}
		if (contact.targetIndex >= 1)
		{
			data.lastContactTick[static_cast<size_t>(contact.targetIndex)] = currentTick;
		}

		const auto now = Clock::now();
		while (!smokes.empty() && smokes.front().expires <= now)
		{
			smokes.pop_front();
		}
		bool smokeUncertain = now < smokeStateUnknownUntil;
		if (!smokeUncertain && contact.targetIndex >= 1)
		{
			smokeUncertain = std::any_of(smokes.begin(), smokes.end(),
										 [&](const TriggerbotSmoke &smoke) { return SegmentTouchesSmokeBounds(eye, contact.point, smoke.center); });
		}

		if (freshAttack && contact.targetIndex >= 1 && data.contactStartCommand >= 0)
		{
			const int reactionTicks = command->cmdNum - data.contactStartCommand;
			if (!smokeUncertain)
			{
				data.pending.push_back({command->cmdNum, contact.targetIndex, reactionTicks, data.activeMode});
				while (data.pending.size() > pendingLimit)
				{
					data.pending.pop_front();
				}
				TRIGGERBOT_DEBUG("%s pressed attack %d ticks after target #%d contact.\n", player->GetName(), reactionTicks, contact.targetIndex);
			}
			else
			{
				TRIGGERBOT_DEBUG("%s's fresh attack was ignored because smoke state could affect the aim line.\n", player->GetName());
			}
		}

		data.lastAngles = angles;
		data.lastAnglesValid = true;
	}

	void TriggerbotModule::OnPlayerHurt(IGameEvent *event, MovementPlayer *attacker, MovementPlayer *victim, const ShotRecord &shot)
	{
		if (!event || !IsEligibleHuman(attacker) || !victim)
		{
			return;
		}
		auto &data = playerData[attacker->index];
		const auto pending = std::find_if(data.pending.begin(), data.pending.end(),
										  [&](const TriggerbotPending &candidate) { return candidate.commandNumber == shot.commandNumber; });
		if (pending == data.pending.end())
		{
			return;
		}
		const TriggerbotPending candidate = *pending;
		data.pending.erase(pending);
		if (candidate.targetIndex != victim->index || shot.victimIndex != victim->index)
		{
			TRIGGERBOT_DEBUG("%s's damage was ignored because target #%d was expected, not #%d.\n", attacker->GetName(), candidate.targetIndex,
							 victim->index);
			return;
		}
		const int damage = event->GetInt("dmg_health", 0);
		if (damage <= 0)
		{
			TRIGGERBOT_DEBUG("%s's hit on target #%d was ignored because it dealt no health damage.\n", attacker->GetName(), victim->index);
			return;
		}

		const std::string_view weapon = NormalizeWeapon(shot.weapon);
		if (!IsBallisticWeapon(weapon) || weapon == "revolver" || weapon == "taser")
		{
			TRIGGERBOT_DEBUG("%s's %.*s shot was excluded.\n", attacker->GetName(), static_cast<int>(weapon.size()), weapon.data());
			return;
		}
		if (candidate.reactionTicks < 0)
		{
			return;
		}

		NetworkSafetyEvidence network;
		if (networkSafety)
		{
			network = networkSafety->Evaluate(attacker);
		}
		else
		{
			network.unavailableSamples = 1;
			network.vetoed = true;
		}
		if (network.vetoed)
		{
			TRIGGERBOT_DEBUG("%s's %d-tick damaging shot was ignored by network safety.\n", attacker->GetName(), candidate.reactionTicks);
			return;
		}

		const auto now = Clock::now();
		while (!data.history.empty() && now - data.history.front().time >= evidenceWindow)
		{
			data.history.pop_front();
		}
		data.history.push_back({now, candidate.reactionTicks});
		while (data.history.size() > historyLimit)
		{
			data.history.pop_front();
		}

		std::array<int, historyLimit> ticks {};
		for (size_t index = 0; index < data.history.size(); ++index)
		{
			ticks[index] = data.history[index].reactionTicks;
		}
		const ReactionScore score = ScoreReactions(ticks, data.history.size());
		TRIGGERBOT_DEBUG("%s recorded a damaging %d-tick shot: score %d/%d across %zu reactions; <=1t %d, 2t %d, normal %d.\n", attacker->GetName(),
						 candidate.reactionTicks, score.score, detectionThreshold, data.history.size(), score.oneTickCount, score.twoTickCount,
						 score.normalCount);
		if (score.score < detectionThreshold || !announce)
		{
			return;
		}

		announce("TRIGGERBOT", attacker,
				 localization::Format(
					 "evidence.triggerbot",
					 "The last {shots} damaging fresh-contact shots reached {score}/{threshold}: {one_tick} landed in 0-1 ticks (+2 each), "
					 "{two_tick} in 2 ticks (+1 each), and {normal} took 3+ ticks (-2 each). Latest: "
					 "{latest_ticks} ticks, {damage} damage, {context}, {hitgroup}, target #{target}.",
					 {{"shots", tfm::format("%zu", data.history.size())},
					  {"score", tfm::format("%d", score.score)},
					  {"threshold", tfm::format("%d", detectionThreshold)},
					  {"one_tick", tfm::format("%d", score.oneTickCount)},
					  {"two_tick", tfm::format("%d", score.twoTickCount)},
					  {"normal", tfm::format("%d", score.normalCount)},
					  {"latest_ticks", tfm::format("%d", candidate.reactionTicks)},
					  {"damage", tfm::format("%d", damage)},
					  {"context", ContactModeName(candidate.mode)},
					  {"hitgroup", HitGroupName(event->GetInt("hitgroup", HITGROUP_GENERIC))},
					  {"target", tfm::format("%d", candidate.targetIndex)}}));
		data.history.clear();
	}

	void TriggerbotModule::OnClientDisconnect(MovementPlayer *player)
	{
		if (player && player->index >= 1 && player->index <= MAXPLAYERS)
		{
			playerData[player->index] = {};
			playerData[player->index].lastContactTick.fill(-1);
		}
	}
} // namespace detection
