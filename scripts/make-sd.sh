#!/usr/bin/env bash
#
# Write a bootable BM-DOS SD card from scratch: partition, format, copy the
# kernel and the sd/ tree.
#
# Usage:
#   sudo ./scripts/make-sd.sh /dev/sdX          # REAL CARD: destructive
#   ./scripts/make-sd.sh --stage ./sdcard        # build a directory instead
#
# make-sd.sh - write a bootable BM-DOS SD card.
#
#   sudo ./scripts/make-sd.sh /dev/sdX            # real card: DESTRUCTIVE
#   ./scripts/make-sd.sh --stage ./sdcard         # just build a directory tree
#
# The staging mode is the useful one while developing: it assembles exactly the
# files a card needs in a local directory, so you can inspect them, copy them by
# hand, or rsync them to a mounted card without re-partitioning anything.
#
# Card layout (single FAT32 partition, which is what the Pi firmware wants and
# what Circle's FatFs reads):
#
#   /bootcode.bin, start*.elf, fixup*.dat, *.dtb   firmware for your model
#   /config.txt                                    Circle's firmware config
#   /cmdline.txt                                   Circle's options
#   /kernel8-rpi4.img     (or kernel8.img / kernel_2712.img)   <- the emulator
#   /dosbox/dosbox.conf                            DOSBox configuration
#   /dos/<GAME>/...                                your DOS software
#   /dosbox/saves/                                 overlay drive, save games
#   /dosbox/system/                                soundfonts etc. (optional)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

RASPPI="${RASPPI:-4}"

# The toolchain lives inside the project (opt/), and OUR Makefile is the thing
# that knows that: it prepends opt/arm-gnu-toolchain-*/bin to PATH for its own
# invocation.  This script calls Circle's makefiles directly (firmware, armstub)
# and those know nothing about our directory layout, so the same PATH has to be
# set up here.  Missing this is why "make -C .../boot armstub64" failed with
# "aarch64-none-elf-gcc: No existe el fichero o el directorio" while a normal
# build worked fine.
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$REPO_ROOT/opt}"
# "|| true" is not decoration: the script runs with -o pipefail, and ls exits 2
# when the glob does not match, so without it the whole script dies here - with
# no message at all - for anyone who has not unpacked the toolchain yet.
TOOLCHAIN_BIN="$(ls -d "$TOOLCHAIN_DIR"/arm-gnu-toolchain-*/bin 2>/dev/null | head -1 || true)"
if [ -n "$TOOLCHAIN_BIN" ]; then
	PATH="$TOOLCHAIN_BIN:$PATH"
	export PATH
fi
STAGE=""

die () { echo "error: $*" >&2; exit 1; }
say () { printf '\n=== %s\n' "$*"; }

case "${1:-}" in
--stage)
	STAGE="${2:?usage: $0 --stage <directory>}"
	;;
--stage=*)
	STAGE="${1#--stage=}"
	;;
"")
	die "usage: $0 /dev/sdX   or   $0 --stage <directory>"
	;;
*)
	TARGET_DEVICE="$1"
	;;
esac

# ---------------------------------------------------------------------------
# Which kernel did the build produce?
# ---------------------------------------------------------------------------

case "$RASPPI" in
3)	KERNEL=kernel8.img ;;
4)	KERNEL=kernel8-rpi4.img ;;
5)	KERNEL=kernel_2712.img ;;
*)	die "RASPPI must be 3, 4 or 5" ;;
esac

[ -f "$KERNEL" ] || die "$KERNEL not found - run 'make' first"

# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------

STAGE="${STAGE:-$(mktemp -d)}"
# dosbox/system/soundfonts must exist: dosbox-core iterates it at startup
# (midi, mt32.romdir) and logs a scare at notice level when it does not.  The
# directory itself is what it wants; the ROMs are optional and belong to the
# user, so they are never copied here.
mkdir -p "$STAGE/dosbox/saves" "$STAGE/dosbox/system/soundfonts" "$STAGE/dos"

