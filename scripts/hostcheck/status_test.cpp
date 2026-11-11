//
// status_test.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// The boot screen of the port: the two fixed lines in their MS-DOS box and the
// scrolling column underneath, run for real on the host.
//
// Why this exists: BareVideoStatusPrint() is a state machine (pin the first two
// lines, flow the rest, scroll when the column fills) plus a drawing routine
// (CP437 box glyphs across the full 18-pixel line pitch).  Both are invisible to
// every other check in this tree - check-kernel compiles bare_video.cpp but
// never runs it, and the only other way to see the result is to flash a card and
// look at the screen, which is how the last three versions of this code were
// judged.  "It compiled" is not a test of an ordering, and "it looked right on
// the photo" is not a test of the rows.
//
// How it works, and why it is not a copy: bare_video.cpp is INCLUDED textually,
// so the code under test is the file that ships - not a paraphrase of it - and
// its file-scope state (s_pFrameBuffer, s_nModeWidth, s_nModeHeight) is in scope
// for the test to set up, which is the one thing the file does not expose.  The
// Circle objects it touches (frame buffer, char generator, cache maintenance)
// are stand-ins, exactly like the ones make check-kernel uses; the font is a
// fake glyph, because what is being checked is WHERE ink lands, not what a
// character looks like.
//
// What it checks, in the order the boot produces it:
//
//   1. the first two lines after BareVideoStatusReset() land inside the box at
//      rows 0..3, and the box is drawn as a box: a continuous top border and a
//      vertical bar with no gap between the two corners (the 2-pixel inter-line
//      gap is the defect this catches - Circle's font is 16 px and the line
//      pitch is 18);
//   2. everything after them flows BELOW the box, one message per row;
//   3. once the column is full it scrolls: the row under the box changes, the
//      newest message is on the last row, and the box is bit-for-bit the same
//      as before the scroll;
//   4. BareVideoStatusEnable() repaints the box and replays the scrolling rows
//      after the frame loop has wiped the screen;
//   5. a small mode (320x240) and a message longer than the screen: ink stays
//      inside the mode, the header still does not move.
//
// It says nothing about the real Circle font or about the real frame buffer's
// cache behaviour: those are the two things only the board can answer.
//
#include <circle/types.h>
#include <circle/bcmframebuffer.h>
#include <circle/chargenerator.h>
#include <circle/synchronize.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Stand-ins for the Circle objects bare_video.cpp talks to.
// ---------------------------------------------------------------------------
static u32     *g_pPixels  = nullptr;
static unsigned g_nWidth   = 0;
static unsigned g_nHeight  = 0;
static unsigned g_nPitch   = 0;

CBcmFrameBuffer::CBcmFrameBuffer (unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) {}
boolean CBcmFrameBuffer::Initialize (void) { return TRUE; }
u32 CBcmFrameBuffer::GetWidth (void) const { return g_nWidth; }
u32 CBcmFrameBuffer::GetHeight (void) const { return g_nHeight; }
u32 CBcmFrameBuffer::GetVirtWidth (void) const { return g_nWidth; }
u32 CBcmFrameBuffer::GetVirtHeight (void) const { return g_nHeight; }
u32 CBcmFrameBuffer::GetPitch (void) const { return g_nPitch; }
u32 CBcmFrameBuffer::GetSize (void) const { return g_nPitch * g_nHeight; }
u32 CBcmFrameBuffer::GetDepth (void) const { return 32; }
uintptr CBcmFrameBuffer::GetBuffer (void) const
{
	return reinterpret_cast<uintptr> (g_pPixels);
}
boolean CBcmFrameBuffer::SetVirtualOffset (u32, u32) { return TRUE; }

// Circle's real font is 8x16.  The glyph here is deliberately trivial: one full
// row and two vertical edges, enough to see that a character was drawn at a
// position and that it did not bleed into the row above or below.
unsigned CCharGenerator::GetCharWidth (void) const { return 8; }
unsigned CCharGenerator::GetCharHeight (void) const { return 16; }
const u8 *CCharGenerator::GetCharPointer (char) const { return nullptr; }
u32 CCharGenerator::GetPixelLine (char, unsigned nLine) const
{
	return nLine == 0 ? 0xFFU : (nLine < 16 ? 0x81U : 0x00U);
}

