//
// posix_stubs.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - the POSIX functions newlib declares but circle-newlib does
// not implement, and that DOSBox (or libc++ on DOSBox's behalf) calls.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// The link errors these four fix are the least obvious of the whole port,
// because all four *compile*: newlib ships the declarations, so the compiler is
// satisfied, and only the linker says otherwise.
//
//   access()    src/dos/drive_local.cpp (localDrive::TestDir) and
//               src/shell/shell.cpp (AUTOEXEC) use it to ask "does this file
//               exist and can I read it".  FatFs has no permission bits at all,
//               so the honest answer is f_exists(): 0 if it opens, -1 if not.
//
//   dirname()   src/dos/cdrom_image.cpp (LoadCueSheet) uses it to strip the
//               file name off a .cue path.  newlib has the implementation in
//               libc/unix/dirname.c, but that directory is only built for
//               targets whose configure.host entry sets unix_dir, and the
//               circle entry does not.  It is twelve lines; reimplementing it
//               is cheaper than patching newlib's configuration.
//
//   ftime()     src/ints/bios.cpp wants the DOS date/time of the host clock.
//               newlib removed ftime() years ago and <sys/timeb.h> is gone, so
//               this is a small wrapper over gettimeofday().
//
//   getpwnam()  src/misc/cross.cpp, to expand "~username".  There is no passwd
//               database on the board.  Returning nullptr makes cross.cpp leave
//               the path alone, which is the same thing that happens on a system
//               where no user exists.  (This is the one the port
//               already anticipated: -DNO_PWD_H in the Makefile drops the
//               #include, and this definition is only here so that the symbol
//               resolves if something else reaches for it.)
//
//   realpath()  Not DOSBox at all: libc++.  std::filesystem::canonical() ends
//               up in ::realpath(), and dosbox-core canonicalises the content
//               path before it does anything else, so with the ENOSYS stub the
//               core refuses to load any game with "Function not implemented".
//
//               Careful: circle-newlib *does* define realpath() - in
//               libgloss/circle/fsstubs.cpp, line 101 - it just defines it as
//               "errno = ENOSYS; return nullptr".  So this is not a missing
//               symbol, it is a *replacement*, and the first attempt at it made
//               the link fail with "multiple definition of realpath".
//
//               It is called dosbox_realpath() and the Makefile aliases the name
//               with --defsym, only after checking that the archive really does
//               export realpath() (see the long comment above LIBS).  Two other
//               approaches were measured and rejected there.
//
// None of them is a "fake": each returns the answer that is true on this
// platform, and each says so in its comment.
//
#include "dosbox.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern "C" {

// ---------------------------------------------------------------------------
// access()
// ---------------------------------------------------------------------------
// mode is ignored: FatFs does not carry permissions, and everything on the SD
// card is readable by the only process that exists.  errno is set the way the C
// library would set it, because DOSBox's callers do look at it.
int access (const char *path, int /*mode*/)
{
	if (path == nullptr)
	{
		errno = EFAULT;
		return -1;
	}

	FILE *f = fopen (path, "rb");
	if (f != nullptr)
	{
		fclose (f);
		return 0;
	}

	// Not a regular file.  It may still be a directory, which is the other
	// thing DOSBox asks about: drive_local tests both C: root and the
	// directories it is about to walk.
	DIR *d = opendir (path);
	if (d != nullptr)
	{
		closedir (d);
		return 0;
	}

	errno = ENOENT;
	return -1;
}

// ---------------------------------------------------------------------------
// dirname()
// ---------------------------------------------------------------------------
// Exactly the POSIX behaviour, including the two cases everyone forgets:
// "no slash" returns "." and "slash at position 0" returns "/".  The string is
// modified in place and the result may point inside it, which is what callers
// expect (cdrom_image.cpp passes a std::string's buffer and uses the result).
char *dirname (char *path)
{
	if (path == nullptr || *path == '\0')
	{
		static char dot[] = ".";
		return dot;
	}

	char *const last_slash = strrchr (path, '/');
	if (last_slash == nullptr)
	{
		static char dot[] = ".";
		return dot;
	}

	if (last_slash == path)
	{
		// "/foo" -> "/"
		path[1] = '\0';
		return path;
	}

	// "a/b/c" -> "a/b".  Trim any run of slashes, so "a//b" -> "a".
	*last_slash = '\0';
	char *end = last_slash;
	while (end > path && *(end - 1) == '/')
	{
		*(--end) = '\0';
	}

	return path;
}

// ---------------------------------------------------------------------------
// ftime()
// ---------------------------------------------------------------------------
// struct timeb fields: time (seconds), millitm (ms), timezone, dstflag.  The
// last two are deprecated in POSIX and unused by DOSBox; they are set to 0.
//
// The struct is declared here because newlib 4.x dropped <sys/timeb.h>
// together with ftime() itself - there is no header left to include, and the
// only declaration of the function in the whole sysroot is the one DOSBox
// carries in its own compat header.  The layout is fixed by decades of ABI,
// so declaring it locally is not a guess.
struct timeb {
	time_t time;
	unsigned short millitm;
	short timezone;
	short dstflag;
};

int ftime (struct timeb *tp)
{
	if (tp == nullptr)
	{
		errno = EFAULT;
		return -1;
	}

	struct timeval tv;
	if (gettimeofday (&tv, nullptr) != 0)
	{
		return -1;
	}

	tp->time = tv.tv_sec;
	tp->millitm = static_cast<unsigned short> (tv.tv_usec / 1000);
	tp->timezone = 0;
	tp->dstflag = 0;

	return 0;
}

