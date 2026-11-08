//
// bare_fs.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - file system glue.
//
// DOSBox-core is full of std::filesystem calls (it needs them for its overlay
// drive, its save directories and for enumerating a game directory).  On a
// desktop that is libstdc++/libc++ over POSIX.  Bare metal, the implementation
// comes from libc++ built by circle-stdlib on top of newlib and Circle's FatFs
// (see circle-stdlib's README: std::filesystem is one of the reasons to build
// with --libcxx).  So there is nothing to implement here - this file only
// provides the two mount points the rest of the port refers to, in one place
// so that "where do files live" is answered once.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <filesystem>

// Circle's FatFs glue mounts a partition by name ("SD:", "USB:", "emmc1-1").
// The stdio path used by DOSBox is relative to the mounted root, so "/" is
// the first FAT32 partition of the SD card.  These constants exist so the
// kernel and the core agree on the layout:
//
//   /dosbox/dosbox.conf     configuration (optional)
//   /dosbox/saves/          overlay drive + saved games
//   /dosbox/system/         soundfonts and anything else the core asks for
//   /dos/                   C: by default: your games, one directory each
//
const char *const kConfigPath = "/dosbox/dosbox.conf";
const char *const kSavesPath  = "/dosbox/saves";
const char *const kGamesPath  = "/dos";

// ---------------------------------------------------------------------------
// Path probes
// ---------------------------------------------------------------------------
//
// Why these exist: retro_load_game() does
//
//     std::filesystem::canonical(std::filesystem::path(game->path))
//
// inside a try/catch and returns false on a filesystem_error, with the reason
// only in a log message.  When that happens the screen says "cannot load" and
// says nothing about why.  These two functions answer the two questions that
// separate the possible causes: can the file be opened at all (is FatFs and the
// path right), and does canonical() agree (is the C++ filesystem layer working).
//
// The two are different layers: fopen() goes through newlib's stdio onto Circle's
// FatFs; canonical() goes through libc++'s std::filesystem.  If fopen() succeeds
// and canonical() throws, the problem is the filesystem layer, not the card.

namespace {

char s_ProbeBuffer[128];

const char *Basename (const char *pPath)
{
	const char *pSlash = strrchr (pPath, '/');

	return pSlash != nullptr ? pSlash + 1 : pPath;
}

} // namespace

const char *BareProbeFile (const char *pPath)
{
	if (pPath == nullptr)
	{
		return "(null)";
	}

	errno = 0;
	FILE *pFile = fopen (pPath, "rb");
	const int nOpenErrno = errno;
	if (pFile != nullptr)
	{
		fclose (pFile);
	}

	std::error_code Error;
	const auto Canonical = std::filesystem::canonical (std::filesystem::path (pPath), Error);

	if (pFile != nullptr && !Error)
	{
		snprintf (s_ProbeBuffer, sizeof s_ProbeBuffer,
			  "file %s: open ok, canonical ok", Basename (pPath));
	}
	else if (pFile != nullptr)
	{
		snprintf (s_ProbeBuffer, sizeof s_ProbeBuffer,
			  "file %s: open ok, canonical FAILED: %s",
			  Basename (pPath), Error.message ().c_str ());
	}
	else
	{
		snprintf (s_ProbeBuffer, sizeof s_ProbeBuffer,
			  "file %s: open FAILED (errno %d)",
			  Basename (pPath), nOpenErrno);
	}

	return s_ProbeBuffer;
}

const char *BareProbeDir (const char *pPath)
{
	if (pPath == nullptr)
	{
		return "(null)";
	}

	errno = 0;
	DIR *pDir = opendir (pPath);
	const int nOpenErrno = errno;
	if (pDir != nullptr)
	{
		closedir (pDir);
	}

	snprintf (s_ProbeBuffer, sizeof s_ProbeBuffer, "dir %s: %s",
		  Basename (pPath),
		  pDir != nullptr ? "listable" : (nOpenErrno == 0 ? "not listable"
								 : "missing"));

	return s_ProbeBuffer;
}