say "Staging into $STAGE"

cp "$KERNEL" "$STAGE/"
# dosbox.conf is copied only if it is not already there, because it is the one
# file meant to be edited by hand (mounts, [autoexec], cycles).  Overwriting it
# on every staging run would silently undo that work.
if [ -f "$STAGE/dosbox/dosbox.conf" ]; then
	echo "  keeping existing $STAGE/dosbox/dosbox.conf (delete it to get the default back)"
else
	cp "$REPO_ROOT/sd/dosbox.conf" "$STAGE/dosbox/dosbox.conf"
fi
cp "$REPO_ROOT/sd/config.txt" "$STAGE/config.txt"
cp "$REPO_ROOT/sd/cmdline.txt" "$STAGE/cmdline.txt"

# Firmware.  Circle's boot/ directory carries the files that are known to work
# with the Circle version we built against, which is a better bet than whatever
# the distro ships.
FIRMWARE_SRC="$REPO_ROOT/third_party/circle-stdlib/libs/circle/boot"
if [ ! -d "$FIRMWARE_SRC" ]; then
	die "$FIRMWARE_SRC not found - run scripts/setup.sh first"
fi

say "Fetching the Raspberry Pi firmware (Circle's boot/ Makefile downloads it)"
# Circle does not keep the firmware binaries in git: its boot/Makefile fetches
# them from the Raspberry Pi foundation at the version that matches this
# Circle release.  That is the version we want, not whatever a distro ships.
make -C "$FIRMWARE_SRC" || die "could not fetch the firmware"

say "Building the AArch64 ARM stub for the Pi 4/5 (needed for FIQ)"
if ! make -C "$FIRMWARE_SRC" armstub64; then
	if [ "$RASPPI" = "4" ]; then
		die "could not build armstub8-rpi4.bin, and sd/config.txt asks for it: a Pi 4 firmware will not boot without it.  Fix the toolchain (make deps) or remove the armstub line from sd/config.txt."
	fi
	echo "  (not needed for this board, continuing)"
fi

say "Copying firmware files"
for f in bootcode.bin start.elf start4.elf start5.elf fixup.dat fixup4.dat \
	 fixup5.dat start_cd.elf start4cd.elf fixup_cd.dat fixup4cd.dat \
	 LICENCE.broadcom COPYING.linux armstub8-rpi4.bin; do
	if [ -f "$FIRMWARE_SRC/$f" ]; then
		cp "$FIRMWARE_SRC/$f" "$STAGE/"
	fi
done

