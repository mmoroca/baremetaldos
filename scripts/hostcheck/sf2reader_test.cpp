//
// sf2reader_test.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// The SoundFont block reader of src/dosbox-stub/midi_stub.cpp, run on the host.
//
// Why it exists: .79 replaced stdio's buffer with a bigger one and the
// SoundFont stopped loading on the board - the failure was in the reading path,
// which nothing in this tree could exercise without flashing a card.  This test
// runs the reader that ships (gen-sf2reader.py extracts it from midi_stub.cpp,
// so it cannot drift) against files written by the test itself, and it does not
// care what a SoundFont is: what is being checked is that the bytes come out
// in order, once, from the right place.
//
//   [1] every size that sits on a boundary of the 64 KB block (0, 1, block-1,
//       block, block+1) and beyond, read in pieces from 1 byte to 100 KB -
//       including the pathological 1-byte-at-a-time case, which is what tsf
//       does for the pdta chunk;
//   [2] skips, which is how tsf passes over the chunks it does not want, and
//       the case where a skip crosses the block boundary;
//   [3] one huge read, which is exactly what tsf does with the sample chunk;
//   [4] a file shorter than it claims: the read must stop and SAY where,
//       instead of handing back whatever was in the buffer;
//   [5] the extra hold on the card: a read that comes back EIO once - which
//       is what the .79c boot did at 15168 KB - must be retried in place and
//       must not cost the whole file again, while a card that keeps failing
//       must be reported once and not once per attempt;
//   [6] the RAM guard in OpenStage(): a SoundFont of any size can land on the
//       card, and on this platform an exhausted heap panics instead of
//       returning NULL, so the refusal has to happen before tsf allocates.
//       A guard that refuses too much is its own kind of bug, so the case
//       where Circle does not answer (0) must still try.
//
// It does not test what is below stdio - the SD card, FatFs, DMA.  That is the
// part only the board can answer, and the reason the failure is now reported
// with an offset and an errno.
//
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <ctime>

typedef unsigned long u64;
typedef unsigned long Bitu;

// The only things the reader asks of the port.
static u64 s_nFakeHeapFree = 0;		// what BareSysInfoHeapFree() will answer
static unsigned s_nLogs = 0;
static void BareLog (int, const char *pText)
{
	s_nLogs++;
	std::printf ("       [log] %s\n", pText);
}
static u64 BareSysInfoHeapFree (void) { return s_nFakeHeapFree; }
static u64 BareMicros (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (u64) ts.tv_sec * 1000000ull + (u64) ts.tv_nsec / 1000;
}

#include "sf2reader.inc"

static const char *kPath = "build/sf2reader.elf.bin";
static unsigned char *g_pFile = nullptr;

static unsigned s_nFails = 0;
static void Check (bool bOk, const char *pWhat)
{
	std::printf ("  %s  %s\n", bOk ? "ok  " : "FAIL", pWhat);
	if (!bOk) s_nFails++;
}

static unsigned char ByteAt (long nPos)
{
	return static_cast<unsigned char> ((nPos * 7 + 3) & 0xFF);
}

static void MakeFile (long nSize)
{
	FILE *pFile = std::fopen (kPath, "wb");
	if (pFile == nullptr)
	{
		std::printf ("no puedo escribir %s\n", kPath);
		std::exit (99);
	}

	g_pFile = static_cast<unsigned char *> (std::malloc (nSize ? nSize : 1));
	for (long i = 0; i < nSize; i++)
	{
		g_pFile[i] = ByteAt (i);
	}
	if (nSize)
	{
		std::fwrite (g_pFile, 1, nSize, pFile);
	}
	std::fclose (pFile);
}

static bool Open (long nSize)
{
	s_pStageFile = std::fopen (kPath, "rb");
	if (s_pStageFile == nullptr)
	{
		return false;
	}
	s_nStageSize        = nSize;
	s_nStageCursor      = 0;
	s_nWindowStart      = 0;
	s_nWindowLen        = 0;
	s_nStreamBytes      = 0;
	s_nStreamReadMicros = 0;
	s_bReadFailed       = false;
	s_bRetryReported    = false;
	return true;
}

static void Close (void)
{
	if (s_pStageFile != nullptr)
	{
		std::fclose (s_pStageFile);
		s_pStageFile = nullptr;
	}
}

static void DropFile (void)
{
	std::free (g_pFile);
	g_pFile = nullptr;
}

static void SequentialCase (long nSize, unsigned nReadSize)
{
	MakeFile (nSize);
	Open (nSize);

	bool bOk = true;
	long nPos = 0;
	unsigned char *pOut = static_cast<unsigned char *> (std::malloc (nReadSize));

	while (nPos < nSize)
	{
		unsigned nWant = nReadSize;
		if (static_cast<long> (nWant) > nSize - nPos)
		{
			nWant = static_cast<unsigned> (nSize - nPos);
		}

		const int nGot = StageRead (nullptr, pOut, nWant);
		if (nGot != static_cast<int> (nWant)
		    || std::memcmp (pOut, g_pFile + nPos, nWant) != 0)
		{
			bOk = false;
			break;
		}
		nPos += nWant;
	}

	// One read past the end: empty, not a hang and not garbage.
	const int nAtEnd = StageRead (nullptr, pOut, 16);

	Close ();
	std::free (pOut);
	DropFile ();

	char What[160];
	std::snprintf (What, sizeof What, "%ld B leidos de %u en %u", nSize, nReadSize,
		       nReadSize);
	Check (bOk && nAtEnd == 0, What);
}

