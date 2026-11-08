//
// bare_storage.h - flushing the SD card, without guessing.
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// ---------------------------------------------------------------------------
// Why this file exists, and what it replaced
// ---------------------------------------------------------------------------
//
// .59 shipped a call to POSIX sync() in CKernel::Cleanup().  It did not link:
//
//     build/src/kernel.o: in function `CKernel::Cleanup()':
//     undefined reference to `sync'
//
// which is the second time in two rounds that the same mistake was made in the
// same place: assuming what circle-stdlib provides instead of checking.  Worth
// writing down what is actually there, because the chain is short but no part
// of it is guessable:
//
//   * circle-stdlib mounts the card itself.  CStdlibAppStdio::Initialize() calls
//     f_mount (&mFileSystem, "SD:", 1) and CStdlibAppStdio::Cleanup() calls
//     f_mount (0, "", 0).  mFileSystem is *private*, FATFS, from FatFs - the
//     addon in Circle's tree, not a system interface.
//
//   * POSIX sync() is not part of that.  circle-stdlib's newlib is the upstream
//     one plus a libgloss target; neither provides sync(), and nor does FatFs
//     (FatFs has f_sync, per *file object*, and f_mount, which is what
//     CStdlibAppStdio already does on the way out).
//
//   * f_mount(0, "", 0) registers nothing and, looking at FatFs's own source
//     (f_mount: `cfs->fs_type = 0`), it does NOT flush: there is no call to
//     disk_ioctl(CTRL_SYNC) on that path.  So "the framework already does it"
//     would have been the wrong conclusion too.
//
//   * What DOES flush is CTRL_SYNC, and in Circle's addon/fatfs/diskio.cpp that
//     becomes IOCtl(DEVICE_IOCTL_SYNC) on the block device for the volume, which
//     is the SD card driver's own flush.  FatFs issues CTRL_SYNC from f_sync()
//     and f_close().
//
// So the honest sequence at shutdown is: fflush(stdout) (stdio buffers that have
// not even reached FatFs), then f_sync() on a file opened on the target volume -
// which pushes CTRL_SYNC down to the card driver.  That is what this file does.
//
// ---------------------------------------------------------------------------
// Why a separate header with a fake underneath
// ---------------------------------------------------------------------------
//
// The real implementation needs FatFs's ff.h and a mounted volume, neither of
// which exist on the host.  But the *decision* - which calls, in which order,
// what counts as success - is exactly the kind of thing that gets written wrong
// and is testable on its own.  So: one header, two backends, chosen by
// BARE_STORAGE_HOST.  The test (check-storage) compiles this file against the
// fake backend and asserts the exact call sequence, including the failure paths.
// The board build compiles it against FatFs and nothing of the test.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef BARE_STORAGE_H
#define BARE_STORAGE_H

#include <cstdio>