# Device trees: the firmware picks by model, so copy every one present.
for f in "$FIRMWARE_SRC"/*.dtb; do
	[ -f "$f" ] && cp "$f" "$STAGE/"
done

# The Pi 5 needs one overlay in a subdirectory.
if [ -f "$FIRMWARE_SRC/bcm2712d0.dtbo" ]; then
	mkdir -p "$STAGE/overlays"
	cp "$FIRMWARE_SRC/bcm2712d0.dtbo" "$STAGE/overlays/"
fi

# A README on the card, because in six months you will not remember which
# directory is mounted as C:.
cat > "$STAGE/LEEME.txt" <<'EOF'
BM-DOS - tarjeta SD
=========================

/dosbox/dosbox.conf   Configuracion de DOSBox. Se edita en el PC.
/dos/                 Aqui van los juegos, un directorio por juego.
                      En DOS aparece como C:   (MOUNT C /dos)
/dosbox/saves/        Guardados y el disco de superposicion (overlay).
/dosbox/system/       ROMs/soundfonts opcionales.

Arranque: el emulador entra directamente en DOS y ejecuta el [autoexec] de
dosbox.conf.  Si quieres arrancar un juego automatico, descomenta las lineas
"cd" y "*.EXE" al final de ese fichero.

Teclado USB y raton USB: conectar antes de encender (el hot-plug funciona pero
el primer informe de un teclado conectado en caliente puede perder una tecla).

Para salir: escribe EXIT en la consola de DOS.
EOF

# ---------------------------------------------------------------------------
# Sanity checks before anything touches a disk
# ---------------------------------------------------------------------------
#
# The kernel is a static binary: Circle, newlib, libc++, every DOSBox source we
# compile and the emulator itself, all in one file.  ~5 MB is the expected
# order of magnitude.  This block exists because "5 MB seems too small to hold
# an OS and an emulator" is a reasonable thing to think - and the honest answer
# is not an opinion, it is the number, plus a check that the number is sane.
if [ -f "$STAGE/$KERNEL" ]; then
	KERNEL_KB="$(du -k "$STAGE/$KERNEL" | cut -f1)"
	STAGE_KB="$(du -sk "$STAGE" | cut -f1)"
	printf '\n  kernel:    %s KB  (%s)\n' "$KERNEL_KB" "$KERNEL"
	printf '  full card: %s KB  (kernel + firmware + dosbox.conf)\n' "$STAGE_KB"
	if [ "$KERNEL_KB" -lt 1000 ]; then
		die "$KERNEL is only ${KERNEL_KB} KB. That is not the emulator - it is a stub or an empty link. Do not copy it to a card."
	fi
	echo "             (a static AArch64 binary with libc++ and DOSBox in it: expect 4000-8000 KB)"
fi

# config.txt names the kernel and (on a Pi 4) the ARM stub.  Both live in
# model-specific sections, so the check has to read the file the way the
# firmware does: a bare line before any [section] applies always, a line inside
# [pi4] applies only on a Pi 4.  Without this, a Pi 4 build trips over the
# [pi3] kernel= line, and a missing armstub produces the firmware's useless
# "error loading armstub" with no hint of what to do.
if [ -f "$STAGE/config.txt" ]; then
	AWK_SECTION='
		/^[[:space:]]*\[/ {
			s = $0; gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", s); sect = s; next
		}
		/^[[:space:]]*(kernel|armstub)=/ {
			k = $0; sub(/=.*/, "", k); gsub(/^[[:space:]]+/, "", k)
			v = $0; sub(/^[^=]*=/, "", v); gsub(/[[:space:]]+$/, "", v)
			if (sect == "" ) print k "\t" v "\tglobal"
			else print k "\t" v "\t" sect
		}
	'
	OUR_SECTIONS="all global"
	case "$RASPPI" in
	3)	OUR_SECTIONS="$OUR_SECTIONS pi3 pi3+ pi02 pi2" ;;
	4)	OUR_SECTIONS="$OUR_SECTIONS pi4 cm4" ;;
	5)	OUR_SECTIONS="$OUR_SECTIONS pi5" ;;
	esac

	while IFS="$(printf '\t')" read -r KEY VALUE SECT; do
		[ -n "${KEY:-}" ] || continue
		case " $OUR_SECTIONS " in
		*" $SECT "*) ;;		# not our board: the firmware ignores it
		*) continue ;;
		esac
		if [ "$KEY" = "kernel" ]; then
			[ -f "$STAGE/$VALUE" ] || die "config.txt says kernel=$VALUE for this board (section [$SECT]) but $VALUE is not in the staged card. The build produced $(basename "$KERNEL") - are RASPPI and config.txt out of sync?"
		else
			if [ ! -f "$STAGE/$VALUE" ]; then
				echo "  warning: config.txt wants armstub=$VALUE (section [$SECT]) and it is not on the card."
				echo "           A Pi 4 firmware refuses to boot without it.  Build it with:"
				echo "             make -C third_party/circle-stdlib/libs/circle/boot armstub64"
				echo "           or remove the armstub line from sd/config.txt (this port does not use FIQ)."
			fi
		fi
	done < <(awk "$AWK_SECTION" "$STAGE/config.txt")
fi

