// coreopts_test.cpp - check-coreopts
//
// Three things are worth testing about bare_coreopts.h, and none of them is
// "does it compile":
//
//   1  the arithmetic.  The value that reaches the emulator is not the constant
//      in the header, it is `cpu_cycles x multiplier + cpu_cycles_fine x
//      fine_multiplier` evaluated by dosbox-core.  If the header says 24000 and
//      the spelling adds up to 20000, the header is a comment.
//
//   2  the spelling.  Every option name is a string that has to match the core's
//      key exactly; a typo does not fail, it falls through to the core default,
//      which is the bug this whole feature exists to fix.  So the names are
//      compared against the core's own definition header, found through
//      DOSBOX_REV's tree (see the Makefile recipe for where it looks).
//
//   3  vga_hz (.65) answers "0" and nothing else.  .65 to .65f tried to make it
//      a setting - a constant, a /boot.conf key, a pin that moved the pacer and
//      the panel - and it was removed because it never made a slow machine
//      fast, it only made it slower (see bare_coreopts.h).  The check that
//      remains is the one that keeps it removed: the answer is "0", the table
//      has no second source of truth, and the range the core would silently
//      clamp is still documented.
//
// The second test skips itself when the core sources are not present, and says
// so out loud.  A check that quietly passes because it could not look is worse
// than no check: that is the false green this port keeps paying for.

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bare_coreopts.h"

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

// --- 1. the arithmetic ------------------------------------------------------

static void TestArithmetic (void)
{
	printf ("aritmetica\n");

	// Read the five answered values back through the lookup, the same way the
	// frontend does, and evaluate the core's expression with them.  Not with
	// the constants: the point is that the *strings* add up.
	const char *pMode = bare_coreopts::Lookup ("cpu_cycles_mode");
	const char *pCoarse = bare_coreopts::Lookup ("cpu_cycles");
	const char *pCoarseMul = bare_coreopts::Lookup ("cpu_cycles_multiplier");
	const char *pFine = bare_coreopts::Lookup ("cpu_cycles_fine");
	const char *pFineMul = bare_coreopts::Lookup ("cpu_cycles_multiplier_fine");

	Expect (pMode != nullptr && strcmp (pMode, "fixed") == 0,
		"mode is fixed (auto/max would make the figure a ceiling, not a speed)");
	Expect (pCoarse != nullptr && pCoarseMul != nullptr
		&& pFine != nullptr && pFineMul != nullptr,
		"all five options are answered");

	if (s_nFailures != 0)
	{
		return;
	}

	const int nCycles = atoi (pCoarse) * atoi (pCoarseMul)
			  + atoi (pFine) * atoi (pFineMul);

	char Msg[128];
	snprintf (Msg, sizeof Msg, "the five values evaluate to %d", bare_coreopts::CYCLES_DEFAULT);
	Expect (nCycles == bare_coreopts::CYCLES_DEFAULT, Msg);

	// 486DX2-66 is 23880 on the core's scale.  The grid cannot express it, so
	// what is checked is that we are on the 486DX2 side of 20000 and not on
	// the 386DX side (6075), which is the failure this guards against.
	snprintf (Msg, sizeof Msg, "%d is within 1%% of 23880 (486DX2-66)", nCycles);
	Expect (nCycles > 23641 && nCycles < 24119, Msg);

	snprintf (Msg, sizeof Msg, "%d is not 386DX class (6075) nor 486DX-33 (12010)", nCycles);
	Expect (nCycles > 15000, Msg);
}

// --- 1c. vga_hz: answered as "0", and that is all ---------------------------

static void TestVgaHz (void)
{
	printf ("vga_hz: la respuesta es \"0\"\n");

	// The table owns the key (that is how a typo in the core's name would be
	// caught by the spelling test below) and the value is the core's own
	// default, so answering it changes nothing.  Two assertions, because they
	// are two different promises: the key is ours, and the value is inert.
	const char *pAnswered = bare_coreopts::Lookup ("vga_hz");
	Expect (pAnswered != nullptr, "vga_hz es una clave que este puerto responde");
	Expect (pAnswered != nullptr && strcmp (pAnswered, "0") == 0,
		"la respuesta es \"0\": el core usa la tasa de cada modo");

	// The one thing that must not come back: a second number for the same
	// question.  .65 had VGA_HZ and a string derived from it; .65d had a
	// /boot.conf key as well; and the bug those produced was a build that said
	// 60 in the table and paced at 0.  If a constant reappears here, this test
	// is where it has to be noticed.
	Expect (bare_coreopts::Lookup ("vga_hz_pin") == nullptr,
		"no hay segunda clave para fijar la tasa (ni constante, ni boot.conf)");

	// The range is written down even though nothing sets it any more, because
	// it is the reason the setting was useless in the first place: the core
	// applies max(50, value), so 1 and 50 are the same and 34 - the number the
	// panel actually showed - cannot be expressed at all.
	printf ("  nota   el core hace max(50, vga_hz): no puede expresar 34 ni 70.086\n");
}

// --- 2. the spelling -------------------------------------------------------

