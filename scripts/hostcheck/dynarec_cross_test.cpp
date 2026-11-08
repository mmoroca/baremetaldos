// dynarec_cross_test.cpp - check-dynarec, part two: crossing a 512 MB boundary
//
// The JIT's code cache is a single 8 MB malloc().  An allocation that size is
// exactly the kind that starts near the top of a 512 MB block, and the first
// version of ClearPXNForRange() *refused* to cross a block boundary - it logged
// and stopped.  On a machine where the allocator happened to put the cache
// across a boundary, that would have been a JIT that could not run, on that
// boot, for a reason nobody could see from the outside.
//
// So the walk now repeats the first-level lookup when the range crosses, and
// this test is the one that matters for it: two adjacent 512 MB blocks, a range
// that starts in the first and ends in the second, and the same guarantees as
// before - every page in the range loses PXN, no page outside it changes, and
// the second block's table is reached through *its own* entry.
//
// The block below is extracted verbatim from src/platform/bare_dynarec.cpp by
// gen-dynarec.py (same mechanism as the other tests: a copy would drift, and a
// page-table walk that drifts corrupts the machine in silence).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

typedef uint64_t u64;
typedef uint32_t u32;

// Circle's spelling, which the port uses inside this file.
typedef bool boolean;
#define TRUE  true
#define FALSE false

// --- what the extracted block expects from its file -----------------------
static constexpr u64 PageSize = 0x10000;
static constexpr u64 DescTypeMask = 0x3;
static constexpr u64 DescTypeTable = 0x3;
static constexpr u64 DescTypePage = 0x3;
static constexpr u64 BitPXN = 1ULL << 53;
static constexpr u64 BitAF = 1ULL << 10;
static constexpr u64 DescAddressMask = 0x0000FFFFFFFF0000ULL;

static const char FromDynarec[] = "dynarec";
enum { LogWarning = 3, LogError = 4, LogNotice = 2 };

static unsigned s_nRefusals = 0;

class CLogger
{
public:
	static CLogger *Get (void) { static CLogger Logger; return &Logger; }
	void Write (const char *, int, const char *, ...) { s_nRefusals++; }
};

// Answered from the fake tables, which are built just below.
static u64 s_nTableBase = 0;
static u64 ReadTtbr0 (void) { return s_nTableBase; }

#include "dynarec_cross.inc"

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

// ---------------------------------------------------------------------------
// A page table in the shape Circle builds: 128 first-level descriptors of
// 512 MB, each pointing at a table of 8192 64 KB pages.  Only two blocks are
// populated; the rest are left as invalid descriptors so that a walk that
// strays into them is caught rather than followed.
// ---------------------------------------------------------------------------

static const unsigned L1Entries = 128;
static const unsigned L3Entries = 8192;
static constexpr u64 L1Span = 512ULL * 1024 * 1024;

// Circle allocates each level-3 table separately - 8192 descriptors, 64 KB, page
// aligned - and keeps only bits [47:16] of its address in the descriptor.  A
// fake that puts both tables in one struct does not survive that mask: the
// offset inside the struct is below bit 16, so it is dropped and the walk writes
// through the base.  (That is what the second draft of this test did, and it
// looked like eight failures in the walk.)
//
// The walk also assumes the address it is given sits at its true place in the
// 512 MB-block grid - that is what `(address >> 29) & 0x1FF` means, and a plain
// static in a host process does not satisfy it.  So the level-1 table is
// allocated at a 512 MB boundary and every address is built from it.  512 MB and
// not 1 GB: a 1 GB alignment needs a gigabyte of free address space at a 1 GB
// boundary, which a sandbox and ASan both refuse.
static constexpr u64 TableAlign = 512ULL * 1024 * 1024;
static constexpr u64 PageAlign = 0x10000;

static u64 *s_pL1 = nullptr;
static u64 *s_pL3[2] = { nullptr, nullptr };

/// \brief An address inside fake block \p nBlock, \p nOffset bytes into it.
static u64 BlockAddress (unsigned nBlock, u64 nOffset)
{
	return s_nTableBase + nBlock * L1Span + nOffset;
}

static void AllocateTable (void)
{
	if (s_pL1 != nullptr)
	{
		return;
	}

	if (posix_memalign (reinterpret_cast<void **> (&s_pL1), TableAlign,
			    L1Entries * sizeof (u64)) != 0 || s_pL1 == nullptr)
	{
		printf ("  OMITIDO  no hay sitio para alinear la tabla de nivel 1\n");
		exit (77);	// check-dynarec lo cuenta como omision, no como exito
	}

	for (unsigned nBlock = 0; nBlock < 2; nBlock++)
	{
		if (posix_memalign (reinterpret_cast<void **> (&s_pL3[nBlock]), PageAlign,
				    L3Entries * sizeof (u64)) != 0
		    || s_pL3[nBlock] == nullptr)
		{
			printf ("  OMITIDO  no hay sitio para las tablas de nivel 3\n");
			exit (77);
		}
	}
}

// First-level index the tables actually fall on.  The walk computes it from the
// address it is given, so the blocks live at whatever index the aligned
// allocation lands on - which is also the point: the test must not assume the
// table starts at index 0.
static unsigned s_nFirstL1Index = 0;

