#!/usr/bin/env python3
#
# gen-sf2reader.py
#
# by @mmoroca & arena.ai 2026  -  https://arena.ai
#
# Extracts the SoundFont block reader out of src/dosbox-stub/midi_stub.cpp so it
# can be run on the host.  Same reason as gen-resample.py and gen-scaling.py: a
# copy of the code in a test is a test of the copy.  The markers below are the
# two edges of the block; if either disappears the generator fails loudly
# instead of silently writing a test that proves nothing.
#
# Usage: python3 scripts/hostcheck/gen-sf2reader.py [salida]
#
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "dosbox-stub", "midi_stub.cpp")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "sf2reader.inc")

# Two blocks: the reader itself, and OpenStage() - which is where the RAM guard
# lives, and a guard that refuses too much is its own kind of bug.
BLOCKS = (
    ("constexpr size_t kReadBlock = 64 * 1024;",
     "// The plain path: tsf reads straight from stdio"),
    ("bool OpenStage (const char *pPath)",
     "tsf *LoadPlain (const char *pPath)"),
)

with open(SRC, encoding="utf-8") as f:
    text = f.read()

out = []
for start, end in BLOCKS:
    try:
        i0 = text.index(start)
    except ValueError:
        sys.exit("gen-sf2reader: no encuentro %r en %s" % (start, SRC))

    try:
        i1 = text.index(end, i0)
    except ValueError:
        sys.exit("gen-sf2reader: no encuentro %r en %s" % (end, SRC))

    out.append(text[i0:i1])

block = "\n".join(out)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(block)

print("gen-sf2reader: %d bytes de codigo real en %s" % (len(block), OUT))
