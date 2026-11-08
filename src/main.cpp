//
// main.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - entry point.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "kernel.h"

#include <circle/startup.h>

#include <cstdlib>

// ---------------------------------------------------------------------------
// main - the two lines of control flow this program has
// ---------------------------------------------------------------------------
//
// Circle's startup code (lib/startup.S) sets up the CPU and the stack, then
// calls main().  Everything below is the *only* control flow at this level:
// construct, initialise, run, halt or reboot.  Both exits end in a Circle
// function that never returns, because there is nothing to return to - the
// alternative would be falling off the end into whatever the linker put next.
//
// Things that may surprise a reader coming from a hosted C++ program:
//
//  * The constructor is doing real work.  CStdlibAppStdio's constructor brings
//    up the logger, the interrupt system, the timer and USB; CKernel's own
//    members (the scheduler) have side effects too.  Stack-allocating the
//    object IS the hardware initialisation.
//
//  * There is no exit() and no returning to an OS.  main() either halts the CPU
//    or resets the SoC.  That is why a crash has to be diagnosed from the
//    screen: there is no process to kill.
//
//  * The two failure paths differ, and the difference is deliberate: a failed
//    Initialize() halts *without* Cleanup(), because tearing the frame buffer
//    down would erase the status lines that say what failed.
int main (void)
{
	// Cannot return from main(): the C++ runtime in Circle does not run
	// destructors of static objects, so we halt or reboot explicitly.
	CKernel Kernel;

	if (!Kernel.Initialize ())
	{
		// No Cleanup() on this path: tearing the frame buffer down blanks the
		// display, and the status lines written by the failed initialisation
		// are the only diagnostic there is once the emulator owns the screen.
		// halt() leaves them where they are.
		halt ();
		return EXIT_HALT;
	}

	const CStdlibApp::TShutdownMode ShutdownMode = Kernel.Run ();

	Kernel.Cleanup ();

	switch (ShutdownMode)
	{
	case CStdlibApp::ShutdownReboot:
		reboot ();
		return EXIT_REBOOT;

	case CStdlibApp::ShutdownHalt:
	default:
		halt ();
		return EXIT_HALT;
	}
}
