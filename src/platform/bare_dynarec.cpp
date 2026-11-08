//
// bare_dynarec.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - everything the DOSBox dynamic recompiler needs from the machine,
// which bare metal does not provide by itself.
//
// The engine is DOSBox-core's own (src/cpu/core_dynrec.cpp).  It compiles for
// AArch64 - 485 KB of object code, 49k instructions, eighteen `br` - and it
// asks the environment for exactly two things:
//
//   1. a writable buffer it will then execute from.  dyn_cache.h malloc()s it
//      and, if C_HAVE_MPROTECT, calls mprotect() on it - which newlib here does
//      not implement, so the buffer stays as the allocator left it;
//   2. the guarantee that when it closes a block, the instruction cache sees
//      the new code.  cache_block_closing() -> cache_flush() in
//      risc_armv8le.h does that correctly already (dc civac / ic ivau /
//      dsb ish / isb).
//
// (2) works only if the memory holding the cache is *inner-shareable normal
// memory*, and (1) works only if that memory is executable.  Both depend on the
// page tables, and this is the thing bare metal does not give us for free:
// Circle maps the region above _etext with PXN = 1 ("privileged execute never",
// lib/translationtable64.cpp:135) because it assumes everything after the
// kernel's text is data.  A malloc'd code cache lands exactly there, so the
// first translated block would fault.  On x86 nobody notices; on AArch64 it is
// a synchronous exception at the first block.
//
// So this file maps PXN = 0 for the pages that hold the cache, and only those.
//
// How the mapping is reached - the answer is not "call a Circle API", because
// there is none (CTranslationTable exposes GetBaseAddress() and nothing else,
// and there is no Map()/Unmap()):
//
//   * Circle's MMU is 64 KB granule (TCR_EL1.TG0), TTBR0, and the walk has
//     *two* levels in use, not three.  That is worth being explicit about,
//     because the first version of this file assumed three and was wrong:
//     lib/translationtable64.cpp builds LEVEL2_TABLE_ENTRIES (128 on a Pi 4)
//     table descriptors of 512 MB each, and each of those points straight at
//     the last level, 8192 descriptors of 64 KB.  So the first table covers
//     64 GB in 128 entries and there is no middle level at all.
//   * Only the last level decides PXN for our address.
//   * TTBR0_EL1 holds the table base and TCR_EL1.EPD0 is clear (translation
//     enabled), both set by CMemorySystem in lib/memory64.cpp.
//   * The walk below is read-only until the very last descriptor, and every
//     step checks the descriptor type before using it.  Anything that does not
//     look the way Circle's own code writes it gives up with a log line
//     instead of poking at whatever it found.
//
// What this does NOT do: touch SCTLR_EL1.WXN.  Circle already clears it
// (lib/memory64.cpp:220: `nSCTLR_EL1 &= ~(SCTLR_EL1_WXN | SCTLR_EL1_A)`), so
// writable pages may be executed; the per-page PXN bit is what was in the way.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"

#include <circle/logger.h>
#include <circle/types.h>

static const char FromDynarec[] = "dynarec";

#if AARCH == 64