// No cache on the host: the calls have to exist and do nothing.
void CleanAndInvalidateDataCacheRange (u32, u32) { }
void SyncDataAndInstructionCache (void) { }

// ---------------------------------------------------------------------------
// The file under test, verbatim.
// ---------------------------------------------------------------------------
#include "platform/bare_video.cpp"

// ---------------------------------------------------------------------------
// Reading the picture back.
// ---------------------------------------------------------------------------
static const unsigned kLineH = 18;	// bare_video.cpp: char height + 2
static const unsigned kTop   = 8;	// bare_video.cpp: STATUS_TOP
static const unsigned kHalf  = 8;	// half a 16-pixel glyph: the middle line

static unsigned BandY (unsigned nRow) { return kTop + nRow * kLineH; }

static u32 Pixel (unsigned x, unsigned y)
{
	return g_pPixels[y * (g_nPitch / 4) + x];
}

static bool BandIsBlank (unsigned nRow)
{
	unsigned nEnd = BandY (nRow) + kLineH;
	if (nEnd > g_nHeight) nEnd = g_nHeight;

	for (unsigned y = BandY (nRow); y < nEnd; y++)
	{
		for (unsigned x = 0; x < g_nWidth; x++)
		{
			if (Pixel (x, y) != 0) return false;
		}
	}

	return true;
}

static bool BandHasInk (unsigned nRow) { return !BandIsBlank (nRow); }

static unsigned BandHash (unsigned nRow)
{
	unsigned nHash = 2166136261u;

	for (unsigned y = BandY (nRow); y < BandY (nRow) + kLineH; y++)
	{
		for (unsigned x = 0; x < g_nWidth; x++)
		{
			nHash ^= Pixel (x, y);
			nHash *= 16777619u;
		}
	}

	return nHash;
}

// The four rows of the box, hashed together: the acceptance criterion is "the
// header does not move", so anything that changes it - by one pixel - fails.
static unsigned HeaderHash (void)
{
	unsigned nHash = 2166136261u;

	for (unsigned nRow = 0; nRow < 4; nRow++)
	{
		nHash ^= BandHash (nRow);
		nHash *= 16777619u;
	}

	return nHash;
}

static unsigned s_nFails = 0;

static void Check (bool bOk, const char *pWhat)
{
	std::printf ("  %s  %s\n", bOk ? "ok  " : "FAIL", pWhat);
	if (!bOk) s_nFails++;
}

static void SetMode (unsigned nWidth, unsigned nHeight)
{
	if (g_pPixels != nullptr)
	{
		std::free (g_pPixels);
		g_pPixels = nullptr;
	}

	g_nWidth = nWidth;
	g_nHeight = nHeight;
	g_nPitch = nWidth * 4;
	g_pPixels = static_cast<u32 *> (std::calloc (g_nPitch * nHeight, 1));

	delete s_pFrameBuffer;
	s_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 32);

	s_nModeWidth = nWidth;
	s_nModeHeight = nHeight;

	BareVideoFill (0x00000000);
}

// The two Report() calls the kernel makes straight after BareVideoStatusReset()
// (kernel.cpp: "BM-DOS ..." then "by @mmoroca ...").
static void BootHeader (void)
{
	BareVideoStatusPrint ("BM-DOS .80 - DOSBox on baremetal Raspberry Pi");
	BareVideoStatusPrint ("by @mmoroca & arena.ai 2026");
}

