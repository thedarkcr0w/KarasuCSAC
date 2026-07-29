// Detects malformed, spammed, or deliberately flattened subtick input.
#include "movement_analysis/detection/movement_detection.h"
#include "sdk/usercmd.h"
#include "movement_analysis/events/movement_events.h"
#include "settings.h"

// Fixed thresholds for subtick command checks.
#define SUBTICK_INITIAL_IGNORE_TIME        10.0f
#define SUBTICK_INVALID_COMMAND_WINDOW     5.0f
#define SUBTICK_INVALID_COMMAND_THRESHOLD  8
#define SUBTICK_SUSPICIOUS_MOVES_WINDOW    0.5f
#define SUBTICK_SUSPICIOUS_MOVES_THRESHOLD 20
#define SUBTICK_SUBTICK_INPUTS_WINDOW      20.0f
#define SUBTICK_SUBTICK_INPUTS_THRESHOLD   30
#define SUBTICK_ZERO_WHEN_RATIO_THRESHOLD  0.9f

CConVar<bool> cs2ac_subtick_debug("cs2ac_subtick_debug", FCVAR_NONE, "Show Invalid Input, Subtick Spam, and Desubticking evidence", false);

// Every command should have all button presses/releases accounted for in subtick moves.
// Only cheats that modify buttons without updating subtick moves would fail this.
static_global bool VerifyCommand(const PlayerCommand &cmd)
{
	// Expect a press/release this tick
	u64 expectedButtons = cmd.base().buttons_pb().buttonstate2() | cmd.base().buttons_pb().buttonstate3();
	// We only care about certain movement buttons.
	expectedButtons &= (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_DUCK);
	for (i32 i = 0; i < cmd.base().subtick_moves_size(); i++)
	{
		if (cmd.base().subtick_moves(i).has_button())
		{
			expectedButtons &= ~cmd.base().subtick_moves(i).button();
		}
		else
		{
			// analog input?
			if (cmd.base().subtick_moves(i).has_analog_forward_delta() && cmd.base().subtick_moves(i).analog_forward_delta() != 0)
			{
				expectedButtons &= ~(IN_FORWARD | IN_BACK);
			}
			if (cmd.base().subtick_moves(i).has_analog_left_delta() && cmd.base().subtick_moves(i).analog_left_delta() != 0)
			{
				expectedButtons &= ~(IN_MOVELEFT | IN_MOVERIGHT);
			}
		}
	}
	return expectedButtons == 0;
}

// Yaw/pitch alias abuse detection: If there's too many subtick moves with the same button + timing and non-zero angles, flag it.
static_global bool HasExcessiveSubtickMovesWithAngles(const PlayerCommand &cmd)
{
	if (cmd.base().subtick_moves_size() < 2)
	{
		return false;
	}
	std::set<std::pair<u64, f32>> buttonTimes;
	// Get a list of all button presses with timings
	for (i32 i = 0; i < cmd.base().subtick_moves_size(); i++)
	{
		const CSubtickMoveStep &step = cmd.base().subtick_moves(i);
		if (!step.pressed() || !step.has_button())
		{
			continue;
		}
		// Ignore attack buttons and turn binds (since they wouldn't work anyway)
		if (step.button() == IN_ATTACK || step.button() == IN_ATTACK2 || step.button() == IN_USE || step.button() == IN_RELOAD
			|| step.button() == IN_TURNLEFT || step.button() == IN_TURNRIGHT)
		{
			continue;
		}
		if (step.has_pitch_delta() || step.has_yaw_delta())
		{
			buttonTimes.insert({step.button(), step.when()});
		}
	}
	// Go through the list again. If we find the same button + timing again on release, it's suspicious.
	i32 numSuspicious = 0;
	for (i32 i = 0; i < cmd.base().subtick_moves_size(); i++)
	{
		const CSubtickMoveStep &step = cmd.base().subtick_moves(i);
		if (step.pressed() || !step.has_button())
		{
			continue;
		}
		auto it = buttonTimes.find({step.button(), step.when()});
		if (it != buttonTimes.end())
		{
			numSuspicious++;
			if (numSuspicious >= 2)
			{
				if (cs2ac_subtick_debug.GetBool())
				{
					Msg("[CS2AC Subtick Spam] Suspicious command %d: %s\n", cmd.cmdNum, cmd.DebugString().c_str());
				}
				return true;
			}
		}
	}
	return false;
}

static_global bool PossibleDesubtickedCommand(const PlayerCommand &cmd)
{
	// Can't tell if there are no subtick moves.
	if (cmd.base().subtick_moves_size() == 0)
	{
		return false;
	}

	// If there are subtick moves but the timing is at exactly 0, it's suspicious... unless it's controller-issued commands.
	bool hasZeroWhen = true;
	bool hasNonAnalogMove = false;
	for (i32 i = 0; i < cmd.base().subtick_moves_size(); i++)
	{
		const CSubtickMoveStep &step = cmd.base().subtick_moves(i);
		if (step.has_analog_forward_delta() || step.has_analog_left_delta())
		{
			continue;
		}
		hasNonAnalogMove = true;
		hasZeroWhen &= (step.when() == 0.0f);
	}
	// If every move was analog, we have no non-analog evidence to call this suspicious.
	return hasNonAnalogMove && hasZeroWhen;
}

