//
// bare_time.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - time and logging glue.
//
// The whole port measures time through BareMicros(), and until .57 that
// function was lying by a factor of 54.  Everything that decides *when* things
// happen was therefore 54 times too impatient or 54 times too patient:
//
//   * the frame loop's pacer compared a 54 MHz counter against a 16.6 ms
//     budget written in microseconds, concluded it was always late, and never
//     slept - so the only thing limiting the speed of the emulated machine was
//     how fast the interpreter happened to be  ("se nota todo acelerado ...
//     cuando el procesador va mas libre");
//   * the stall watchdog fired after 2 seconds of a counter that advances 54
//     times faster than it thinks;
//   * the clock governor was fed a frame duration 54 times too large, so it
//     saw a permanent overrun and pinned itself;
//   * bare_audio's "micros since the last successful write" was 54 times too
//     large, which is exactly the instrument one would look at to decide
//     whether the sound device is being starved.
//
// The cause is a subtle one and it is worth writing down, because the code that
// had it looked right.  Circle's CTimer has two counters:
//
//   GetClockTicks()   1 MHz, converted internally with CLOCKHZ / CNTFRQ
//   GetClockTicks64() the raw physical counter, *not* converted
//
// On a desktop CPU the two are the same thing.  On a Pi 4 the physical counter
// runs at CNTFRQ_EL0 = 54 MHz, so GetClockTicks64() returns 54 ticks per
// microsecond while its name and its 1 MHz sibling suggest otherwise.  Using it
// directly as a microsecond value - which is what this file did, and what
// Circle's own GetClockTicks() documentation invites - is wrong by CNTFRQ/1e6.
//
// So the raw counter stays (it is still the best time source here: no device
// access, monotonic, safe from any core), and it is converted to microseconds
// by a factor measured at start-up rather than assumed.  The factor is measured
// against GetClockTicks(), which is the same piece of hardware already divided
// to exactly 1 MHz by Circle - so the calibration does not trust a second
// assumption, it checks one against the other.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"

#include <circle/logger.h>
#include <circle/timer.h>

#include <cstddef>
#include <cstring>

static const char FromTime[] = "time";
static const char FromCore[] = "core";

namespace {

// Raw physical-counter ticks per microsecond.  0 = not measured yet.
//
// Written once (see BareTimeCalibrate) and read by every BareMicros() call.
// A plain u64 and not an atomic: the value only ever goes 0 -> measured, both
// sides compute the same number, and a reader that sees 0 falls back to the
// uncalibrated path below for one call.  A lock here would be a lock taken
// thousands of times per second for a variable that changes once.
u64 s_nTicksPerMicro = 0;

// How many microseconds one 32-bit rollover of GetClockTicks() covers.  Only
// used while measuring; the wrap is handled in Calibrate().
constexpr u64 CalibrationMicros = 20000;	// 20 ms

/// \brief Measure the physical counter against Circle's microsecond clock.
///
/// Why not read CNTFRQ_EL0 and divide by it: that would be a second assumption
/// stacked on the one that was already wrong.  This measures the *ratio between
/// the two counters Circle hands out*, which is exactly the quantity BareMicros
/// needs, and it stays correct whatever the firmware set CNTFRQ to.
///
/// 20 ms is chosen so that the measurement is good to about one part in 20000
/// even though each of the two reads is a couple of instructions apart: at
/// 54 MHz that is ~1.08 M counter ticks against 20000 microsecond ticks.
u64 MeasureTicksPerMicro (void)
{
	// Sample both counters as close together as possible, twice.
	const u64 nRaw1 = CTimer::GetClockTicks64 ();
	const unsigned nUs1 = CTimer::GetClockTicks ();

	CTimer::SimpleusDelay (static_cast<unsigned> (CalibrationMicros));

	const u64 nRaw2 = CTimer::GetClockTicks64 ();
	const unsigned nUs2 = CTimer::GetClockTicks ();

	// The microsecond counter is 32 bit and wraps every 71 minutes; the
	// subtraction is done in unsigned 32-bit arithmetic on purpose so that a
	// wrap in the middle of the measurement does not turn into a 4-billion
	// microsecond interval.
	const unsigned nElapsedUs = nUs2 - nUs1;
	if (nElapsedUs < CalibrationMicros / 2)
	{
		// Too short to divide by, or the clock moved backwards.  Not worth
		// a second attempt: the caller falls back and says so.
		return 0;
	}

	const u64 nElapsedRaw = nRaw2 - nRaw1;
	if (nElapsedRaw == 0)
	{
		return 0;
	}

	// Rounded to the nearest integer.  One microsecond of resolution is all
	// any caller here needs - the finest thing measured is a 14 ms frame.
	const u64 nRatio = (nElapsedRaw + nElapsedUs / 2) / nElapsedUs;

	return nRatio != 0 ? nRatio : 1;
}

} // namespace

