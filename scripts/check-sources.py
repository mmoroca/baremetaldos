#!/usr/bin/env python3
#
# Compare the source files DOSBox-core compiles with the ones this Makefile
# compiles, so that a file upstream adds cannot silently stop being built here.
# Both lists come from `make`, never from a text analysis.
#
# Usage:
#   make check-sources           # part of make check-host; needs third_party/dosbox-core
#
"""Compare the source files DOSBox-core compiles with the ones this port does.

Both lists come from 'make', never from a text analysis:

    make print-srcs       -> $(ALL_SRCS) of this Makefile
    make upstream-srcs    -> $(SOURCES_CXX) $(SOURCES_C) of
                             libretro/Makefile.common, expanded by make with the
                             same WITH_* flags upstream uses

So what this prints is a fact: a file is in both lists, or in neither, or it is
in EXCEPTIONS with a reason written by hand.

Exit codes: 0 = everything matches, 1 = something is missing or extra, 2 = one
of the two lists could not be read.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOSBOX = os.environ.get("DOSBOX_DIR", "third_party/dosbox-core")

# Files upstream compiles and this port does not, with the reason.  A reason has
# to say what happens to the code, not "we do not need it": if the core calls it,
# not compiling it is a link failure waiting to happen.
EXCEPTIONS = {
    "libretro/src/emu_thread.cpp":
        "DBC_SINGLE_THREADED=1: switchThread() is provided by bare_thread.cpp,"
        " because Circle has no std::thread. Set that to 0 to compile this one",
    "libretro/src/fakesdl/SDL/SDL_endian.cpp":
        "the fakesdl layer would duplicate the SDL_* symbols already provided by"
        " src/platform/",
    "libretro/src/fakesdl/SDL/SDL_thread.cpp": "same as SDL_endian.cpp",
    "libretro/src/fakesdl/SDL/SDL_timer.cpp": "same as SDL_endian.cpp",
    "src/gui/midi.cpp":
        "includes <mt32emu/mt32emu.h> unconditionally (midi_mt32.h:7); replaced"
        " by src/dosbox-stub/midi_stub.cpp",
    "src/gui/midi_mt32.cpp": "same as midi.cpp: there is no libmt32emu",
    "src/dos/cdrom_aspi_win32.cpp": "the whole body is under #if defined(WIN32)",
    "src/dos/cdrom_ioctl_win32.cpp": "the whole body is under #if defined(WIN32)",
}


def run_make(target):
    cmd = ["make", "-s", "--no-print-directory", target, "DOSBOX_DIR=" + DOSBOX]
    try:
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write("fallo '%s':\n%s\n" % (" ".join(cmd), exc.stderr))
        sys.exit(2)
    return out.stdout.split()


def relative(path):
    """Deja la ruta relativa a la raiz de dosbox-core."""
    for root in (os.path.abspath(os.path.join(ROOT, DOSBOX)), "/tmp/dbcore"):
        if path.startswith(root + os.sep):
            return path[len(root) + 1:]
    if path.startswith("$(DOSBOX_DIR)/"):
        return path[len("$(DOSBOX_DIR)/"):]
    return path


def main():
    raw_ours = run_make("print-srcs")
    core_dir = os.path.abspath(os.path.join(ROOT, DOSBOX))
    # Two normalisation problems, both found by running this instead of reading
    # it, and both worth writing down because they are the same mistake twice:
    #
    #   * upstream also names files OUTSIDE the core (libretro/deps/common/*.c),
    #     and relative() only shortens paths that live inside the core, so `ours`
    #     was a mix of relative and absolute paths;
    #   * therefore the path comparison has to be done in one space, and it has to
    #     be this tree's space, not the core's.
    #
    # `core_relative()` is that one space.  Before this, the report was wrong in
    # both directions depending on how third_party was mounted (a symlink to
    # /tmp/dbcREV made DOSBOX point at a path that the Makefile expanded
    # differently), which is exactly the kind of number that makes people stop
    # trusting a check.
    def core_relative(path):
        absolute = path if os.path.isabs(path) else os.path.abspath(os.path.join(ROOT, path))
        if absolute.startswith(core_dir + os.sep):
            return os.path.relpath(absolute, core_dir).replace(os.sep, "/")
        return None

    def root_relative(path):
        absolute = path if os.path.isabs(path) else os.path.join(ROOT, path)
        return os.path.relpath(absolute, ROOT).replace(os.sep, "/")

    # Named here because the renamed entry (`ours.discard`) would otherwise make
    # the count below one short, and a count that changes for a bookkeeping
    # reason is a count nobody reads.
    n_core_files = sum(1 for f in raw_ours if core_relative(f) is not None)

    # BOTH lists have to be expressed the same way, or the comparison is between
    # two different spellings of the same file and every line says FALTA.  That
    # happened here once already, in this very function: `ours` was normalised to
    # the tree's root and `theirs` to the core's, and the run reported all 130+
    # core files missing.  One space, and it is the core's for anything that lives
    # in the core.
    ours = {core_relative(f) or root_relative(f) for f in raw_ours}
    theirs = {core_relative(f) or root_relative(f) for f in run_make("upstream-srcs")}

    # Renamed sources: not an exemption - the file IS compiled - but the mapping
    # between the path upstream names and the path that stands in for it here.
    # The dynarec is the case: with DYNAREC=1 the CPU engine is
    # build/bare_dynrec_unit.cpp, a generated wrapper that #includes
    # src/cpu/core_dynrec.cpp and redefines CPU_Core_Dynrec_Cache_Init on top of
    # it (see the long note next to DOSBOX_DYNAREC_HOOK in the Makefile).
    RENAMED = {
        "src/cpu/core_dynrec.cpp": "build/bare_dynrec_unit.cpp",
    }

    for upstream_path, our_path in RENAMED.items():
        if upstream_path in theirs and our_path in ours:
            theirs.discard(upstream_path)
            ours.discard(our_path)

    if not ours or not theirs:
        sys.stderr.write("una de las dos listas sale vacia: no hay nada que comparar\n")
        sys.exit(2)

    missing = sorted(theirs - ours - set(EXCEPTIONS))
    obsolete = sorted(set(EXCEPTIONS) - (theirs - ours))
    used = sorted(set(EXCEPTIONS) & (theirs - ours))

    print("  upstream compiles: %d files" % len(theirs))
    print("  compiled here: %d files (%d from dosbox-core, %d original to the port)"
          % (len(ours), n_core_files, len(raw_ours) - n_core_files))
    print("  deliberately not compiled: %d" % len(used))
    for name in used:
        print("    %s\n      %s" % (name, EXCEPTIONS[name]))

    for name in missing:
        print("FALTA %s  (upstream lo compila; o lo anades, o va a EXCEPTIONS con"
              " el motivo)" % name)
    for name in obsolete:
        print("SOBRA en EXCEPTIONS %s  (ya no hay diferencia: borra la entrada)"
              % name)

    if missing or obsolete:
        return 1
    print("  OK: no falta nada y no sobra ninguna excepcion")
    return 0


if __name__ == "__main__":
    sys.exit(main())