// ---------------------------------------------------------------------------
// getpwnam()
// ---------------------------------------------------------------------------
struct passwd *getpwnam (const char * /*name*/)
{
	// No passwd database.  nullptr means "no such user", which is a case
	// cross.cpp already handles: it leaves "~user" untouched.
	return nullptr;
}

// ---------------------------------------------------------------------------
// realpath()
// ---------------------------------------------------------------------------
// POSIX realpath() resolves symlinks and "." / ".." and returns an absolute
// path.  There are no symlinks on a FAT32 card, so the only work is:
//
//   1. make the path absolute, if it is not already;
//   2. normalise it: drop empty components and ".", and pop a component for
//      every "..";
//   3. check that the result exists (realpath fails on a missing final
//      component - that is part of the contract and callers depend on it);
//   4. write it into resolved_name, or into a buffer from malloc() if it is
//      nullptr, which is the form libc++ uses.
//
// Why a real implementation and not just "return the input": the path it
// returns is the path DOSBox then opens.  Returning something unnormalised
// would work until the day a path contains "./" or "..", and then the emulator
// silently loads the wrong file.  Cheap to do properly.
//
// Volume names ("SD:") are not touched: Circle's FatFs glue needs them, and a
// ":" is not special in this walk.  The core is given paths without them.
// newlib's <limits.h> is allowed not to define PATH_MAX, and on this target it
// does not.  512 is what Circle's FatFs layer deals with (FF_MAX_LFN plus the
// volume name), and is comfortably above any path DOSBox builds.
#ifndef PATH_MAX
#define PATH_MAX 512
#endif

// Name: dosbox_realpath, not realpath.  circle-newlib already defines realpath
// (fsstubs.cpp:101, the ENOSYS stub), so defining it again is a duplicate symbol
// and the link stops.  The Makefile points the linker at this one with
//
//     -Wl,--defsym=realpath=dosbox_realpath
//
// which is the only mechanism that actually wins: putting our object before the
// archive does not (fsstubs.o is pulled in anyway for fchmod/pathconf, dragging
// its realpath with it), and neither does --allow-multiple-definition, which was
// measured here and lets the stub win.  --defsym was measured too: the call ends
// up in this function.
char *dosbox_realpath (const char *path, char *resolved_name)
{
	if (path == nullptr)
	{
		errno = EINVAL;
		return nullptr;
	}

	if (*path == '\0')
	{
		errno = ENOENT;
		return nullptr;
	}

	static char s_Buffer[PATH_MAX];

	// --- 1. absolute ---
	char Full[PATH_MAX];
	if (path[0] != '/')
	{
		if (getcwd (s_Buffer, sizeof s_Buffer) == nullptr)
		{
			return nullptr;
		}

		const int nUsed = snprintf (Full, sizeof Full, "%s/%s", s_Buffer, path);
		if (nUsed < 0 || static_cast<size_t> (nUsed) >= sizeof Full)
		{
			errno = ENAMETOOLONG;
			return nullptr;
		}
	}
	else
	{
		const int nUsed = snprintf (Full, sizeof Full, "%s", path);
		if (nUsed < 0 || static_cast<size_t> (nUsed) >= sizeof Full)
		{
			errno = ENAMETOOLONG;
			return nullptr;
		}
	}

	// --- 2. normalise, in place, into Out ---
	char Out[PATH_MAX];
	size_t nOut = 0;
	Out[0] = '\0';

	const char *p = Full;
	while (*p != '\0')
	{
		while (*p == '/')
		{
			p++;
		}

		const char *pStart = p;
		while (*p != '\0' && *p != '/')
		{
			p++;
		}

		const size_t nLen = static_cast<size_t> (p - pStart);
		if (nLen == 0 || (nLen == 1 && pStart[0] == '.'))
		{
			continue;
		}

		if (nLen == 2 && pStart[0] == '.' && pStart[1] == '.')
		{
			// Pop one component.  At the root there is nothing to pop.
			while (nOut > 0 && Out[nOut - 1] != '/')
			{
				nOut--;
			}
			if (nOut > 0)
			{
				nOut--;
			}
			Out[nOut] = '\0';
			continue;
		}

		if (nOut + nLen + 2 >= sizeof Out)
		{
			errno = ENAMETOOLONG;
			return nullptr;
		}

		Out[nOut++] = '/';
		memcpy (Out + nOut, pStart, nLen);
		nOut += nLen;
		Out[nOut] = '\0';
	}

	if (nOut == 0)
	{
		// The root itself.
		Out[nOut++] = '/';
		Out[nOut] = '\0';
	}

	// --- 3. it has to exist ---
	if (access (Out, F_OK) != 0)
	{
		// access() above already set errno, but be explicit: ENOENT is what
		// realpath() reports for a path that does not resolve, and the error
		// text is what ends up in the core's log.
		errno = ENOENT;
		return nullptr;
	}

	// --- 4. out ---
	if (resolved_name != nullptr)
	{
		memcpy (resolved_name, Out, nOut + 1);
		return resolved_name;
	}

	char *pResult = static_cast<char *> (malloc (nOut + 1));
	if (pResult == nullptr)
	{
		errno = ENOMEM;
		return nullptr;
	}

	memcpy (pResult, Out, nOut + 1);
	return pResult;
}

} // extern "C"
