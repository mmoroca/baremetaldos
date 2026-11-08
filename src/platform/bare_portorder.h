//
// bare_portorder.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.
//
// When this port is allowed to call retro_set_controller_port_device().
//
// The rule and its reason
// -----------------------
// retro_set_controller_port_device() looks like a frontend call - "tell the
// core what is plugged into port N, touch a flag, return".  In dosbox-core it is
// not.  Its implementation (libretro/src/libretro.cpp, pinned at 7bcf083e) ends
// with
//
//     connected[port] = ...;  gamepad[port] = ...;
//     libretro_input_init();
//
// and libretro_input_init() *writes a DOSBox variable* to match:
//
//     update_dosbox_variable(false, "joystick", "joysticktype", "4axis");
//
// whose body is
//
//     Section *section = control->GetSection("joystick");
//     Section_prop *secprop = static_cast<Section_prop *>(section);
//     if (secprop) { section->ExecuteDestroy(false); ... }
//
// `control` is `Config * control` in dosbox-core's src/dosbox.cpp: a global with
// no initialiser, i.e. null.  It is assigned in start_dosbox(), the function the
// *emulation task* runs:
//
//     static void start_dosbox(const std::string cmd_line)
//     {
//         ...
//         Config myconf(&com_line);
//         control = &myconf;            <-- here
//         ...
//         control->Init();
//         switchThread();               <-- first hand-back, to us
//         control->StartUp();
//
// Inside retro_load_game() the emulation task is started and the frontend
// blocks until that first hand-back, so from the moment retro_load_game()
// returns, `control` points at a live Config whose sections exist: safe.
// *Before* it, `control` is null and GetSection() is a member call on a null
// this, which reads near address zero.  On the board that is a data abort with
// no message - and the last two lines still on screen are ours
// ("core: load ...") and DOSBox's ("One port connected, enabling 4 buttons in
// connected port."), because the abort happens one instruction after the log
// line.  That is exactly what .61 did:
//
//     with a pad     -> stopped after "core: load" + the port line
//     without a pad  -> stopped after "core: load"      (device NONE takes the
//                       core's "no ports" branch, which calls
//                       update_dosbox_variable() too, and prints nothing)
//
// The .60 round put the call *before* retro_load_game() on the reasoning "this
// is when DOSBox reads the controller layout".  That reasoning is wrong in the
// only way that matters: DOSBox reads the layout in libretro_input_init(), which
// also runs on every video mode change and every core-option change, and the
// first video mode change happens inside retro_load_game() - after `control` is
// live.  So announcing after the load is not later in any sense that matters,
// and it is the difference between a dead machine and a working one.
//
// Second half of the rule
// -----------------------
// Every call tears the input map down and rebuilds it (update_dosbox_variable
// with autoexec=false runs ExecuteDestroy() then ExecuteInit() on the section).
// That is far too heavy - and, once the machine is running, too invasive - to
// do on a whim, so re-announcements are metered: a state change is announced at
// once, and a pad that flaps produces at most one call per
// PortAnnounceMinMicros.
//
// Why this lives in a header of its own
// -------------------------------------
// So that `make check-portorder` can exercise the ordering and the metering on
// the host, with no core and no Circle, and so that the regression that cost a
// boot ("announced too early") is a failing test rather than a comment.
//
// No Circle and no libretro types here on purpose: the logic is integer
// bookkeeping and the caller does the two things only it can do - ask the
// firmware whether a pad exists, and make the libretro call.
//
#pragma once
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.

