// portorder_test.cpp - check-portorder
//
// The .61 boot stopped dead with a pad attached (and, when it did not, stopped
// one line earlier).  The cause was ours: retro_set_controller_port_device()
// was called *before* retro_load_game(), and inside dosbox-core that function
// writes a DOSBox variable through `control->GetSection()`, where `control` is
// the Config the *emulation task* constructs - i.e. null until the load has
// started that task.  bare_portorder.h has the whole account.
//
// So the state machine that decides when to announce is worth a test, and this
// is it: no core, no Circle, no libretro.h - just the rules, including the one
// that cost the boot.
//
// What is checked:
//
//   1  before the load: Nothing.  (The .61 regression, pinned.)
//   2  after the load: exactly one unconditional Announce, even for "no pad" -
//      the core's default is "Keyboard + Mouse", so silence is a state too.
//   3  a stable pad: no further calls, ever.  Every call tears down and
//      rebuilds the core's input map.
//   4  a hot-plug: announced at once.
//   5  a pad that flaps: at most one call per metering window, and the state
//      is not lost - the next call after the window announces it.
//   6  the device ids still agree with libretro.h's numbers.

#include <cstdio>

#include "bare_portorder.h"

using namespace bare_portorder;

static unsigned s_nFailures = 0;

static void Expect (bool bCondition, const char *pWhat)
{
	if (!bCondition)
	{
		printf ("  FALLO  %s\n", pWhat);
		s_nFailures++;
	}
	else
	{
		printf ("  ok     %s\n", pWhat);
	}
}

// One call of the decision machinery, as bare_libretro.cpp does it: ask whether
// an announcement is due, and if so make it.  Returns TRUE if it announced.
static bool Step (TState &State, bool bPadPresent, unsigned long long nNowMicros)
{
	SetPadPresent (State, 0, bPadPresent);

	if (Due (State, 0, nNowMicros) != TAction::Announce)
	{
		return false;
	}

	NoteAnnounced (State, 0, nNowMicros);

	return true;
}

int main (void)
{
	printf ("check-portorder: cuando se le puede hablar al core del game port\n");

	// --- 1  the regression -------------------------------------------------
	{
		TState State = NewState ();

		// A pad is already plugged in at boot, and every line of the load
		// sequence runs before retro_load_game() has returned.
		Expect (!Step (State, true, 1000),
			"con mando, antes de retro_load_game: NO se anuncia");
		Expect (State.nAnnouncements == 0,
			"antes de la carga no se ha anunciado nada");
		Expect (!Safe (State),
			"antes de la carga el core no es alcanzable");

		// And the machine without a pad takes the same route.
		TState NoPad = NewState ();
		Expect (!Step (NoPad, false, 1000),
			"sin mando, antes de retro_load_game: NO se anuncia");
	}

	// --- 2  the first announcement after the load -------------------------
	{
		TState State = NewState ();
		NoteLoaded (State);
		Expect (Safe (State), "despues de la carga el core es alcanzable");

		Expect (Step (State, true, 1000),
			"con mando, despues de la carga: se anuncia");
		Expect (State.nAnnouncements == 1, "exactamente un anuncio");

		// No pad has to be said out loud as well: it is not "nothing to
		// do", it is "port 0 = none", which is what switches the emulated
		// game port off deliberately instead of leaving the core's
		// keyboard-and-mouse default in place.
		TState NoPad = NewState ();
		NoteLoaded (NoPad);
		Expect (Step (NoPad, false, 1000),
			"sin mando, despues de la carga: tambien se anuncia");
		Expect (NoPad.bAnnounced[0] == false
			&& NoPad.nAnnouncements == 1,
			"sin mando el core queda con port 0 = none");
	}

	// --- 3  a stable pad is silent ----------------------------------------
	{
		TState State = NewState ();
		NoteLoaded (State);
		Step (State, true, 1000);

		bool bAny = false;
		for (unsigned n = 1; n <= 10000; n++)
		{
			if (Step (State, true, 1000 + n * 16666ULL))
			{
				bAny = true;
			}
		}

		Expect (!bAny, "un mando estable no genera ni un anuncio mas (10.000 frames)");
		Expect (State.nAnnouncements == 1, "sigue habiendo un solo anuncio");
	}

	// --- 4  hot-plug -------------------------------------------------------
	{
		TState State = NewState ();
		NoteLoaded (State);
		Step (State, false, 1000);	// arranca sin mando

		Expect (!Step (State, false, 20000), "sin mando, sigue callado");

		// Plugged in a second later: announced at once, with no window to
		// wait for.
		Expect (Step (State, true, 1000000), "mando conectado en caliente: se anuncia ya");
		Expect (State.bAnnounced[0], "el core sabe que hay mando");
		Expect (!Step (State, true, 3000000), "y no se repite");

		// Unplugged: also at once.
		Expect (Step (State, false, 3000000 + PortAnnounceMinMicros),
			"mando desconectado: se anuncia");
		Expect (!State.bAnnounced[0], "el core sabe que ya no hay mando");
	}

	// --- 5  a pad that flaps ----------------------------------------------
	{
		TState State = NewState ();
		NoteLoaded (State);
		Step (State, true, 1000);

		// Ten state changes inside one window: one announcement, not ten.
		unsigned long long nNow = 2000000;
		unsigned nCalls = 0;
		for (unsigned n = 0; n < 10; n++)
		{
			if (Step (State, n % 2 == 0, nNow))
			{
				nCalls++;
			}
			nNow += 10000;	// 10 ms apart
		}
		Expect (nCalls == 1, "un mando que parpadea: un solo anuncio por ventana");
		Expect (State.nAnnouncements == 2, "el primero de la carga mas ese");

		// The last flap left the state at "no pad", which is what the core
		// was last told, so there is nothing pending: quiet is correct.
		Expect (!Step (State, false, nNow),
			"pasada la ventana, si el core ya sabe la verdad no se le molesta");

		// A change *after* the window goes out at once - the metering must
		// not be able to lose one.
		nNow += PortAnnounceMinMicros;
		Expect (Step (State, true, nNow),
			"pasada la ventana, un cambio real se anuncia");
		Expect (State.bAnnounced[0], "y el core queda con mando");
	}

	// --- 6  the ids -------------------------------------------------------
	{
		Expect (DeviceNone == 0, "RETRO_DEVICE_NONE es 0");
		Expect (DeviceGamepad == 1, "RETRO_DEVICE_JOYPAD es 1");
		Expect (DeviceFor (true) == DeviceGamepad, "con mando se pide JOYPAD");
		Expect (DeviceFor (false) == DeviceNone, "sin mando se pide NONE");
		Expect (Ports == 1, "este port solo anuncia el puerto 0");
	}

	printf ("\n%d fallos\n", (int) s_nFailures);

	return s_nFailures != 0 ? 1 : 0;
}