// --- [5] Un stream que falla a voluntad -------------------------------------
//
// glibc deja envolver un FILE* alrededor de funciones propias (fopencookie), que
// es la unica forma de reproducir en el host lo que hizo la tarjeta: -1 con
// errno = EIO en mitad de un bloque, y despues seguir leyendo bien.
#ifdef __GLIBC__
#include <unistd.h>
#include <sys/types.h>

struct TFaultCookie
{
	const unsigned char *pData;
	long nSize;
	long nPos;
	long nFailAt;		// donde contesta EIO (si nFailsLeft > 0)
	int  nFailsLeft;
	unsigned nCalls;
};

static ssize_t FaultRead (void *pCookie, char *pBuf, size_t nSize)
{
	TFaultCookie *p = static_cast<TFaultCookie *> (pCookie);
	p->nCalls++;

	if (p->nFailsLeft > 0 && p->nPos == p->nFailAt)
	{
		p->nFailsLeft--;
		errno = EIO;
		return -1;
	}

	if (p->nPos >= p->nSize)
	{
		return 0;
	}

	size_t nTake = nSize;
	if (p->nPos + static_cast<long> (nTake) > p->nSize)
	{
		nTake = static_cast<size_t> (p->nSize - p->nPos);
	}
	std::memcpy (pBuf, p->pData + p->nPos, nTake);
	p->nPos += static_cast<long> (nTake);
	return static_cast<ssize_t> (nTake);
}

static int FaultSeek (void *pCookie, off64_t *pOff, int nWhence)
{
	TFaultCookie *p = static_cast<TFaultCookie *> (pCookie);
	long nNew = 0;

	switch (nWhence)
	{
	case SEEK_SET: nNew = static_cast<long> (*pOff); break;
	case SEEK_CUR: nNew = p->nPos + static_cast<long> (*pOff); break;
	case SEEK_END: nNew = p->nSize + static_cast<long> (*pOff); break;
	default: errno = EINVAL; return -1;
	}

	if (nNew < 0)
	{
		errno = EINVAL;
		return -1;
	}

	p->nPos = nNew;
	*pOff   = nNew;
	return 0;
}

static int FaultClose (void * /*pCookie*/) { return 0; }

static FILE *MakeFaultStream (TFaultCookie &Cookie, long nSize, long nFailAt,
			      int nFailsLeft)
{
	Cookie.pData      = g_pFile;
	Cookie.nSize      = nSize;
	Cookie.nPos       = 0;
	Cookie.nFailAt    = nFailAt;
	Cookie.nFailsLeft = nFailsLeft;
	Cookie.nCalls     = 0;

	cookie_io_functions_t Funcs = {};
	Funcs.read  = FaultRead;
	Funcs.seek  = FaultSeek;
	Funcs.close = FaultClose;

	return fopencookie (&Cookie, "rb", Funcs);
}
#endif