namespace bare_portorder {

/// \brief How often a *repeat* announcement may happen, in microseconds.
///
/// One state change is announced immediately.  This bound only applies to
/// further calls inside the same window, i.e. to a pad whose USB reports make
/// it appear to come and go.  A quarter of a second is longer than any USB
/// re-enumeration and short enough that a real re-plug is seen at once.
constexpr unsigned long long PortAnnounceMinMicros = 250000ULL;

/// \brief Ports this port will ever announce.  Port 0 only: the emulated
///\t   machine gets one pad, which is what DOSBox's own two-port mapping
///\t   reduces to "2 axes / 2 buttons" anyway.
constexpr unsigned Ports = 1;

/// \brief What the state machine wants done about one port.
enum class TAction
{
	Nothing,	///< nothing to do, or not safe yet
	Announce,	///< call retro_set_controller_port_device() now
};

struct TState
{
	/// TRUE once retro_load_game() has returned.  Until then no call is
	/// allowed, because `control` inside the core is still null.
	bool bLoaded;
	/// What the input layer says is plugged in, per port.
	bool bPadPresent[Ports];
	/// What the core has last been told, per port.
	bool bAnnounced[Ports];
	/// When the last announcement happened, 0 if never.
	unsigned long long nLastAnnounceMicros;
	/// Announcements made, for the panel/log - and for the test.
	unsigned nAnnouncements;
};

/// \brief A state that has never loaded anything and has announced nothing.
inline TState NewState (void)
{
	TState State {};

	State.bLoaded = false;
	State.nLastAnnounceMicros = 0;
	State.nAnnouncements = 0;
	for (unsigned i = 0; i < Ports; i++)
	{
		State.bPadPresent[i] = false;
		State.bAnnounced[i] = false;
	}

	return State;
}

/// \brief retro_load_game() has returned: from here on the core is reachable.
///
/// It does *not* announce anything by itself.  The caller announces
/// unconditionally right after this (Step 1 below) so that the first state is
/// always the truth, even when that truth is "no pad".
inline void NoteLoaded (TState &State)
{
	State.bLoaded = true;
}

/// \brief Truth from the input layer.  Cheap; call it every frame.
inline void SetPadPresent (TState &State, unsigned nPort, bool bPadPresent)
{
	if (nPort < Ports)
	{
		State.bPadPresent[nPort] = bPadPresent;
	}
}

/// \brief TRUE if the core can be talked to at all right now.
inline bool Safe (const TState &State)
{
	return State.bLoaded;
}

/// \brief What to do about this port at this instant.
///
/// Three rules, in order, and all three have a boot behind them:
///
///   1  not loaded -> Nothing.  This is the .61 abort; a test pins it.
///   2  already what the core was told -> Nothing.  Without this, hot-plug
///      detection would rebuild the core's input map every frame.
///   3  a change, but inside the metering window -> Nothing (it stays
///      pending; nothing is lost, the next call after the window announces it).
inline TAction Due (const TState &State, unsigned nPort, unsigned long long nNowMicros)
{
	if (!State.bLoaded || nPort >= Ports)
	{
		return TAction::Nothing;
	}

	const bool bWant = State.bPadPresent[nPort];
	if (State.bAnnounced[nPort] == bWant && State.nAnnouncements != 0)
	{
		// Already announced once and unchanged.  The `nAnnouncements != 0`
		// term is not redundant: at boot the core is told nothing yet, so
		// "announced == want == false" means "we have never said a word",
		// which has to be announced (the core's default is
		// "Keyboard + Mouse", not "none").
		return TAction::Nothing;
	}

	if (State.nAnnouncements != 0
	    && nNowMicros - State.nLastAnnounceMicros < PortAnnounceMinMicros)
	{
		return TAction::Nothing;
	}

	return TAction::Announce;
}

/// \brief Record that the caller did the call.
inline void NoteAnnounced (TState &State, unsigned nPort, unsigned long long nNowMicros)
{
	if (nPort < Ports)
	{
		State.bAnnounced[nPort] = State.bPadPresent[nPort];
	}

	State.nLastAnnounceMicros = nNowMicros;
	State.nAnnouncements++;
}

/// \brief The libretro device id for a port, as plain integers.
///
/// RETRO_DEVICE_NONE is 0 and RETRO_DEVICE_JOYPAD is 1 in libretro.h.  Kept as
/// integers here so this header needs no libretro.h, with a static_assert at
/// the call site (bare_libretro.cpp) that the real enum still agrees - if the
/// numbers ever drift, that is a compile error there and not a wrong device
/// silently passed to the core.
inline unsigned DeviceFor (bool bPadPresent)
{
	return bPadPresent ? 1u : 0u;
}

constexpr unsigned DeviceNone = 0;
constexpr unsigned DeviceGamepad = 1;

} // namespace bare_portorder
