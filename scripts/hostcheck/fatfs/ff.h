// fatfs/ff.h - stand-in for Circle's addon/fatfs/ff.h, used by `make check-kernel`
// and by nothing else.
//
// Why a stand-in and not the real header: the real one lives in the Circle and
// circle-stdlib trees, which only exist on the board build.  This one exists
// because kernel.cpp now includes bare_storage.h, and a check that skipped
// kernel.cpp would be a check that misses the file which broke the .59 build.
//
// It declares ONLY what bare_storage.h uses.  A signature that differs from the
// real FatFs will pass here - that is the known cost of not having the tree - so
// the calls in bare_storage.h were read against Circle's ff.h by hand:
//
//   f_getfree (const TCHAR *path, DWORD *nclst, FATFS **fatfs)
//   f_open    (FIL *fp, const TCHAR *path, BYTE mode)
//   f_sync    (FIL *fp)
//   f_close   (FIL *fp)
//   f_mount   (FATFS *fs, const TCHAR *path, BYTE opt)
//
// and the error codes used (FR_OK, FR_DISK_ERR, FR_NOT_READY,
// FR_INVALID_DRIVE, FR_INVALID_OBJECT) are the ones ff.h defines with those
// exact names.

#ifndef CHECK_FF_H
#define CHECK_FF_H

typedef char TCHAR;
typedef unsigned char BYTE;
typedef unsigned long DWORD;

typedef enum
{
	FR_OK = 0,
	FR_DISK_ERR,
	FR_INT_ERR,
	FR_NOT_READY,
	FR_NO_FILE,
	FR_NO_PATH,
	FR_INVALID_NAME,
	FR_DENIED,
	FR_EXIST,
	FR_INVALID_OBJECT,
	FR_WRITE_PROTECTED,
	FR_INVALID_DRIVE,
	FR_NOT_ENABLED,
	FR_NO_FILESYSTEM,
	FR_MKFS_ABORTED,
	FR_TIMEOUT,
	FR_LOCKED,
	FR_NOT_ENOUGH_CORE,
	FR_TOO_MANY_OPEN_FILES,
	FR_INVALID_PARAMETER,
} FRESULT;

// The four mode bits bare_storage.h uses.  FA_OPEN_ALWAYS is 0x10 in FatFs
// (R0.13+): it creates the file when it is missing and opens it when it is not,
// which is what makes the flush probe work on a fresh card and on every boot
// after.  Copying the value wrong would be a probe that only ever works on a
// card that already has the file, so it is the real FatFs number.
#define FA_READ			0x01
#define FA_WRITE		0x02
#define FA_OPEN_EXISTING	0x00
#define FA_OPEN_ALWAYS		0x10

typedef struct
{
	DWORD nDummy;
} FILINFO;

typedef struct
{
	void *pFs;
	unsigned nDummy;
} FIL;

typedef struct
{
	unsigned nDummy;
} FATFS;

extern "C" {
FRESULT f_getfree (const TCHAR *path, DWORD *nclst, FATFS **fatfs);
FRESULT f_open (FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_sync (FIL *fp);
FRESULT f_close (FIL *fp);
FRESULT f_mount (FATFS *fs, const TCHAR *path, BYTE opt);
}

#endif