void MovementDetectionService::CheckSubtickAbuse(PlayerCommand *cmd)
{
	if (this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	// Verify all button presses/releases are accounted for in subtick moves
	if (settings::IsDetectionEnabled(DetectionType::InvalidInput) && !VerifyCommand(*cmd))
	{
		this->invalidCommandTimes.push_back(g_pCS2ACUtils->GetServerGlobals()->curtime);
		if (cs2ac_subtick_debug.GetBool())
		{
			Msg("[CS2AC Invalid Input] Suspicious command %d: %s\n", cmd->cmdNum, cmd->DebugString().c_str());
		}
	}
	// Check for excessive subtick moves with angles
	if (settings::IsDetectionEnabled(DetectionType::SubtickSpam) && HasExcessiveSubtickMovesWithAngles(*cmd))
	{
		this->suspiciousSubtickMoveTimes.push_back(g_pCS2ACUtils->GetServerGlobals()->curtime);
	}
	// Check for possible desubticked commands
	if (cmd->base().subtick_moves_size() > 0)
	{
		this->numCommandsWithSubtickInputs.push_back(g_pCS2ACUtils->GetServerGlobals()->curtime);
	}
	if (PossibleDesubtickedCommand(*cmd))
	{
		this->zeroWhenCommandTimes.push_back(g_pCS2ACUtils->GetServerGlobals()->curtime);
	}
}

void MovementDetectionService::CheckSuspiciousSubtickCommands()
{
	if (this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	if (this->player->movementEvents->GetTimeInServer() < SUBTICK_INITIAL_IGNORE_TIME)
	{
		// Don't check for the first 10 seconds to avoid false positives on connect... just in case.
		this->invalidCommandTimes.clear();
		this->suspiciousSubtickMoveTimes.clear();
		this->numCommandsWithSubtickInputs.clear();
		this->zeroWhenCommandTimes.clear();
		return;
	}
	f32 currentTime = g_pCS2ACUtils->GetServerGlobals()->curtime;

	// Clear out invalid commands within configured window
	while (!this->invalidCommandTimes.empty() && currentTime - this->invalidCommandTimes.front() > SUBTICK_INVALID_COMMAND_WINDOW)
	{
		this->invalidCommandTimes.pop_front();
	}
	if (this->invalidCommandTimes.size() >= SUBTICK_INVALID_COMMAND_THRESHOLD)
	{
		this->MarkInfraction(
			Infraction::Type::InvalidInput,
			localization::Format("evidence.invalid_input",
								 "{commands} commands had movement button changes without matching subtick records within {seconds} seconds.",
								 {{"commands", tfm::format("%zu", this->invalidCommandTimes.size())},
								  {"seconds", tfm::format("%.1f", SUBTICK_INVALID_COMMAND_WINDOW)}}));
		this->invalidCommandTimes.clear();
	}

	// Clear out old entries beyond configured window
	while (!this->suspiciousSubtickMoveTimes.empty() && currentTime - this->suspiciousSubtickMoveTimes.front() > SUBTICK_SUSPICIOUS_MOVES_WINDOW)
	{
		this->suspiciousSubtickMoveTimes.pop_front();
	}
	// Repeated same-time button aliases with angle changes become Subtick Spam evidence.
	if (this->suspiciousSubtickMoveTimes.size() >= SUBTICK_SUSPICIOUS_MOVES_THRESHOLD)
	{
		this->MarkInfraction(
			Infraction::Type::SubtickSpam,
			localization::Format("evidence.subtick_spam",
								 "{commands} commands contained repeated same-time button aliases with angle changes within {seconds} seconds.",
								 {{"commands", tfm::format("%zu", this->suspiciousSubtickMoveTimes.size())},
								  {"seconds", tfm::format("%.1f", SUBTICK_SUSPICIOUS_MOVES_WINDOW)}}));
		this->suspiciousSubtickMoveTimes.clear();
	}

	// Clear out old entries beyond configured window
	while (!this->numCommandsWithSubtickInputs.empty() && currentTime - this->numCommandsWithSubtickInputs.front() > SUBTICK_SUBTICK_INPUTS_WINDOW)
	{
		this->numCommandsWithSubtickInputs.pop_front();
	}
	while (!this->zeroWhenCommandTimes.empty() && currentTime - this->zeroWhenCommandTimes.front() > SUBTICK_SUBTICK_INPUTS_WINDOW)
	{
		this->zeroWhenCommandTimes.pop_front();
	}
	// A sustained zero timing ratio becomes Desubticking evidence.
	if (this->numCommandsWithSubtickInputs.size() >= SUBTICK_SUBTICK_INPUTS_THRESHOLD)
	{
		f32 ratio = (f32)this->zeroWhenCommandTimes.size() / (f32)this->numCommandsWithSubtickInputs.size();
		if (ratio >= SUBTICK_ZERO_WHEN_RATIO_THRESHOLD)
		{
			localization::Text details =
				localization::Format("evidence.desubticking", "{zero} of {commands} commands with subtick input had zero timing ({ratio}%).",
									 {{"zero", tfm::format("%zu", this->zeroWhenCommandTimes.size())},
									  {"commands", tfm::format("%zu", this->numCommandsWithSubtickInputs.size())},
									  {"ratio", tfm::format("%.1f", ratio * 100.0f)}});
			if (cs2ac_subtick_debug.GetBool())
			{
				Msg("[CS2AC Desubticking] Player slot %d: %s\n", this->player->GetPlayerSlot().Get(), details.english.c_str());
			}
			this->MarkInfraction(Infraction::Type::Desubtick, details);
			this->zeroWhenCommandTimes.clear();
			this->numCommandsWithSubtickInputs.clear();
		}
	}
}
