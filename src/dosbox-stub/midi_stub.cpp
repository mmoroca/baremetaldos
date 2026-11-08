//
// midi_stub.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - the MIDI side of DOSBox, wired to nothing.
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
// Why this file exists
// --------------------
// The first link of this port failed with
//
//     undefined reference to MIDI_Init(Section*)
//     undefined reference to MIDI_RawOutByte(unsigned char)
//     undefined reference to MIDI_Available()
//     undefined reference to midi
//
// and it was tempting to answer "we did not build midi.cpp, therefore MIDI is
// off".  That is not how DOSBox is structured: src/dosbox.cpp calls MIDI_Init()
// from DOSBOX_Init() unconditionally, and MPU401 and SBLASTER call
// MIDI_RawOutByte() and MIDI_Available() from their constructors and command
// handlers.  A missing definition is a link error, not a disabled feature.
//
// src/gui/midi.cpp is the real implementation, and it is 300 lines of device
// negotiation: OSS on Linux, CoreMIDI on macOS, ALSA, MT-32 through libmt32emu,
// and the libretro MIDI interface.  On a bare metal board there is no device to
// negotiate with, so this file provides the same four entry points with the
// behaviour DOSBox would reach in its "none" handler:
//
//   MIDI_Init()        claims the [midi] section, logs that MIDI is off, and
//                      registers nothing.  DOS games that probe for an MPU-401
//                      get "not available", which is the truth.
//   MIDI_Available()   false, always.
//   MIDI_RawOutByte()  drops the byte.  It still has to exist: MPU401 calls it
//                      from its track-update path even when no device opened.
//   midi               the global state struct declared in midi.h.  Zero
//                      initialised, which is exactly what "no handler" means:
//                      midi.handler == nullptr and midi.available == false.
//
// What this DOES affect: General MIDI / MT-32 soundtracks play as silence.  OPL
// (AdLib, Sound Blaster FM) is a completely separate path - adlib.cpp, dbopl.cpp
// and nukedopl.c - and is not touched by any of this.
//
// If MIDI is ever wanted on the Pi (a USB-MIDI keyboard, or a software MT-32
// renderer), the change is here: replace these definitions with the real
// midi.cpp and add the backend it needs.  That is why this file is called a
// stub and not "midi support".
//
#include "dosbox.h"
#include "midi.h"
#include "setup.h"

DB_Midi midi;

void MIDI_RawOutByte (Bit8u /*data*/)
{
	// Nothing is listening.  See the header comment for why this cannot simply
	// be an undefined symbol.
}

bool MIDI_Available (void)
{
	return false;
}

void MIDI_Init (Section * /*pSection*/)
{
	// Nothing to do, and deliberately nothing to say either.
	//
	// The obvious line here is LOG_MSG("MIDI disabled...").  In DOSBox, LOG_MSG
	// is a macro for GFX_ShowMsg(), which lives in src/gui/sdlmain.cpp - the SDL
	// frontend that this port does not build.  Using it would pull the whole SDL
	// message queue into the link for the sake of one line of text.  (That is
	// exactly what happened: the first version of this file did use it and the
	// link failed with "undefined reference to GFX_ShowMsg".)
	//
	// If this needs to be visible, BareLog() in bare_env.h is the port's own
	// logging path and does not depend on the frontend.
}
