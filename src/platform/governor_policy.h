//
// governor_policy.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.
//
// BM-DOS - the clock governor's *decision*, with no Circle in it.
//
// Why this file exists, and why it is a header with code.
//
// Until .65f, `check-governor` tested the level *table* (which MHz go in which
// slot) and nothing tested the *policy* (when to move).  Three behaviours the
// policy is responsible for were visible on the board and nowhere else:
//
//   * the clock starts at 600 MHz, jumps to 1500 almost at once and then to
//     1800, on the DOS text screen at boot;
//   * the panel keeps printing `busy overXXXX` and never once `comf`;
//   * a DOS prompt that is doing nothing still walks the clock to the top.
//
// The first two are policy, and the policy was only reachable from the board.
// So the three numbers that decide it live here: thresholds, and one function
// that says what the state of the world is.  bare_sysinfo.cpp keeps the state
// machine, the mailbox and the counters; this keeps the judgement, which is the
// part that was wrong.
//
// The same discipline as bare_coreopts.h: no Circle, no firmware, no globals -
// just enough to be compiled and executed by a host test with numbers that came
// off a screen.
//
#ifndef BM_DOS_GOVERNOR_POLICY_H
#define BM_DOS_GOVERNOR_POLICY_H
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.

namespace governor
{

// A frame that used more than 90 % of its budget has no margin: a faster clock
// would help.  Under 75 % there is room, and the clock may come down.
constexpr unsigned OverrunPercent = 90;
constexpr unsigned ComfortPercent = 75;

/// What one frame said about the machine.
/// What one frame said about the machine.
enum class TVerdict
{
	Slow,		///< under ComfortPercent: there is room
	Borderline,	///< between comfort and overrun: no evidence either way
	Overrun,	///< over the budget: a faster clock would help, if there is one
};

/// Judge one frame.  0 of budget means "no measurement", which is not a verdict:
/// that case must be handled by the caller before it gets here (see the note in
/// BareSysInfoGovernorTick - a governor with no measurement must not escalate).
constexpr TVerdict Judge (unsigned nUsedMicros, unsigned nBudgetMicros)
{
	if (nBudgetMicros == 0)
	{
		return TVerdict::Borderline;
	}

	// Integer arithmetic, in percent, in 64 bits: a slow frame can be several
	// times the budget and the product overflows 32 bits above 42 ms.
	const unsigned long long nPercent =
		static_cast<unsigned long long> (nUsedMicros) * 100 / nBudgetMicros;

	if (nPercent > OverrunPercent)
	{
		return TVerdict::Overrun;
	}

	if (nPercent < ComfortPercent)
	{
		return TVerdict::Slow;
	}

	return TVerdict::Borderline;
}

/// \brief TRUE when this frame proves the emulated machine is doing nothing.
///
/// The .65g addition, and it is a *measurement*, not a heuristic: a frame that
/// is identical to the one before it is an emulator that ran and produced
/// nothing new.  On a DOS prompt that is every frame - seventy a second of the
/// same text - which is why the governor's old idle test ("nothing presented for
/// a second") could never fire on a text screen and the board sat at its top
/// level over a prompt that was doing nothing at all.
///
/// One condition, on purpose: whether "identical for long enough" has been true
/// is the caller's clock, not this function's.  What belongs here is the
/// judgement, and the judgement is that a repeated frame is not work.
constexpr bool IsIdleFrame (bool bFrameChanged)
{
	return !bFrameChanged;
}

/// The word the panel prints, and it needs to know what the governor can still
/// do - not only what the last frame cost.
///
/// A panel reading `busy over3614` at 1800 MHz is the top of the board's range:
/// "busy" means "climbing", and there is nowhere to climb.  So the *level* is as much a part of the answer as the
/// frame, and the four words become five:
///
///     idle   nothing is being presented; no clock is justified
///     max    over budget, at the top level: no clock will help (the old "busy")
///     busy   over budget with a level above: the governor is climbing
///     comf   comfortable: there is room, and the governor may step down
///     -      between comfort and overrun: no evidence either way
///
/// `comf` is deliberately not used for the max case even though it is what a
/// person waiting for the board to cool down would like to read: frames of
/// 28 ms in a 16.7 ms budget are not comfortable, they are the emulator being
/// too slow here.  Saying "comf" would be the panel lying to make a number look
/// better, which is the one thing this port has spent three rounds undoing.
constexpr const char *Label (bool bIdle, bool bAtTop, TVerdict Verdict)
{
	if (bIdle)
	{
		return "idle";
	}

	switch (Verdict)
	{
	case TVerdict::Slow:
		return "comf";

	case TVerdict::Overrun:
		return bAtTop ? "max" : "busy";

	case TVerdict::Borderline:
	default:
		return bAtTop ? "max" : "-";
	}
}

} // namespace governor

#endif // BM_DOS_GOVERNOR_POLICY_H