// Reads one `inline constexpr const char* NAME = "value";` out of the core's
// libretro_core_options.h.  Deliberately dumb: the line is searched for the
// identifier, then for the first quoted string after it.  If the core ever
// changes the shape of that file this returns "" and the test says so.
static std::string ReadCoreKey (const char *pHeader, const char *pIdentifier)
{
	FILE *pFile = fopen (pHeader, "r");
	if (pFile == nullptr)
	{
		return "";
	}

	std::string Result;
	char Line[512];
	const size_t nLen = strlen (pIdentifier);

	while (fgets (Line, sizeof Line, pFile) != nullptr)
	{
		std::string Text (Line);

		// Exact identifier, not a prefix of a longer one.  This is not
		// pedantry: CORE_OPT_CPU_CYCLES is a prefix of
		// CORE_OPT_CPU_CYCLES_MODE, and the first version of this test
		// matched the wrong line and reported "cpu_cycles_mode" for
		// CORE_OPT_CPU_CYCLES.  A substring search here would pass for the
		// wrong reason as easily as fail for the wrong reason.
		size_t nPos = Text.find (pIdentifier);
		if (nPos == std::string::npos)
		{
			continue;
		}
		if (nPos > 0 && (isalnum (static_cast<unsigned char> (Text[nPos - 1]))
				 || Text[nPos - 1] == '_'))
		{
			continue;
		}

		const size_t nAfter = nPos + nLen;
		if (nAfter < Text.size ()
			&& (isalnum (static_cast<unsigned char> (Text[nAfter]))
			    || Text[nAfter] == '_'))
		{
			continue;
		}

		// The definition, not a use: the identifier must be followed by '='.
		if (Text.find ('=', nAfter) == std::string::npos)
		{
			continue;
		}

		size_t nQuote = Text.find ('"', nAfter);
		if (nQuote == std::string::npos)
		{
			continue;
		}
		size_t nEnd = Text.find ('"', nQuote + 1);
		if (nEnd == std::string::npos)
		{
			break;
		}

		Result = Text.substr (nQuote + 1, nEnd - nQuote - 1);
		break;
	}

	fclose (pFile);
	return Result;
}

static void TestAgainstCore (int argc, char **argv)
{
	printf ("claves contra el core\n");

	if (argc < 2)
	{
		printf ("  OMITIDO  no se ha pasado la ruta del core; nada comprobado aqui\n");
		return;
	}

	const char *pHeader = argv[1];

	// The core's identifier -> the key this port answers, in that order.
	static const struct { const char *pIdentifier; const char *pAnswered; } Map[] =
	{
		{ "CORE_OPT_CPU_CYCLES_MODE",		"cpu_cycles_mode"	},
		{ "CORE_OPT_CPU_CYCLES",		"cpu_cycles"		},
		{ "CORE_OPT_CPU_CYCLES_MULTIPLIER",	"cpu_cycles_multiplier"	},
		{ "CORE_OPT_CPU_CYCLES_FINE",		"cpu_cycles_fine"	},
		{ "CORE_OPT_CPU_CYCLES_MULTIPLIER_FINE",	"cpu_cycles_multiplier_fine" },
		{ "CORE_OPT_CORE_VGA_REFRESH",		"vga_hz"		},
	};

	unsigned nChecked = 0;
	for (const auto& Entry : Map)
	{
		const std::string Key = ReadCoreKey (pHeader, Entry.pIdentifier);
		if (Key.empty ())
		{
			char Msg[160];
			snprintf (Msg, sizeof Msg, "%s not found in the core header (%s)",
				  Entry.pIdentifier, pHeader);
			Expect (false, Msg);
			continue;
		}

		char Msg[200];
		snprintf (Msg, sizeof Msg, "%s is \"%s\" in the core and \"%s\" here",
			  Entry.pIdentifier, Key.c_str (), Entry.pAnswered);
		Expect (Key == Entry.pAnswered, Msg);

		if (Key == Entry.pAnswered)
		{
			nChecked++;
		}
	}

	char Msg[96];
	snprintf (Msg, sizeof Msg, "%u of %u keys found in the core header",
		  nChecked, (unsigned) (sizeof Map / sizeof Map[0]));
	Expect (nChecked == sizeof Map / sizeof Map[0], Msg);
}

// --- 3. the fence ----------------------------------------------------------

static void TestFence (void)
{
	printf ("la valla\n");

	// Everything else has to fall through to "not ours", or the port would be
	// answering options it knows nothing about.
	Expect (bare_coreopts::Lookup ("cpu_core") == nullptr,
		"cpu_core is not answered here (the core's own default is wanted)");
	Expect (bare_coreopts::Lookup ("") == nullptr, "the empty name is not answered");
	Expect (bare_coreopts::Lookup ("cpu_cycles_mode_x") == nullptr,
		"a near miss of a real key is not answered");
	Expect (bare_coreopts::Lookup ("vga_hz_x") == nullptr,
		"a near miss of vga_hz is not answered");
	Expect (bare_coreopts::Lookup ("vga_refresh") == nullptr,
		"the option's description is not its key: vga_refresh is not answered");
	Expect (bare_coreopts::Lookup (nullptr) == nullptr, "nullptr is not answered");
}

int main (int argc, char **argv)
{
	TestArithmetic ();
	TestVgaHz ();
	TestAgainstCore (argc, argv);
	TestFence ();

	printf ("\n");
	if (s_nFailures != 0)
	{
		printf ("%u fallos\n", s_nFailures);
		return 1;
	}

	printf ("0 fallos\n");
	return 0;
}