namespace {

constexpr u64 PageSize = 0x10000;		// ARMV8MMU_LEVEL3_PAGE_SIZE

// Descriptor type, bits [1:0].  3 means "table" at levels 1 and 2 and "page"
// at level 3, which is why the checks below say which one they expect at each
// level instead of just following whatever address is there.
constexpr u64 DescTypeMask = 0x3;
constexpr u64 DescTypeTable = 0x3;
constexpr u64 DescTypePage = 0x3;

// PXN is bit 53 of the last-level descriptor.  Written as a raw shift and not
// through Circle's bitfield: the walk reads plain u64 words out of live page
// tables, and must not depend on a struct layout that could change.
constexpr u64 BitPXN = 1ULL << 53;
constexpr u64 BitAF = 1ULL << 10;

// Circle writes descriptor addresses as [47:16]; this mask is what
// ARMV8MMUL3PAGEADDR() keeps, widened to a pointer.
constexpr u64 DescAddressMask = 0x0000FFFFFFFF0000ULL;

u64 ReadTtbr0 (void)
{
	u64 nValue;
	asm volatile ("mrs %0, ttbr0_el1" : "=r" (nValue));
	return nValue;
}

void SyncInstructionStream (void)
{
	// The TLB has to forget the old permission and the pipeline has to see the
	// new one before any translated block runs.  tlbi vmalle1 is more than a
	// handful of pages needs, but this happens once, at boot, and a wrong
	// local/global distinction here would be a fault much later.
	asm volatile ("dsb ish" : : : "memory");
	asm volatile ("tlbi vmalle1" : : : "memory");
	asm volatile ("dsb ish" : : : "memory");
	asm volatile ("isb" : : : "memory");
}

/// \brief Clear PXN on every 64 KB page overlapping [nStart, nEnd).
/// \return Pages changed, or -1 if the walk gave up.
int ClearPXNForRange (u64 nStart, u64 nEnd)
{
	const u64 nTtbr0 = ReadTtbr0 () & ~0xFFFFULL;	// low bits are ASID/flags

	// First level: one table descriptor per 512 MB (ARMV8MMU_LEVEL2_BLOCK_SIZE,
	// i.e. 8192 pages of 64 KB), covering 64 GB in 128 entries on a Pi 4.
	// A block descriptor here would mean Circle mapped half a gigabyte as one
	// page, which it does not - so that is "not the layout this code was
	// written against" and the honest answer is to stop.
	const u64 nL1Index = (nStart >> 29) & 0x1FF;
	const u64 nL1Entry = *reinterpret_cast<const volatile u64 *> (
				nTtbr0 + nL1Index * sizeof (u64));

	if ((nL1Entry & DescTypeMask) != DescTypeTable)
	{
		CLogger::Get ()->Write (FromDynarec, LogWarning,
			"L1 entry %llu is not a table descriptor (0x%llx)",
			(unsigned long long) nL1Index, (unsigned long long) nL1Entry);
		return -1;
	}

	// Straight to the last level: Circle's first table points at the page
	// table, there is no middle level (see the header).
	const u64 nL3Base = nL1Entry & DescAddressMask;

	int nChanged = 0;
	boolean bStopped = FALSE;		// recorrimos parte del rango, no todo
	u64 nAddress = nStart & ~(PageSize - 1);

	// The L1 entry the loop is currently walking through.  It starts as the one
	// that holds nStart and moves on when the range crosses a 512 MB boundary.
	//
	// The first version of this function *refused* to cross one, on the
	// reasoning that a second walk might be subtly different.  That is the
	// wrong trade for an 8 MB cache: allocations that big are exactly the ones
	// likely to start near the top of a 512 MB block, and a refusal there is a
	// JIT that cannot run on that boot.  So the walk repeats the lookup, with
	// the same checks, and only for as many blocks as the range really spans.
	u64 nCurrentL1 = nL1Index;
	u64 nL3 = nL3Base;

	while (nAddress < nEnd)
	{
		const u64 nThisL1 = (nAddress >> 29) & 0x1FF;
		if (nThisL1 != nCurrentL1)
		{
			const u64 nEntry = *reinterpret_cast<const volatile u64 *> (
					nTtbr0 + nThisL1 * sizeof (u64));
			if ((nEntry & DescTypeMask) != DescTypeTable)
			{
				CLogger::Get ()->Write (FromDynarec, LogWarning,
					"L1 entry %llu is not a table descriptor (0x%llx)",
					(unsigned long long) nThisL1,
					(unsigned long long) nEntry);
				bStopped = TRUE;
				break;
			}

			nCurrentL1 = nThisL1;
			nL3 = nEntry & DescAddressMask;
		}

		const u64 nL3Index = (nAddress >> 16) & 0x1FFF;	// 8192 entries
		volatile u64 *pDesc = reinterpret_cast<volatile u64 *> (
					nL3 + nL3Index * sizeof (u64));

		if ((*pDesc & DescTypeMask) != DescTypePage)
		{
			CLogger::Get ()->Write (FromDynarec, LogWarning,
				"L3 entry for 0x%llx is not a page descriptor (0x%llx)",
				(unsigned long long) nAddress,
				(unsigned long long) *pDesc);
			bStopped = TRUE;
			break;
		}

		if ((*pDesc & BitAF) == 0)
		{
			// No access flag: the first access would fault in a way that
			// looks like a permission problem.  Circle sets AF on
			// everything it maps, so this also means the tables are not
			// what this code thinks they are.
			CLogger::Get ()->Write (FromDynarec, LogWarning,
				"L3 entry for 0x%llx has AF clear; leaving it alone",
				(unsigned long long) nAddress);
			bStopped = TRUE;
			break;
		}

		if ((*pDesc & BitPXN) != 0)
		{
			// Clear PXN.  UXN is left as it is: nothing runs this from
			// user mode, and changing a bit nobody asked about is how a
			// fix becomes the next bug.
			*pDesc = *pDesc & ~BitPXN;
			nChanged++;
		}

		nAddress += PageSize;
	}

	// Anything other than "walked the whole range" is a failure, *including* a
	// walk that changed a lot of pages before it stopped.  The caller turns a
	// negative answer into "do not start the JIT"; half a cache with execute
	// permission is the case that must never reach it.  This used to read
	// `nChanged > 0 ? nChanged : ...`, which returned 64 for a range whose
	// second half had been refused - the number was right about what it did and
	// silent about what it did not.
	if (bStopped)
	{
		return -1;
	}

	return nChanged;
}

} // namespace

#endif // AARCH == 64

boolean BareDynarecPrepareCodeCache (void *pCache, unsigned nBytes)
{
#if AARCH == 64
	if (pCache == nullptr || nBytes == 0)
	{
		return FALSE;
	}

	const u64 nStart = reinterpret_cast<u64> (pCache);
	const u64 nEnd = nStart + nBytes;

	const int nChanged = ClearPXNForRange (nStart, nEnd);

	if (nChanged < 0)
	{
		CLogger::Get ()->Write (FromDynarec, LogError,
			"cannot make %u KB at 0x%llx executable: the dynarec would "
			"fault on its first block",
			nBytes / 1024, (unsigned long long) nStart);
		return FALSE;
	}

	SyncInstructionStream ();

	CLogger::Get ()->Write (FromDynarec, LogNotice,
		"code cache %u KB at 0x%llx: %d pages made executable",
		nBytes / 1024, (unsigned long long) nStart, nChanged);

	return TRUE;
#else
	(void) pCache;
	(void) nBytes;

	// 32-bit ARM has no PXN, so there is nothing to undo.
	return TRUE;
#endif
}