static void BuildTable (void)
{
	AllocateTable ();

	memset (s_pL1, 0, L1Entries * sizeof (u64));

	s_nTableBase = reinterpret_cast<u64> (s_pL1);
	s_nFirstL1Index = static_cast<unsigned> ((s_nTableBase >> 29) & 0x1FF);

	for (unsigned nBlock = 0; nBlock < 2; nBlock++)
	{
		memset (s_pL3[nBlock], 0, L3Entries * sizeof (u64));

		// Bits [47:16] hold the address; low bits are the descriptor type.
		s_pL1[s_nFirstL1Index + nBlock] =
			(reinterpret_cast<u64> (s_pL3[nBlock])
			 & 0x0000FFFFFFFF0000ULL) | 0x3;

		for (unsigned nPage = 0; nPage < L3Entries; nPage++)
		{
			// AF set (as Circle does), PXN set (as Circle does above
			// _etext), everything else plausible.
			s_pL3[nBlock][nPage] = 0x3 | (1ULL << 10) | (1ULL << 53);
		}
	}
}

void SyncInstructionStream (void)
{
	// No pipeline to synchronise on the host.
}

static int PXNCount (unsigned nBlock, unsigned nFirst, unsigned nLast)
{
	int n = 0;
	for (unsigned nPage = nFirst; nPage <= nLast; nPage++)
	{
		if ((s_pL3[nBlock][nPage] & (1ULL << 53)) != 0)
		{
			n++;
		}
	}
	return n;
}

int main (void)
{
	printf ("check-dynarec (2): la cache que cruza un limite de 512 MB\n\n");

	// --- 1  a range that ends exactly on a boundary --------------------
	{
		BuildTable ();

		// 16 pages (1 MB) at the very end of block 0.
		const u64 nStart = BlockAddress (0, (L3Entries - 16) * 0x10000);
		const u64 nEnd = BlockAddress (0, L3Entries * 0x10000);

		printf ("[1] un rango que termina justo en el limite\n");
		Expect (ClearPXNForRange (nStart, nEnd) == 16, "16 paginas cambiadas");
		Expect (PXNCount (0, L3Entries - 16, L3Entries - 1) == 0,
			"las 16 del final pierden PXN");
		Expect (PXNCount (0, 0, L3Entries - 17) == L3Entries - 16,
			"las de antes se quedan como estaban");
	}

	// --- 2  the case that used to refuse: across the boundary ----------
	{
		BuildTable ();

		// 8 MB starting 4 MB below the boundary: half in each block, which is
		// exactly what an unlucky malloc() produces.
		const u64 nStart = BlockAddress (0, L3Entries * 0x10000 - (4ULL * 1024 * 1024));
		const u64 nEnd = nStart + (8ULL * 1024 * 1024);

		printf ("\n[2] 8 MB a caballo del limite (lo que antes se rendia)\n");
		const int nChanged = ClearPXNForRange (nStart, nEnd);

		Expect (nChanged == 128, "128 paginas cambiadas (8 MB / 64 KB)");
		Expect (PXNCount (0, L3Entries - 64, L3Entries - 1) == 0,
			"las 64 del primer bloque pierden PXN");
		Expect (PXNCount (1, 0, 63) == 0,
			"y las 64 del segundo tambien (tabla distinta)");
		Expect (PXNCount (1, 64, L3Entries - 1) == L3Entries - 64,
			"el resto del segundo bloque no se toca");
		Expect (PXNCount (0, 0, L3Entries - 65) == L3Entries - 64,
			"el resto del primero tampoco");
	}

	// --- 3  the second block reached only through its own L1 entry -----
	{
		BuildTable ();

		// 64 KB inside block 1, i.e. one page.  If the walk reused the first
		// block's table - the bug this test exists for - it would clear PXN
		// on a page in block 0 instead, and block 1 would be untouched.
		const u64 nStart = BlockAddress (1, 5 * 0x10000);
		const int nChanged = ClearPXNForRange (nStart, nStart + 0x10000);

		printf ("\n[3] el segundo bloque se alcanza por su propia entrada\n");
		Expect (nChanged == 1, "una pagina cambiada");
		Expect ((s_pL3[1][5] & (1ULL << 53)) == 0, "la correcta");
		Expect ((s_pL3[0][5] & (1ULL << 53)) != 0,
			"la misma pagina del bloque 0 sigue protegida");
	}

	// --- 4  a broken second table is refused, not followed -------------
	{
		BuildTable ();

		// Make block 1's first-level descriptor point at nothing.
		s_pL1[s_nFirstL1Index + 1] = 0;

		const u64 nStart = BlockAddress (0, L3Entries * 0x10000 - (4ULL * 1024 * 1024));
		const u64 nEnd = nStart + (8ULL * 1024 * 1024);

		printf ("\n[4] si el segundo bloque no es una tabla, se para\n");
		Expect (ClearPXNForRange (nStart, nEnd) < 0, "devuelve -1");
		Expect (PXNCount (0, L3Entries - 64, L3Entries - 1) == 0,
			"lo que si pudo recorrer, lo cambio");
	}

	printf ("\n%d fallos\n", (int) s_nFailures);

	return s_nFailures != 0 ? 1 : 0;
}