void BareTimeCalibrate (void)
{
	s_nTicksPerMicro = MeasureTicksPerMicro ();

	if (s_nTicksPerMicro == 0)
	{
		// The measurement failed.  Treating the raw counter as microseconds
		// is the behaviour of every build before .57: wrong by CNTFRQ/1e6 on
		// a Pi 4, but at least self-consistent and no worse than it was.
		s_nTicksPerMicro = 1;
		CLogger::Get ()->Write (FromTime, LogWarning,
			"cannot measure the clock counter: using raw ticks as microseconds");
		return;
	}

	// This line is the answer to a question that has cost several rounds:
	// if it says 54, every timing constant in the port used to be 54 times
	// off, and the pacer never slept.  If it says 1, the counter really is a
	// microsecond clock and the speed problem is somewhere else.
	CLogger::Get ()->Write (FromTime, LogNotice,
		"clock counter: %llu ticks per microsecond (%llu MHz)%s",
		(unsigned long long) s_nTicksPerMicro,
		(unsigned long long) s_nTicksPerMicro,
		s_nTicksPerMicro == 1 ? "" : " - measured, not assumed");
}

u64 BareMicros (void)
{
	// CTimer::GetClockTicks64() reads the CPU's physical counter register:
	// no device access, no lock, monotonic, and valid from any core.  It is
	// the only time source safe to use once presentation moves to core 1.
	//
	// Divided, not returned raw: see the file header.  A 64-bit divide per
	// call is about thirty cycles, and this is called a couple of dozen times
	// per frame, so it is nothing next to an emulated frame.
	const u64 nRaw = CTimer::GetClockTicks64 ();

	const u64 nTicksPerMicro = s_nTicksPerMicro;
	if (nTicksPerMicro <= 1)
	{
		return nRaw;
	}

	return nRaw / nTicksPerMicro;
}

void BareLog (int nLevel, const char *pText)
{
	if (pText == nullptr)
	{
		return;
	}

	TLogSeverity severity;
	switch (nLevel)
	{
	case 1:	 severity = LogDebug;	break;
	case 2:	 severity = LogNotice;	break;
	case 3:	 severity = LogWarning;	break;
	default: severity = LogError;	break;
	}

	// DOSBox's own format strings can be long and contain '%': pass them as
	// an argument rather than as a format.
	CLogger::Get ()->Write (FromCore, severity, "%s", pText);

	// One class of error is not worth a line on the display: dosbox-core logs
	// an error per core option when the frontend does not answer
	// RETRO_ENVIRONMENT_GET_VARIABLE.  This port answers five of them from
	// .58 on (bare_coreopts.h) and still leaves the other ~35 to the core's
	// own defaults, on purpose - there is no option UI and the real settings
	// come from dosbox.conf, which the core parses itself.  So the remaining
	// identical "using the default" lines are suppressed here; they cost more
	// than they explain.
	if (strncmp (pText, "RETRO_ENVIRONMENT_GET_VARIABLE", 30) == 0)
	{
		return;
	}

	// Everything else, but not stacked without limit.
	//
	// A message written here is painted in the status column and remembered in
	// a per-line slot (bare_video.cpp), and the slots are repainted on top of
	// the picture once the emulator takes the screen.  So this function is the
	// only writer of "what happened" that can quietly fill the whole screen -
	// and then the thing it is meant to explain is off the bottom of it.  The
	// .61 boot had one harmless core warning per frame boundary from
	// update_gfx_mode() ("SET_SYSTEM_AV_INFO failed.", from a frontend that
	// does not implement the extension - see BareEnvironment), which is a
	// screenful of the same sentence and no diagnosis.
	//
	// So: identical text is not repeated.  THE ONE RULE THAT MATTERS - the
	// release latch.  Any *different* message, at any level, releases the
	// suppression, so a later repeat of the first one is shown again.  Without
	// that, an early warning would silence the same warning for ever, which is
	// the failure mode a "print once" filter always has and this port has
	// already been bitten by once.
	{
		constexpr size_t MaxKeys = 8;

		static char Key[MaxKeys][96];
		static unsigned nKeys = 0;
		static const char *pLast = nullptr;

		if (pLast != nullptr && strcmp (pLast, pText) == 0)
		{
			return;
		}

		bool bKnown = false;
		for (unsigned i = 0; i < nKeys; i++)
		{
			if (strcmp (Key[i], pText) == 0)
			{
				bKnown = true;
				break;
			}
		}

		if (bKnown)
		{
			// Seen before in this run and nothing new since: the screen
			// already says it.
			return;
		}

		if (nKeys < MaxKeys)
		{
			strncpy (Key[nKeys], pText, sizeof Key[0] - 1);
			Key[nKeys][sizeof Key[0] - 1] = '\0';
			nKeys++;
		}

		pLast = Key[nKeys - 1];
	}

	BareVideoStatusPrint (pText);
}
