//
// circle_stdlib_app.h - a stand-in for circle-stdlib, so that kernel.cpp can be
// compiled on the host.
//
// `make check-platform` covers src/platform/, which needs little from Circle.
// kernel.cpp is the other half, and it is the file where a mistake costs a
// five-minute cross build to find - the .54 round lost an hour to calling
// CKernel::Report() from a free function, which this catches in two seconds.
//
// It declares ONLY what kernel.cpp uses today: the base class, its logger, the
// scheduler, the interrupt system and the shutdown enum.  It is not a faithful
// copy and it is not meant to be - the values do not even have to be right,
// because nothing here is executed.  If kernel.cpp starts using something else
// from circle-stdlib, this file grows one line and `make check-kernel` says so
// by failing to compile.
//
// See scripts/hostcheck/check-kernel.sh.
//
#pragma once
#include <circle/types.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <cstring>
static const char *const KernelName = "dosbox";
class CInterruptSystem {};
// main.cpp returns these to Circle's startup code, which turns them into
// halt()/reboot().  Values do not matter here - nothing runs.
#define EXIT_HALT		0
#define EXIT_REBOOT		1

class CStdlibApp {
public:
	enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };
public:
	CStdlibApp (const char *pName) : mName (pName) {}
	virtual ~CStdlibApp (void) {}
	virtual bool Initialize (void) { return true; }
	virtual TShutdownMode Run (void) { return ShutdownHalt; }
	virtual void Cleanup (void) {}
	const char *GetKernelName (void) const { return mName; }
protected:
	const char *mName;
	CInterruptSystem mInterrupt;
};
class CStdlibAppStdio : public CStdlibApp {
public:
	CStdlibAppStdio (const char *pName) : CStdlibApp (pName) {}
};
