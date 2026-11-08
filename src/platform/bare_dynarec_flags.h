//
// bare_dynarec_flags.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.
//
// The two numbers DOSBox's dynrec compares C_TARGETCPU against, copied here so
// that a check can prove the flag actually selects the AArch64 backend.
//
// Why this file exists at all
// ---------------------------
// Upstream passes the target cpu like this (libretro/Makefile.common:239):
//
//     COMMONFLAGS += -DC_DYNREC -DC_TARGETCPU="ARMV8LE"
//
// and shell quoting strips the quotes, so the compiler really sees
// -DC_TARGETCPU=ARMV8LE, and core_dynrec.cpp then has
//
//     #define ARMV8LE         0x07        (line 141)
//     ...
//     #elif C_TARGETCPU == ARMV8LE
//     #include "core_dynrec/risc_armv8le.h"
//
// i.e. **the value is a number, and the comparison is numeric**.  A Makefile
// that keeps the quotes - which is what this port did until .63 -
// hands the compiler -DC_TARGETCPU="ARMV8LE", a *string*, and the preprocessor
// says
//
//     error: token ""ARMV8LE"" is not valid in preprocessor expressions
//
// so the build stops instead of silently falling through to the interpreter.
// That is the good outcome; the bad one is a target whose name happens to be a
// bare identifier, which evaluates to 0 in #if and quietly selects nothing.
// Either way the flag is wrong, and nothing in this tree could see it because
// there was no host build of core_dynrec.cpp.
//
// So: the numbers live here, in a header the check compiles, and the Makefile
// passes the bare token.  If upstream ever renumbers them, `make
// check-dynarec-target` fails and says so, which is a better way to find out
// than a board that runs at interpreter speed with a JIT compiled in.
//
#pragma once
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.

// src/cpu/core_dynrec.cpp, lines 136-141
#define BAREDYN_TARGET_X86      0x01
#define BAREDYN_TARGET_X86_64   0x02
#define BAREDYN_TARGET_MIPSEL   0x03
#define BAREDYN_TARGET_ARMV4LE  0x04
#define BAREDYN_TARGET_ARMV7LE  0x05
#define BAREDYN_TARGET_ARMV8LE  0x07

// The names, defined where core_dynrec.cpp defines them and with its values,
// because C_TARGETCPU is *given* as one of these names: -DC_TARGETCPU=ARMV8LE
// means C_TARGETCPU expands to ARMV8LE, which expands to 0x07, which is what
// the #if compares.  A check that skipped this step would be checking a number
// nobody uses.
#ifndef ARMV8LE
#define ARMV8LE BAREDYN_TARGET_ARMV8LE
#endif
#ifndef ARMV7LE
#define ARMV7LE BAREDYN_TARGET_ARMV7LE
#endif
#ifndef ARMV4LE
#define ARMV4LE BAREDYN_TARGET_ARMV4LE
#endif