int main (void)
{
	std::printf ("[1] entrega secuencial: el mismo fichero, trozo a trozo\n");
	const long kSizes[] = { 1, 100, 65535, 65536, 65537, 200000, 1000003 };
	const unsigned kReads[] = { 1, 3, 7, 4096, 65536, 100000 };
	for (long nSize : kSizes)
	{
		for (unsigned nRead : kReads)
		{
			if (nSize < 100 && nRead > 4096)
			{
				continue;	// ruido: no aporta y multiplica la salida
			}
			SequentialCase (nSize, nRead);
		}
	}

	std::printf ("\n[2] saltos: asi pasa tsf por encima de los chunks que no quiere\n");
	MakeFile (300000);
	Open (300000);
	{
		unsigned char a[10], b[10];
		bool bOk = true;
		bOk = bOk && StageRead (nullptr, a, 10) == 10
			  && std::memcmp (a, g_pFile, 10) == 0;
		bOk = bOk && StageSkip (nullptr, 100000) == 1;
		bOk = bOk && StageRead (nullptr, b, 10) == 10
			  && std::memcmp (b, g_pFile + 100010, 10) == 0;
		bOk = bOk && StageSkip (nullptr, 63 * 1024) == 1;	// cruza la ventana
		bOk = bOk && StageRead (nullptr, b, 10) == 10
			  && std::memcmp (b, g_pFile + 100010 + 10 + 63 * 1024, 10) == 0;
		Check (bOk, "leer, saltar 100 KB, leer, saltar 63 KB, leer: cada byte en su sitio");
		Check (StageSkip (nullptr, 1000000) == 0, "saltar mas alla del final devuelve 0");
	}
	Close ();
	DropFile ();

	std::printf ("\n[3] una lectura enorme de una vez (el chunk de muestras)\n");
	MakeFile (500000);
	Open (500000);
	{
		unsigned char *pOut = static_cast<unsigned char *> (std::malloc (500000));
		const int nGot = StageRead (nullptr, pOut, 500000);
		Check (nGot == 500000, "500 000 B en una sola llamada");
		Check (std::memcmp (pOut, g_pFile, 500000) == 0, "y el contenido es identico");
		std::free (pOut);
	}
	Close ();
	DropFile ();

	std::printf ("\n[4] un fichero mas corto de lo que dice: se para y lo cuenta\n");
	MakeFile (200000);
	Open (200000 + kReadBlock);		// mentira a proposito
	{
		unsigned char *pOut = static_cast<unsigned char *> (std::malloc (300000));
		s_nLogs = 0;
		const int nGot = StageRead (nullptr, pOut, 300000);
		Check (nGot < 300000, "la lectura se corta en vez de devolver basura");
		Check (s_nLogs == 1, "y deja UNA linea de log diciendo donde se paro");
		// El flag de error de stdio deja la siguiente lectura fallando en el
		// mismo sitio: eso no es un segundo acontecimiento.
		StageRead (nullptr, pOut, 1000);
		Check (s_nLogs == 1, "y una segunda lectura no lo cuenta otra vez");
		std::free (pOut);
	}
	Close ();
	DropFile ();

	std::printf ("\n[5] un corte de la tarjeta: se reintenta el bloque, no el fichero\n");
#ifdef __GLIBC__
	MakeFile (300000);
	{
		// Un fichero de verdad no falla a voluntad, asi que el stream es un
		// "cookie": devuelve lo mismo que el fichero, pero la primera lectura
		// que caiga en nFailAt contesta -1/EIO, que es lo que devolvio la
		// tarjeta en el arranque del .79c.
		static TFaultCookie Cookie;
		unsigned char *pOut = static_cast<unsigned char *> (std::malloc (300000));

		FILE *pFault = MakeFaultStream (Cookie, 300000, 0, 1);
		Check (pFault != nullptr, "el stream que falla una vez se abre");
		s_pStageFile   = pFault;
		s_nStageSize   = 300000;
		s_nStageCursor = 0;
		s_nWindowStart = 0;
		s_nWindowLen   = 0;
		s_bReadFailed  = false;
		s_bRetryReported = false;
		s_nStageRetries = 0;
		s_nLogs = 0;
		const int nGot = StageRead (nullptr, pOut, 300000);
		Check (nGot == 300000, "el bloque se reintenta y el fichero se lee entero");
		Check (std::memcmp (pOut, g_pFile, 300000) == 0, "y los bytes son los mismos");
		Check (s_nLogs == 1, "y avisa del reintento UNA vez");
		Check (s_nStageRetries == 1, "y lo cuenta para la linea de arranque");

		// La tarjeta que no se recupera: se rinde, y lo dice una sola vez
		// aunque se le vuelva a pedir.
		Close ();
		pFault = MakeFaultStream (Cookie, 300000, 0, 1 << 20);
		Check (pFault != nullptr, "y otro que falla siempre tambien");
		s_pStageFile   = pFault;
		s_nStageSize   = 300000;
		s_nStageCursor = 0;
		s_nWindowStart = 0;
		s_nWindowLen   = 0;
		s_bReadFailed  = false;
		s_bRetryReported = false;
		s_nStageRetries = 0;
		s_nLogs = 0;
		Check (StageRead (nullptr, pOut, 1000) == 0, "con la tarjeta muerta no devuelve nada");
		Check (s_nLogs == 1, "y lo dice UNA vez, no una por intento");
		StageRead (nullptr, pOut, 1000);
		Check (s_nLogs == 1, "y no lo repite al volver a pedirle");
		Close ();
		std::free (pOut);
	}
	DropFile ();
#else
	std::printf ("  (fopencookie no esta en esta plataforma: caso saltado)\n");
#endif

	std::printf ("\n[6] el guardarrail de RAM: rechaza lo que no cabe, no lo adivina\n");
	MakeFile (200 * 1024);
	{
		// 200 KB de fichero -> pide 500 KB.  Con 1 GB libre, adelante.
		s_nFakeHeapFree = 1024ull * 1024 * 1024;
		Check (OpenStage (kPath), "con RAM de sobra, se abre");
		CloseStage ();

		// Con 100 B libres tiene que negarse, y decirlo UNA vez.
		s_nFakeHeapFree = 100;
		s_nLogs = 0;
		Check (!OpenStage (kPath), "sin RAM, se niega a cargarla");
		Check (s_nLogs == 1, "y lo dice una vez");
		Check (s_pStageFile == nullptr, "y no deja el fichero abierto");

		// 0 = Circle no contesta: se intenta igual, no se rechaza por ignorancia.
		s_nFakeHeapFree = 0;
		Check (OpenStage (kPath), "si la RAM es desconocida, se intenta");
		CloseStage ();
	}
	DropFile ();

	std::remove (kPath);

	std::printf ("\n%s (%u fallos)\n", s_nFails == 0 ? "TODO BIEN" : "HAY FALLOS",
		     s_nFails);
	return s_nFails == 0 ? 0 : 1;
}