namespace bare_storage
{

/// \brief What the flush attempt did, for the caller to log and for the test to
///\t   check.  Not a boolean: "nothing to flush" and "the flush failed" are
///\t   different answers and a user deciding whether to cut power needs to tell
///\t   them apart.
enum class TResult
{
	Flushed,	///< the card driver accepted the flush, power can be cut
	NothingToDo,	///< no volume mounted; nothing was ever written
	Failed,		///< something was written and the flush did not happen
};

/// \brief Why, in the card's own vocabulary.
///
/// Added in .65g because the board said "FLUSH FAILED - do not cut power yet
/// (see the log)" and the log said nothing: Flush() returned Failed and threw
/// away the FRESULT that explained it.  That is a message which cannot be acted
/// on - the one message in this port that a person reads *before deciding to cut
/// the power* - so the reason now travels with the result and the caller prints
/// it.  The names are FatFs's (ff.h), not this port's invention.
enum class TWhy
{
	None,			///< nothing went wrong
	Stdio,			///< fflush(nullptr) != 0: a C stdio buffer could not be written
	NoCard,			///< FR_INVALID_DRIVE / FR_NOT_READY: no card, nothing to lose
	Open,			///< f_open() on the probe file failed; see the FRESULT logged
	Sync,			///< f_sync() failed; see the FRESULT logged
};

/// \brief The card's answer, as a number.
///
/// Deliberately `int` and not `FRESULT`: FRESULT only exists where FatFs exists,
/// and this struct is declared above the `#ifdef` that decides which backend is
/// being compiled.  A cast at the two places that fill it keeps the two builds
/// honest without teaching the header about FatFs.
/// \brief What happened, and why.  Returned by Flush() by reference to the
///\t   caller's TWhy, which keeps Flush()'s result a plain enum and keeps the
///\t   reason optional for callers that do not care (the test does).
struct TReport
{
	TResult Result;
	TWhy    Why;
	int     nFRESULT;	///< the card's FRESULT; only meaningful for Open/Sync
};

#ifdef BARE_STORAGE_HOST

// The host has no FatFs, so FRESULT and its spellings are reproduced here - as
// an enum with FatFs's own values, not as a bare int, so that a switch over it
// warns when a case is missing exactly as the board build would.  The numbers
// are ff.h's (FR_OK = 0 first, then the errors in the order ff.h lists them);
// nothing in this file depends on the exact values, only on "0 is success".
enum FRESULT
{
	FR_OK = 0,
	FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
	FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT,
	FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM,
	FR_MKFS_ABORTED, FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE,
	FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER,
};

// ---------------------------------------------------------------------------
// Host backend: records the calls the real one would make, so that the order
// and the conditions can be asserted.  Deliberately the same shape as FatFs's
// API (open returns a result, sync takes the object, close takes the object) so
// that a mistake in the real path has to be made twice to get past the test.
// ---------------------------------------------------------------------------

namespace fake
{
	struct TFile { int nHandle; };
	struct TStat
	{
		unsigned nFlushes;	///< fflush() calls
		unsigned nOpens;
		unsigned nSyncs;
		unsigned nCloses;
		bool bMountFails;
		bool bOpenFails;
		bool bSyncFails;
		char LastPath[64];	///< the path Flush() asked for, for the test to read
	};

	extern TStat Stat;

