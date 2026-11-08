// dynarec_target_test.cpp - check-dynarec-target
//
// The dynrec's backend is chosen at preprocess time:
//
//     #if C_TARGETCPU == ARMV8LE
//     #include "core_dynrec/risc_armv8le.h"
//     #endif
//
// so a wrong -DC_TARGETCPU does not fail loudly in a way anybody notices - it
// either stops the build with a preprocessor error (the loud case, which is
// what .62 would have hit) or, worse, evaluates to 0 and silently selects no
// backend at all while the JIT's own sources still compile.
//
// This test is compiled with the EXACT flag the Makefile passes, and it checks
// two things:
//
//   1  the flag arrives, and arrives as a number the comparison can use;
//   2  the number is the one core_dynrec.cpp uses for ARMV8LE (0x07).
//
// It also refuses the quoted spelling by name, because that is the mistake this
// port actually made: -DC_TARGETCPU=\"ARMV8LE\" is a string, and
// `#if "ARMV8LE" == 0x07` is not a comparison, it is a syntax error.
//
// No Circle, no libretro, no DOSBox: two macros and a static_assert.

#include "bare_dynarec_flags.h"

#if !defined(C_TARGETCPU)
#error "C_TARGETCPU is not defined: the dynarec build did not pass it"
#endif

// The bare token from BAREDYN_TARGET_TOKEN, which is ARMV8LE, which is 0x07 in
// core_dynrec.cpp.  If the Makefile quotes the value, this line fails in the
// preprocessor with "token is not valid in preprocessor expressions" - which is
// the whole point of writing it this way.
#if C_TARGETCPU != ARMV8LE
#error "C_TARGETCPU is not ARMV8LE: the AArch64 backend would not be selected"
#endif

static_assert (C_TARGETCPU == 0x07,
	       "core_dynrec.cpp says ARMV8LE is 0x07; if upstream renumbered it, "
	       "bare_dynarec_flags.h has to change with it");

static_assert (C_TARGETCPU != BAREDYN_TARGET_ARMV7LE,
	       "ARMV7LE is the 32-bit ARM backend (risc_armv4le.h): wrong engine");

// A runtime check as well, so that the test binary says something out loud and
// can be run by a human without reading the exit code.
#include <cstdio>

int main (void)
{
	printf ("check-dynarec-target: C_TARGETCPU = %d (ARMV8LE = %d)\n",
		(int) C_TARGETCPU, (int) BAREDYN_TARGET_ARMV8LE);

	if ((int) C_TARGETCPU != (int) BAREDYN_TARGET_ARMV8LE)
	{
		printf ("  FALLO  el flag no selecciona el backend AArch64\n");
		return 1;
	}

	printf ("  ok     el backend que se compila es risc_armv8le.h\n");
	printf ("\n0 fallos\n");

	return 0;
}
