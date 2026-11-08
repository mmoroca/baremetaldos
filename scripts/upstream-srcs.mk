#
# Print the source list DOSBox-core builds for a libretro build with the fake
# SDL layer - the closest thing upstream has to "our" build.  Included by the
# Makefile; not meant to be run by hand.
#
# Usage:
#   make upstream-srcs           # included by the Makefile
#
# ---------------------------------------------------------------------------
# upstream-srcs.mk
# ---------------------------------------------------------------------------
# Prints the list of sources that DOSBox-core builds for a libretro build with
# the fake SDL layer, i.e. the closest thing upstream has to "our" build.
#
# It is three lines of make on purpose: Makefile.common is a plain fragment of
# make, so the correct way to know what it selects is to let make expand it,
# not to re-implement its ifeq conditions in Python.  The flags below are the
# ones upstream's own libretro/Makefile.libretro ends up with when
# WITH_FAKE_SDL=1 on Linux: no Voodoo (OpenGL), no ALSA MIDI, no IPX.
#
# Invoked as:  make -s -f scripts/upstream-srcs.mk CORE_DIR=<tree>
# ---------------------------------------------------------------------------
ifeq ($(CORE_DIR),)
$(error CORE_DIR is not set; run this through 'make upstream-srcs')
endif

WITH_FAKE_SDL := 1
WITH_VOODOO := 0
WITH_ALSA_MIDI := 0
WITH_IPX := 0

include $(CORE_DIR)/libretro/Makefile.common

.PHONY: print-upstream-srcs
print-upstream-srcs:
	@echo $(SOURCES_CXX) $(SOURCES_C)