# A note that is worth the four lines: a Circle kernel is not a bootable card by
# itself.  The firmware loads bootcode.bin, then reads config.txt, then the
# kernel.  Copying only the .img gives a Pi that shows nothing at all.
cat > "$STAGE/LEEME-ARRANQUE.txt" <<'EOF'
Esta tarjeta arranca asi (no vale copiar solo el .img):

  bootcode.bin, start*.elf, fixup*.dat   firmware, lo carga la GPU
  *.dtb (y overlays/ si hay)             device trees, por modelo
  config.txt                             le dice al firmware que kernel usar
  cmdline.txt                            opciones de Circle (log, teclado)
  armstub8-rpi4.bin                      obligatorio en Pi 4 si config.txt lo pide
  kernel8-rpi4.img                       el emulador

Todo esto lo coloca scripts/make-sd.sh.  Si copias a mano, copia TODOS los
ficheros de esta carpeta, no solo el kernel.
EOF

say "Contents of the card:"
find "$STAGE" -maxdepth 2 | sed "s|$STAGE|.|" | sort | head -40

# ---------------------------------------------------------------------------
# Write to the device, if one was given
# ---------------------------------------------------------------------------

if [ -n "${TARGET_DEVICE:-}" ]; then
	[ "$(id -u)" -eq 0 ] || die "writing to a device needs root"
	[ -b "$TARGET_DEVICE" ] || die "$TARGET_DEVICE is not a block device"

	# The WHOLE card, not one of its partitions.  This script repartitions and
	# reformats; handed a partition it writes a partition table *inside* it, and
	# the kernel then refuses to re-read it ("particiones escritas pero no hemos
	# podido informar al núcleo"), which looks like a much bigger problem than it
	# is.  Catching it here costs three lines.
	if [ "$(lsblk -no TYPE "$TARGET_DEVICE" 2>/dev/null)" = "part" ]; then
		die "$TARGET_DEVICE is a partition, not the card. This script repartitions the device, so it needs the whole thing:
    sudo $0 $(lsblk -no PKNAME "$TARGET_DEVICE" | sed 's|^|/dev/|')"
	fi

	echo
	echo "About to ERASE and write $TARGET_DEVICE:"
	lsblk -o NAME,SIZE,MODEL "$TARGET_DEVICE" | sed 's/^/  /'
	echo
	echo "Press Ctrl+C now if that is not what you want."
	sleep 5

	PART="${TARGET_DEVICE}"
	# /dev/mmcblk0 -> /dev/mmcblk0p1, /dev/sda -> /dev/sda1
	case "$TARGET_DEVICE" in
	*[0-9]) PART="${TARGET_DEVICE}p1" ;;
	*)	PART="${TARGET_DEVICE}1" ;;
	esac

	umount "${PART}" 2>/dev/null || true

	say "Partitioning"
	parted -s "$TARGET_DEVICE" mklabel msdos
	parted -s "$TARGET_DEVICE" mkpart primary fat32 1MiB 100%
	parted -s "$TARGET_DEVICE" set 1 lba on
	mkfs.vfat -F 32 -n DOSBOX "$PART"

	MOUNT_POINT="$(mktemp -d)"
	mount "$PART" "$MOUNT_POINT"
	trap 'umount "$MOUNT_POINT"; rmdir "$MOUNT_POINT"' EXIT

	say "Copying files"
	# The card is FAT32 and mounted by root; anything root created on a previous
	# write is owned by root and plain cp fails with "Permission denied" halfway
	# through, leaving a half-written card.  rsync handles ownership and deletions
	# better, and falls back to cp if rsync is not installed.
	if command -v rsync >/dev/null 2>&1; then
		rsync -a --no-owner --no-perms "$STAGE"/ "$MOUNT_POINT"/
	else
		cp -r "$STAGE"/. "$MOUNT_POINT"/
	fi
	sync

	say "Done.  Unmount and put the card in the Pi."
fi