int main (void)
{
	SetMode (640, 480);

	std::printf ("[1] las dos primeras lineas van al recuadro de arriba\n");
	BareVideoStatusReset ();
	BootHeader ();
	const unsigned nHeaderPinned = HeaderHash ();

	Check (BandHasInk (0), "la fila 0 lleva el borde superior del recuadro");
	Check (BandHasInk (1), "la fila 1 lleva 'BM-DOS ...'");
	Check (BandHasInk (2), "la fila 2 lleva 'by @mmoroca ...'");
	Check (BandHasInk (3), "la fila 3 lleva el borde inferior");
	Check (BandIsBlank (4), "debajo del recuadro no hay nada todavia");

	// The box glyph in x=8..15 is 0xB3 (vertical bar), which is bit 3 of the
	// character: x = 8 + 3.  The corners turn at their middle line, so the run
	// goes from the middle of the top border to the middle of the bottom one -
	// and the point of the check is that there is no hole in between, which is
	// what the 2-pixel inter-line gap used to leave.
	unsigned nBar = 0;
	for (unsigned y = BandY (0) + kHalf; y <= BandY (3) + kHalf; y++)
	{
		if (Pixel (8 + 3, y) != 0) nBar++;
	}
	Check (nBar == 3 * kLineH + 1,
	       "la barra vertical no tiene huecos entre las dos esquinas");

	// The top border is 0xC4 (horizontal), which is the whole 8-pixel row.
	unsigned nTopRun = 0;
	for (unsigned x = 8 + 8; x < 8 + 8 * 8; x++)
	{
		if (Pixel (x, BandY (0) + kHalf) != 0) nTopRun++;
	}
	Check (nTopRun == 7 * 8, "el borde superior es una linea corrida entre esquinas");

	std::printf ("\n[2] los mensajes siguientes fluyen debajo, uno por fila\n");
	for (unsigned i = 0; i < 6; i++)
	{
		char Msg[64];
		std::snprintf (Msg, sizeof Msg, "mensaje de arranque %u", i);
		BareVideoStatusPrint (Msg);
	}
	for (unsigned r = 4; r < 10; r++)
	{
		char What[80];
		std::snprintf (What, sizeof What, "la fila %u lleva el mensaje %u", r, r - 4);
		Check (BandHasInk (r), What);
	}
	Check (HeaderHash () == nHeaderPinned,
	       "el recuadro no cambia al escribir debajo");

	std::printf ("\n[3] columna llena: scroll real y cabecera quieta\n");
	const unsigned nLines = BareVideoStatusLines ();
	std::printf ("       (%u filas en 640x480)\n", nLines);
	const unsigned nRow4Before = BandHash (4);
	const unsigned nLastBefore = BandHash (nLines - 1);

	for (unsigned i = 0; i < 4 * nLines; i++)	// mas mensajes que filas, de sobra
	{
		char Msg[64];
		std::snprintf (Msg, sizeof Msg, "mensaje tardio %u", i);
		BareVideoStatusPrint (Msg);
	}

	Check (HeaderHash () == nHeaderPinned, "tras desbordar, la cabecera sigue igual");
	Check (BandHash (4) != nRow4Before, "la fila 4 ha cambiado: el texto ha subido");
	Check (BandHash (nLines - 1) != nLastBefore,
	       "la ultima fila lleva el mensaje mas reciente");
	Check (BandIsBlank (nLines) || nLines == STATUS_MAX_LINES,
	       "no se escribe ni una fila por debajo de la ultima que cabe");

	std::printf ("\n[4] la columna se rearma tras un frame (BareVideoStatusEnable)\n");
	s_bStatusEnabled = false;
	BareVideoStatusEnable ();
	Check (HeaderHash () == nHeaderPinned, "al rearmar, el recuadro se repinta igual");
	Check (BandHasInk (4), "y las filas de scroll vuelven a estar ahi");

	std::printf ("\n[5] modo pequeno (320x240) y mensaje mas largo que la pantalla\n");
	SetMode (320, 240);
	BareVideoStatusReset ();
	BootHeader ();
	const unsigned nHeaderSmall = HeaderHash ();
	Check (BandHasInk (3), "el recuadro cabe entero en 240 lineas");

	char Long[200];
	std::memset (Long, 'M', sizeof Long - 1);
	Long[sizeof Long - 1] = '\0';
	BareVideoStatusPrint (Long);
	for (unsigned i = 0; i < 40; i++)
	{
		BareVideoStatusPrint ("linea suelta");
	}

	Check (HeaderHash () == nHeaderSmall, "la cabecera aguanta el scroll en modo pequeno");
	const unsigned nLinesSmall = BareVideoStatusLines ();
	Check (nLinesSmall == (240 - kTop) / kLineH, "el numero de filas sale del modo");
	Check (BandHasInk (nLinesSmall - 1), "la ultima fila que cabe lleva texto");
	Check (BandIsBlank (nLinesSmall) || nLinesSmall == STATUS_MAX_LINES,
	       "nada se escribe por debajo de la ultima fila");

	std::printf ("\n%s (%u fallos)\n", s_nFails == 0 ? "TODO BIEN" : "HAY FALLOS", s_nFails);

	return s_nFails == 0 ? 0 : 1;
}