	int  Mount ();		///< 0 = FR_OK
	int  Open (TFile *pFile, const char *pPath);
	int  Sync (TFile *pFile);
	int  Close (TFile *pFile);
	void Reset ();

} // namespace fake

#else

#include <fatfs/ff.h>		// Circle's addon, via -I$(CIRCLEHOME)/addon/

#endif

/// \brief Flush everything that could still be in a buffer on its way to the
///\t   card: stdio first, then the filesystem below it.
///
/// \brief The file the flush probes with, when there is no real one open.
///
/// It has to be on the volume being flushed (that is where CTRL_SYNC goes) and
/// it has to be a name FatFs accepts.  "SD:" on its own is not: it is a volume,
/// not a file, and FatFs answers FR_INVALID_NAME - which is exactly the bug .65f
/// shipped and .65g fixed.  The name is dull on purpose: it lives in the root
/// where the port's own files are, so a person who finds it knows what it is.
static const char *const ProbePath = "SD:/boot.flush";

/// The order is the point.  fflush() pushes C stdio's buffers into FatFs;
/// f_sync() pushes FatFs's buffers (and FatFs's cached FAT sector) through
/// CTRL_SYNC into the card driver.  Doing only the second leaves the first
/// behind, and doing only the first leaves the FAT sector behind.
inline TReport Flush (void)
{
	// 1. C stdio.  Always, and its result is checked because a full buffer
	//    that cannot be written is exactly a lost file.
	const bool bStdioOk = fflush (nullptr) == 0;

#ifdef BARE_STORAGE_HOST

	// 2. The volume.  The fake reports "nothing mounted" the same way FatFs
	//    does: the mount call fails.
	if (fake::Mount () != 0)
	{
		fake::Stat.nFlushes++;		// count the fflush above, for the test
		return { bStdioOk ? TResult::NothingToDo : TResult::Failed,
			 bStdioOk ? TWhy::NoCard : TWhy::Stdio, 0 };
	}

	fake::Stat.nFlushes++;

	fake::TFile File { 0 };
	const int nOpen = fake::Open (&File, ProbePath);
	if (nOpen != 0)
	{
		return { TResult::Failed, TWhy::Open, static_cast<int> (nOpen) };
	}

	const int nSync = fake::Sync (&File);

	// 3. Close even if the sync failed: an open file left behind is one more
	//    thing between the data and the card.
	(void) fake::Close (&File);

	if (nSync != 0)
	{
		return { TResult::Failed, TWhy::Sync, static_cast<int> (nSync) };
	}

	return { bStdioOk ? TResult::Flushed : TResult::Failed,
		 bStdioOk ? TWhy::None : TWhy::Stdio, 0 };

#else

	// The volume name is the one circle-stdlib mounts (CSTDLIBAPP_DEFAULT_
	// PARTITION).  This port does not choose it; kernel.h's DOSBOX_ROOT is the
	// same string, and the two have to agree or DOSBox would not find the card
	// either.
	//
	// The volume test is f_getfree(), not f_stat(), because it is the only
	// cheap call that is guaranteed not to follow a path: f_stat("SD:") opens
	// the object the path names, and if that were ever a directory it would be
	// a directory read, not a volume check.  It also fails exactly when there is
	// no card - FR_NOT_READY / FR_INVALID_DRIVE - which is the case that must
	// not be reported as a failure to flush.
	FATFS *pFS = nullptr;
	DWORD nFreeClusters = 0;
	const FRESULT nVolume = f_getfree ("SD:", &nFreeClusters, &pFS);
	(void) nFreeClusters;

	if (nVolume == FR_INVALID_DRIVE || nVolume == FR_NOT_READY)
	{
		// No card: nothing can be lost by cutting the power, so this is not
		// a failure.  Saying "FAILED" here would teach the one user who
		// reads this message to ignore it.
		return { bStdioOk ? TResult::NothingToDo : TResult::Failed,
			 bStdioOk ? TWhy::NoCard : TWhy::Stdio,
			 static_cast<int> (nVolume) };
	}

	// FatFs flushes per *file object*: there is no volume-level flush, and
	// f_mount(0, "", 0) - which is all CStdlibAppStdio::Cleanup() does - only
	// invalidates the object (`cfs->fs_type = 0`) without touching the disk.
	// So a file has to be open for there to be something to sync.  FATFS
	// requires FF_FS_MINIMIZE == 0 for f_sync, which Circle's ffconf.h has.
	// .65g: this used to be f_open (&File, "SD:", FA_OPEN_EXISTING|FA_READ),
	// i.e. a file object on THE VOLUME ITSELF, and that cannot work.  In FatFs a
	// path of "SD:" with no file name is rejected by check_fs / follow_path
	// with FR_INVALID_NAME before the card is touched - so on the board the open
	// failed, Flush() returned Failed, and the screen said
	//
	//     FLUSH FAILED - do NOT cut power yet (see the log)
	//
	// with the card perfectly healthy and the log saying nothing.  That is the
	// screenshot taken on the board.
	//
	// The host double could not catch it because the double said "open = 0" for
	// everything: a fake that is kinder than the thing it fakes is a test that
	// reports its own optimism.  It now rejects the exact path the board
	// rejected, so this mistake has to be made twice to get past check-storage.
	//
	// The fix is the probe file below: FATFS has no volume-level flush, so a
	// *real file object* on the volume is opened instead.  FA_OPEN_ALWAYS creates
	// it when it is missing and opens it when it is there, which is what makes
	// this work on the first boot after a fresh flash and on every boot after.
	FIL File;
	const FRESULT nOpen = f_open (&File, ProbePath, FA_OPEN_ALWAYS | FA_WRITE);
	if (nOpen != FR_OK)
	{
		// No file object, no f_sync, so no flush - and the honest answer is
		// Failed, with the card's own reason.  The tempting shortcut here is
		// f_mount(nullptr, "", 0), which "unmounts" and looks like a flush; it
		// is not one (see above, and it is what CStdlibAppStdio::Cleanup()
		// does on its own).  Reporting it as success would be worse than
		// reporting nothing: this is the message a person reads before
		// cutting the power.
		return { TResult::Failed, TWhy::Open, static_cast<int> (nOpen) };
	}

	const FRESULT nSync = f_sync (&File);
	(void) f_close (&File);		// close even on failure: one fewer open file

	if (nSync != FR_OK)
	{
		return { TResult::Failed, TWhy::Sync, static_cast<int> (nSync) };
	}

	return { bStdioOk ? TResult::Flushed : TResult::Failed,
		 bStdioOk ? TWhy::None : TWhy::Stdio, 0 };

#endif
}

/// \brief The card's own name for an FRESULT, so the log can be searched
///\t   against ff.h instead of guessing which number was which.
inline const char *ResultName (FRESULT nResult)
{
	switch (nResult)
	{
	case FR_OK:                    return "FR_OK";
	case FR_DISK_ERR:              return "FR_DISK_ERR";
	case FR_INT_ERR:               return "FR_INT_ERR";
	case FR_NOT_READY:             return "FR_NOT_READY";
	case FR_NO_FILE:               return "FR_NO_FILE";
	case FR_NO_PATH:               return "FR_NO_PATH";
	case FR_INVALID_NAME:          return "FR_INVALID_NAME";
	case FR_DENIED:                return "FR_DENIED";
	case FR_EXIST:                 return "FR_EXIST";
	case FR_INVALID_OBJECT:        return "FR_INVALID_OBJECT";
	case FR_WRITE_PROTECTED:       return "FR_WRITE_PROTECTED";
	case FR_INVALID_DRIVE:         return "FR_INVALID_DRIVE";
	case FR_NOT_ENABLED:           return "FR_NOT_ENABLED";
	case FR_NO_FILESYSTEM:         return "FR_NO_FILESYSTEM";
	case FR_MKFS_ABORTED:          return "FR_MKFS_ABORTED";
	case FR_TIMEOUT:               return "FR_TIMEOUT";
	case FR_LOCKED:                return "FR_LOCKED";
	case FR_NOT_ENOUGH_CORE:       return "FR_NOT_ENOUGH_CORE";
	case FR_TOO_MANY_OPEN_FILES:   return "FR_TOO_MANY_OPEN_FILES";
	case FR_INVALID_PARAMETER:     return "FR_INVALID_PARAMETER";
	default:                       return "FR_?";
	}
}

/// \brief The card's own name for a report's FRESULT, whoever is asking.
///
/// The cast lives here and not at the call sites: FRESULT is a real type only on
/// the board, an `int` in TReport, and this is the one place that knows the two
/// are the same thing.
inline const char *ResultNameOf (const TReport &Report)
{
	return ResultName (static_cast<FRESULT> (Report.nFRESULT));
}

/// \brief One line for the log, for whichever result came back.
inline const char *Describe (TResult Result)
{
	switch (Result)
	{
	case TResult::Flushed:
		return "card flushed, power can be cut";
	case TResult::NothingToDo:
		return "no card volume to flush";
	case TResult::Failed:
	default:
		return "FLUSH FAILED - do not cut power yet";
	}
}

/// \brief The second half of that line: what to look at when it failed.
///
/// "FLUSH FAILED - do not cut power yet (see the log)" and then nothing in the
/// log is a message that costs a round trip.  This is the sentence that goes
/// under it.
inline const char *Explain (const TReport &Report)
{
	switch (Report.Why)
	{
	case TWhy::None:
		return "";
	case TWhy::Stdio:
		return "stdio: a buffer could not be written (the card is full?)";
	case TWhy::NoCard:
		return "no card volume: there was nothing to flush";
	case TWhy::Open:
		return "opening the probe file failed; the card is mounted but "
		       "not writable, or it is full";
	case TWhy::Sync:
		return "the card driver refused CTRL_SYNC: the write is still "
		       "in the filesystem cache";
	default:
		return "";
	}
}

/// \brief The whole failure, as one line for the panel.
///
/// The footer holds one sentence and no more, so this is the sentence: it says
/// what failed and what the card answered, and it does not send the reader to a
/// log they cannot open while deciding whether to cut the power.  It is built
/// once, in a static buffer, because the caller hands the pointer straight to
/// BareVideoFooter(), which does not copy it.
inline const char *FooterFailure (const TReport &Report)
{
	static char Text[96];

	snprintf (Text, sizeof Text, "FLUSH FAILED (%s) - do NOT cut power yet",
		   Report.Why == TWhy::Open || Report.Why == TWhy::Sync
			   ? ResultName (static_cast<FRESULT> (Report.nFRESULT))
			   : (Report.Why == TWhy::Stdio ? "stdio" : "no card"));

	return Text;
}

} // namespace bare_storage

#endif
