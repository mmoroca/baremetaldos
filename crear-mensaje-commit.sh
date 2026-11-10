#!/bin/bash
#
# crear-mensaje-commit.sh - escribe el mensaje del commit .78 en /tmp.
#
# Para qué: para no depender de copiar un fichero entre el workspace y tu
# máquina.  Lánzalo y luego usa el mensaje tal cual:
#
#   ./crear-mensaje-commit.sh
#   cd ~/devel/dosbox-circle
#   git add -A
#   git commit -F /tmp/commit-v0.78.txt
#
# No toca el repositorio: escribe un fichero en /tmp y nada más.
#
set -euo pipefail

OUT="${1:-/tmp/commit-v0.78.txt}"

cat > "$OUT" <<'MENSAJE'
BM-DOS 2026-11-10.78: cmdline.txt owns the display mode, governor and boot fixes

Display mode
------------
Circle creates its own console framebuffer before this port creates its own, and
its size comes from width= / height= in cmdline.txt, not from boot.conf.  Two
different geometries meant a mode change at run time, and that is what left the
display scanning memory that still held the console's text.

  * sd/cmdline.txt reduced to ONE line, under 2048 bytes.  Circle's parser splits
    on spaces only, and drops the whole file above 2048 bytes
    (TPropertyTagCommandLine::String[2048]), silently: a commented cmdline.txt
    was a cmdline.txt whose options were read as part of the last comment.
  * make-sd.sh and update-sd.sh write width= and height= from boot.conf,
    collapse the file to one line, and refuse to write anything over the limit.
  * make-sd.sh now copies boot.conf (it never did, so a fresh card fell back to
    the built-in 1280x720).
  * cmdline.txt has the LAST WORD on the display mode, and that is now what the
    kernel does.  It used to compare the console's physical mode against
    boot.conf and refuse to boot when they disagreed, which turned a cosmetic
    disagreement between two text files into a dead board.  It now prints both
    numbers, says which file it is using, adopts the console's geometry and
    carries on.  boot.conf keeps its display= because the scripts need a source
    for width=/height= at card-build time; it is a claim, not an order.
  * A comment that had been lying for three rounds is fixed: CBcmFrameBuffer's
    GetWidth() returns the number that went INTO the request, not what the
    firmware granted.  With width=/height= present the two coincide; without
    them the constructor fills the width from the display dimensions, which
    arrive with the overscan border already subtracted - that is where the
    1824x984 came from.
  * READMEs: a new "Configuration: cmdline.txt" section - one line, spaces only,
    2048 bytes, the exact line this project writes, and a table of the six
    options that matter here with Circle's real ranges (loglevel 0..5 is a
    severity ceiling and NOT a destination; keymap is the physical layout, not
    the DOS one; socmaxtemp above 78 is ignored by Circle, so the 80 on the card
    changes nothing).  Everything else links to Circle's doc/cmdline.txt, with a
    note on why fast= and sounddev= do nothing here.
  * sd/config.txt: disable_overscan=1.  The firmware subtracts a 48 px border per
    edge in CEA modes, which is the 1824x984 that appeared on the board.

Clock governor
--------------
  * Idleness is decided by DURATION, not by counters.  Three rules were tried on
    the board and two of them were mine and wrong:
      - "identical frame is idle" walked a Pi 3 down to 600 MHz in the middle of
        DOOM, because a game repeats its picture too; the audio broke up.
      - "identical and cheap is idle" fixed DOOM and pinned a DOS prompt at the
        top level at 63 C, because most of a 42 ms text frame is the emulated
        machine sleeping on its own timer, not working.
      - "advanced fewer times than the mode asks" was withdrawn before it shipped:
        a healthy machine reads 30/60 or 35/70, so the test is meaningless.
    What works is waiting ten seconds of a literally identical picture
    (governor::IdleFramesForPrompt).  A prompt does that; no game does.  Being
    wrong costs one level for three seconds.
  * IsStalledFrame() and IsWaitingFrame() removed, and the governor tick is back
    to three arguments: the counters cannot separate idle from working.
  * BuildLevels() now guarantees a strictly increasing table.  A ceiling below
    1500 used to produce {600, ceiling, ceiling} with two levels, so the governor
    believed it could step down while SetLevel() moved nothing.

Boot screen
-----------
  * Banner() removed: it painted the version block on Circle's console and
    Report() painted it again on ours.  The nine key-help lines went the same way.
  * BareVideoStatus() erases the line before drawing it, so a replaced message
    no longer overlaps the one before it.
  * boot_log = auto | quiet | off in boot.conf: how much of the boot is painted,
    never what is logged.

Build and checks
----------------
  * check-kernel.sh compiles to an object instead of -fsyntax-only: gcc only
    reports -Wunused-function when it generates code.
  * setup.sh applies patches/*.patch automatically, so a clone works on a Pi 3
    without a manual step.
  * Sources and script output in English.
  * READMEs: display mode by board, the cycles table by emulated CPU, and a
    "Lessons learned" section with the platform traps (cmdline.txt, the two
    framebuffers, overscan, -fsyntax-only, idle detection, single core).

Co-authored-by: arena.ai
MENSAJE

printf 'mensaje escrito en %s (%s lineas, %s bytes)\n' \
	"$OUT" "$(wc -l < "$OUT")" "$(wc -c < "$OUT")"
printf 'sha256: %s\n' "$(sha256sum "$OUT" | cut -d' ' -f1)"
printf '\n  cd ~/devel/dosbox-circle && git add -A && git commit -F %s\n\n' "$OUT"
