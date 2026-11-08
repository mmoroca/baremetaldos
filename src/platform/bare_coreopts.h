// bare_coreopts.h - the core options this port answers itself.
//
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
// ---------------------------------------------------------------------------
// Why this file exists
// ---------------------------------------------------------------------------
//
// BM-DOS is a libretro *frontend* for exactly one core (dosbox-core).  We have
// no option screen and no per-user option file, so the honest default for
// RETRO_ENVIRONMENT_GET_VARIABLE is "no", and that is what bare_libretro.cpp
// used to answer - after which dosbox-core falls back to the default baked into
// its own option table.
//
// That fallback is fine for the ~40 options that agree with what we want, and
// it is wrong for the CPU cycles ones, which are the ones that decide how fast
// the emulated machine runs.  In the core (libretro_core_options.cpp, revision
// pinned in DOSBOX_REV, table "CORE_OPT_CPU_CYCLES_MODE" and friends) the
// defaults are:
//
//     cpu_cycles_mode            fixed
//     cpu_cycles_multiplier      10000
//     cpu_cycles                   1          -> 1 x 10000 = 10000
//     cpu_cycles_multiplier_fine 1000
//     cpu_cycles_fine              0          -> 0 x 1000  =     0
//                                             ----------------------
//                                             "fixed 10000"
//
// and 10000 cycles is DOSBox's 386DX class.  The target is a **486DX2 at
// 66 MHz**, which is 23880 in the scale the core's own documentation uses
// (src/cpu/cpu.cpp turns "fixed N" straight into CPU_CycleMax, and that number
// is the classic "cycles" figure).
//
// ---------------------------------------------------------------------------
// The part that has to be read carefully
// ---------------------------------------------------------------------------
//
// The obvious fix is the one that was in the tree before this file existed:
// ship `cycles=fixed 23880` in sd/dosbox.conf and let the core read it.  That
// looks right and is half wrong, because dosbox-core does not simply *read* the
// .conf - it rewrites the variable from its option table.  In the pinned
// revision, libretro.cpp:
//
//     start_dosbox()          parse .conf  ->  control->Init()
//                             dosbox_initialiazed = true
//                             check_variables()            <- second pass
//
// and in that second pass `check_cpu_cycle_variables()` calls
// `update_dosbox_variable("cpu", "cycles", make_cpu_cycles_string())`.  The
// early-out is `if (dosbox_initialiazed && compare_dosbox_variable(...))` - it
// only skips when the *computed* string already equals the .conf value, and the
// computed string comes from the core options.  So with a frontend that answers
// GET_VARIABLE with "no":
//
//     .conf says  fixed 23880
//     computed is fixed 10000
//     they differ  -> update_dosbox_variable() runs
//     HandleInputline("cycles=fixed 10000") -> sync_core_opts_to_conf()
//     -> "lock changed options": core_cycles is locked to 10000
//
// The last step is what makes it invisible.  The lock replaces the option's
// value *list* with the single value that was just applied, so the core options
// screen - when there is one - shows 10000 as the only choice, and the .conf
// line reads as if it had been honoured.
//
// So the number the emulated machine runs at is not the .conf's.  It is
// whatever the option table says, and the option table's default is 386-class.
// Fixing it in the .conf alone therefore cannot work, and "it is in the .conf,
// so it is right" is precisely the false green this port keeps paying for.
//
// ---------------------------------------------------------------------------
// What we do instead
// ---------------------------------------------------------------------------
//
// Answer GET_VARIABLE for the five options that make up that expression, so the
// computed string is the one we want and the .conf and the core agree:
//
//     cpu_cycles_mode             fixed
//     cpu_cycles                  2
//     cpu_cycles_multiplier       10000     ->   2 x 10000 = 20000
//     cpu_cycles_fine             4
//     cpu_cycles_multiplier_fine  1000      ->   4 x 1000  =  4000
//                                               ------------------
//                                               24000 cycles
//
// Why 24000 and not exactly 23880: the core's arithmetic is
// `cpu_cycles * cpu_cycles_multiplier + cpu_cycles_fine * fine_multiplier`,
// where both values are limited to 0..9, the coarse multipliers are
// {100, 1000, 10000, 100000} and the fine ones {1, 10, 100, 1000, 10000}.  That
// is a grid of, at most, 900000 in steps of 100 near this size, and 23880 is
// not on it.  The nearest expressible values are 23700 (2x10000 + 37x100, and
// 37 is not a legal value either), no: the reachable neighbours are 23000
// (2x10000 + 3x1000) and 24000 (2x10000 + 4x1000).  24000 is 0.5 % above
// 23880 - under one percent of the emulated clock, far below the jitter of an
// interpreter, and the same machine class.  The target is a 486DX2 at
// 66 MHz, not for a digit; the header says which figure it is and why.
//
// The alternative - answering "fixed 23880" from *our* side by rewriting the
// variable after the core's second pass - was rejected: it would fight the core
// for ownership of a variable during startup, and a race there boots at the
// wrong speed silently.  Two sources of truth is how the .conf got into this
// mess in the first place.
//
// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------
//
// Only the options in the table below are answered.  Everything else still gets
// "no" and the core's own default, which is what the port wants for the other
// forty.  This is not an option system; it is one correction with a fence
// around it, and the fence has a test (check-coreopts).
//
// There is one more entry, "vga_hz", and it is the exception that proves the
// rule: what it answers is "0", which is the core's own default.  It owns the
// key and changes nothing - see the note on it further down, which also records
// why every attempt to make it a setting (.65 to .65f) was removed.
//
// No Circle and no libretro here: this compiles on the host, which is the whole
// point of it being a separate header.
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.

#ifndef BARE_COREOPTS_H
#define BARE_COREOPTS_H

#include <cstring>

namespace bare_coreopts
{

// ---------------------------------------------------------------------------
// The number
// ---------------------------------------------------------------------------

/// Emulated cycles, in DOSBox's "cycles" units (one cycle is one emulated
/// instruction per millisecond of emulated time). 486DX2-66 class.
///
/// Reference points from the core's own documentation, same scale:
///   286 @ 12 MHz     1510        386DX @ 33 MHz     6075
///   486DX @ 33 MHz  12010        486DX2 @ 66 MHz   23880
///   486DX4 @ 100 MHz 33445       Pentium @ 75 MHz  40072
///
/// This is the 486DX2 @ 66 MHz entry.  23880 cannot be spelled in
/// the core's coarse/fine option pairs (see the file comment), so what is
/// answered is 24000, which is that machine to within 0.5 %.
constexpr int CYCLES_DEFAULT = 24000;

/// How CYCLES_DEFAULT is spelled to the core: `cpu_cycles` x its coarse
/// multiplier, plus `cpu_cycles_fine` x its fine multiplier.  Kept as named
/// constants rather than literals so the test can check the arithmetic instead
/// of trusting the comment.
constexpr int COARSE_VALUE = 2;		///< cpu_cycles           (0..9 in the core)
constexpr int COARSE_MULTIPLIER = 10000;	///< cpu_cycles_multiplier
constexpr int FINE_VALUE = 4;		///< cpu_cycles_fine      (0..9 in the core)
constexpr int FINE_MULTIPLIER = 1000;	///< cpu_cycles_multiplier_fine

static_assert (COARSE_VALUE * COARSE_MULTIPLIER + FINE_VALUE * FINE_MULTIPLIER
		== CYCLES_DEFAULT,
	       "the coarse/fine spelling must add up to CYCLES_DEFAULT");

static_assert (COARSE_VALUE >= 0 && COARSE_VALUE <= 9,
	       "the core's cpu_cycles option only offers 0..9");

static_assert (FINE_VALUE >= 0 && FINE_VALUE <= 9,
	       "the core's cpu_cycles_fine option only offers 0..9");

// ---------------------------------------------------------------------------
// vga_hz: answered as "0", and nothing else
// ---------------------------------------------------------------------------
//
// The core has an option that REPLACES the emulated video mode's refresh rate
// instead of reporting it (src/hardware/vga_draw.cpp):
//
//     if (hz_override == 0) fps = clock/(vtotal*htotal);   // the mode's own
//     else                  fps = std::max(50, hz_override);
//
// .65 spent several rounds exposing it: a constant, then a /boot.conf key, then
// a "pin" that moved the pacer, the panel and the thread budget.  All of it was
// removed in .65g, and this is why, because the reason is worth keeping:
//
//   * it cannot make a machine that fails to reach its mode's rate reach it.  A
//     70 Hz mode presenting 34 frames stays at 34; this option only ever LOWERS
//     the bar, and lowering it makes the emulated machine run slower - a
//     different fault with a different cure;
//   * the only thing it could disguise was a pacer that was not metering at all,
//     and that is fixed properly in kernel.cpp (.65f);
//   * and it was unreadable from the outside: a setting that changes the speed
//     of the emulated machine, that dosbox.conf cannot set, and that needed a
//     star in the panel to be told apart from a machine that cannot keep up.
//
// So the port answers "0" - the core's own default, "leave the mode alone" - and
// there is no other value, no key in /boot.conf and nothing on the panel.  One
// less thing to explain and one less thing to get wrong.
//
// (The core does not even match this answer, which is its own quirk: the value
// list for this option is built from ints whose toString() is empty, so no
// frontend answer can match and CoreOptions::operator[] falls back to the option
// default.  That default is 0, so the effect is exactly what this file means.)
// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

/// The options this port answers.  The names are the core's keys verbatim
/// (libretro_core_options.h, CORE_OPT_CPU_CYCLES_* and CORE_OPT_CORE_VGA_REFRESH);
/// a typo here would silently fall through to the core default, so the test
/// checks them one by one against the strings as written there.
///
/// Note that "vga_hz" is in this table as "0" on purpose: answering it is how
/// the key name gets checked against the core, and "0" is the core's default, so
/// the entry is a no-op.  The value is what the frontend answers for it.
struct TVariable
{
	const char *pName;
	const char *pValue;
};

constexpr TVariable VARIABLES[] =
{
	{ "cpu_cycles_mode",		"fixed" },
	{ "cpu_cycles",			"2" },
	{ "cpu_cycles_multiplier",	"10000" },
	{ "cpu_cycles_fine",		"4" },
	{ "cpu_cycles_multiplier_fine",	"1000" },
	{ "vga_hz",			"0" },   // off: the core uses each mode's own rate
};

constexpr unsigned VariableCount = sizeof VARIABLES / sizeof VARIABLES[0];

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

/// The value this port answers for one core option, or nullptr when the option
/// is not ours to answer.
///
/// Deliberately a linear scan over six entries: it runs during startup and is
/// then never called again, and a std::map here would be a heap allocation
/// before newlib's allocator is warm for no benefit.
inline const char *Lookup (const char *pName)
{
	if (pName == nullptr)
	{
		return nullptr;
	}

	for (unsigned i = 0; i < VariableCount; i++)
	{
		if (strcmp (pName, VARIABLES[i].pName) == 0)
		{
			return VARIABLES[i].pValue;
		}
	}

	return nullptr;
}

/// Total number of emulated cycles per millisecond this port asks the core for.
/// Used by the kernel to put the same figure on screen as the one the emulator
/// is actually using: if the panel and the core ever disagree, the panel is
/// lying about the machine, and that is the kind of thing that hides for
/// several rounds.
constexpr int CyclesPerMilliSecond (void)
{
	return CYCLES_DEFAULT;
}

} // namespace bare_coreopts

#endif
