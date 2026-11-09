#!/bin/bash
#
# Update a card that already boots, without touching its games, its dosbox.conf
# or anything else that was put there by hand.
#
# Usage:
#   sudo ./scripts/update-sd.sh /dev/sdX         # only kernel + sd/ defaults
#   sudo ./scripts/update-sd.sh --stage ./sdcard
#
# make-sd.sh is the "start from nothing" path: it partitions, it formats, it
# writes the firmware.  This script does the opposite.  It is for the case where
# there is a card in the Pi, with games and programs in /dos, and a new kernel
# has been built.
#
# The rule it follows, in one sentence: it may replace files it owns, it may
# create files that are missing, and it never deletes anything it did not create
# in this run.
#
#   * It never partitions, never formats, never uses parted or mkfs.
#   * It refuses to touch a directory that does not look like a BM-DOS card.
#   * /dos (your games) and /dosbox/saves (your saves) are only ever created if
#     missing.  Their contents are not read, not compared, not written.
#   * dosbox.conf is yours: if it exists, it is left alone, always.
#   * config.txt and cmdline.txt are ours, but you may have edited them (logdev,
#     keymap, gpu_mem...).  If one differs, the card's copy is saved as
#     <file>.bak and never overwritten again (one backup, the oldest), and then
#     ours is written.
#   * Files are compared with cmp, not by size or date.  The repository carries
#     frozen timestamps (Jan 2 1980) and FAT has two-second mtime resolution, so
#     any date-based decision here would be wrong in one direction or the other.
#   * --dry-run prints the plan and writes nothing.  Use it first; it is free.
#
# Usage:
#
#   ./scripts/update-sd.sh --dry-run                     # show the plan
#   ./scripts/update-sd.sh                               # update the one card
#   ./scripts/update-sd.sh --sdcard /run/media/me/DOSBOX # name it explicitly
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set -eu
set -o pipefail

# The toolchain for computing the hash of the new kernel, if it is needed.  Not
# required: this script copies a file that make has already produced.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DRY_RUN=0
SDCARD=""
TARGET_STAMP="$(sed -n 's/^PROJECT_SNAPSHOT ?= //p' "$REPO_ROOT/Makefile" 2>/dev/null | head -1)"
[ -n "$TARGET_STAMP" ] || TARGET_STAMP="desconocida"

usage () {
	sed -n '3,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--dry-run)
		DRY_RUN=1
		shift
		;;
	--sdcard)
		[ $# -ge 2 ] || { echo "--sdcard necesita un directorio" >&2; exit 2; }
		SDCARD="$2"
		shift 2
		;;
	-h|--help)
		usage 0
		;;
	*)
		echo "Opción desconocida: $1" >&2
		usage 2
		;;
	esac
done

say () { printf '%s\n' "$*"; }
step () { printf '  %-10s %s\n' "$1" "$2"; }

# ---------------------------------------------------------------------------
# 1. Find the card
# ---------------------------------------------------------------------------
#
# A card is a mounted FAT filesystem on a removable device.  The check uses
# findmnt, which reads /proc/self/mountinfo: it says what is mounted *now*, not
# what the hardware is, which is exactly the question here.

find_card_by_mountpoint () {
	# Prints every mounted vfat filesystem whose device is removable, one
	# mountpoint per line.  Anything unexpected (no lsblk, no findmnt) prints
	# nothing rather than something wrong.
	command -v findmnt >/dev/null 2>&1 || return 0
	command -v lsblk   >/dev/null 2>&1 || return 0

	findmnt -rno TARGET,SOURCE,FSTYPE | while read -r mp src fstype; do
		case "$fstype" in
		vfat|msdos|exfat) ;;
		*) continue ;;
		esac

		# /dev/mmcblk0p1 -> mmcblk0 ; /dev/sdd1 -> sdd
		dev="$(basename "$src")"
		parent="$(lsblk -no PKNAME "/dev/$dev" 2>/dev/null | head -1)"
		[ -n "$parent" ] || continue

		rm="$(lsblk -dno RM "/dev/$parent" 2>/dev/null | head -1)"
		[ "$rm" = "1" ] || continue

		printf '%s\n' "$mp"
	done
}

looks_like_a_card () {
	# Not a guess about the hardware: the three files that make a card boot
	# this port.  Without config.txt there is no boot configuration; without an
	# image there is no kernel.
	[ -f "$1/config.txt" ] || return 1
	[ -d "$1/dos" ] || return 1
	for k in kernel8-rpi4.img kernel8.img kernel_2712.img; do
		[ -f "$1/$k" ] && return 0
	done
	return 1
}

if [ -z "$SDCARD" ]; then
	candidates="$(find_card_by_mountpoint)"
	n="$(printf '%s\n' "$candidates" | grep -c . || true)"

	if [ "$n" -eq 0 ]; then
		echo "No encuentro ninguna tarjeta montada (FAT, en un dispositivo extraíble)." >&2
		echo >&2
		echo "Si está montada pero no la reconozco, pásale el directorio a mano:" >&2
		echo "    $0 --sdcard /run/media/$USER/ETIQUETA" >&2
		exit 2
	fi

	if [ "$n" -gt 1 ]; then
		echo "Hay más de una candidata. Dime cuál, con --sdcard:" >&2
		printf '%s\n' "$candidates" | sed 's/^/    /' >&2
		exit 2
	fi

	SDCARD="$candidates"
fi

DADO="$SDCARD"
SDCARD="$(cd "$SDCARD" 2>/dev/null && pwd)" || {
	echo "No existe el directorio $DADO" >&2
	exit 2
}

if ! looks_like_a_card "$SDCARD"; then
	echo "$SDCARD no parece una tarjeta de BM-DOS:" >&2
	echo "debería tener config.txt, un kernel*.img y un directorio dos/." >&2
	echo >&2
	echo "Puede ser la tarjeta de otra cosa, o una tarjeta que aún no se ha" >&2
	echo "preparado nunca (para eso está ./scripts/make-sd.sh)." >&2
	exit 3
fi

# Write access is checked without writing: -w on the directory, and a real test
# only if it matters.  A FAT mount from udisks is owned by the calling user, so this
# should not need sudo.
if [ "$DRY_RUN" -eq 0 ] && [ ! -w "$SDCARD" ]; then
	echo "$SDCARD no se puede escribir con tu usuario." >&2
	echo "Si la montó root, desmóntala y vuelve a montarla, o usa sudo." >&2
	exit 4
fi

FS_SOURCE="$(findmnt -rno SOURCE --target "$SDCARD" 2>/dev/null || echo '?')"
FS_TYPE="$(findmnt -rno FSTYPE --target "$SDCARD" 2>/dev/null || echo '?')"

say "  CARD       $SDCARD"
say "             $FS_SOURCE ($FS_TYPE)"
say "  DESDE      $REPO_ROOT"
say "  SELLO      $TARGET_STAMP"
say ""

if [ "$DRY_RUN" -eq 1 ]; then
	say "  MODO       --dry-run: no se escribe nada"
	say ""
fi

# ---------------------------------------------------------------------------
# 2. What to write, and what to leave alone
# ---------------------------------------------------------------------------
#
# Origin pool: the repository root for the image, third_party for the firmware,
# sd/ for config.txt and cmdline.txt.  Staging (./sdcard) is deliberately not
# used: it is a scratch directory and may be older than the build.
BOOT_DIR="$REPO_ROOT/third_party/circle-stdlib/libs/circle/boot"

N_CHANGED=0
N_SAME=0
N_CREATED=0

# copy_if_different <origen> <destino> <descripción>
copy_if_different () {
	src="$1"
	dst="$2"
	what="$3"

	if [ ! -f "$src" ]; then
		return 0
	fi

	if [ -f "$dst" ]; then
		if cmp -s "$src" "$dst"; then
			step "igual" "$what"
			N_SAME=$((N_SAME + 1))
			return 0
		fi
		step "actualiza" "$what  ($(wc -c <"$dst") -> $(wc -c <"$src") bytes)"
		N_CHANGED=$((N_CHANGED + 1))
	else
		step "nuevo" "$what"
		N_CREATED=$((N_CREATED + 1))
	fi

	if [ "$DRY_RUN" -eq 1 ]; then
		return 0
	fi

	# Write to a temporary name in the same directory and rename over, so an
	# interrupted update leaves the card with either the old file or the new
	# one, never half of either.  FAT rename is atomic enough for this.
	tmp="$dst.nuevo"
	cp -f "$src" "$tmp"
	mv -f "$tmp" "$dst"
}

# copy_if_different_backup <origen> <destino> <descripción>
#
# Same, but keeps the previous version as <destino>.bak the first time it
# differs, and never replaces an existing backup: the first one is the version
# the card actually had before any of this.
copy_if_different_backup () {
	src="$1"
	dst="$2"
	what="$3"

	if [ -f "$dst" ] && ! cmp -s "$src" "$dst" && [ ! -f "$dst.bak" ]; then
		step "respaldo" "$what.original -> $(basename "$dst").bak"
		[ "$DRY_RUN" -eq 1 ] || cp -f "$dst" "$dst.bak"
	fi

	copy_if_different "$src" "$dst" "$what"
}

say "-- kernel y arranque"
for img in "$REPO_ROOT"/kernel*.img; do
	[ -f "$img" ] || continue
	copy_if_different "$img" "$SDCARD/$(basename "$img")" "$(basename "$img")"
done

if [ -f "$BOOT_DIR/armstub8-rpi4.bin" ]; then
	copy_if_different "$BOOT_DIR/armstub8-rpi4.bin" "$SDCARD/armstub8-rpi4.bin" "armstub8-rpi4.bin"
fi

say ""
say "-- configuración (se guarda una copia .bak la primera vez que cambia)"
copy_if_different_backup "$REPO_ROOT/sd/config.txt"  "$SDCARD/config.txt"  "config.txt"
copy_if_different_backup "$REPO_ROOT/sd/cmdline.txt" "$SDCARD/cmdline.txt" "cmdline.txt"

# boot.conf is the port's *own* settings (display mode, clock ceiling) and it is
# yours once it exists: the first copy comes from here with a .bak kept, and
# after that it is respected like dosbox.conf.  Same rule as the rest of the
# configuration - the port updates what it owns and leaves alone what you edit.
if [ -f "$SDCARD/boot.conf" ]; then
	copy_if_different_backup "$REPO_ROOT/sd/boot.conf" "$SDCARD/boot.conf" "boot.conf"
else
	copy_if_different "$REPO_ROOT/sd/boot.conf" "$SDCARD/boot.conf" "boot.conf"
fi

say ""
say "-- firmware (sólo si falta o es distinto)"
for f in bootcode.bin start4.elf fixup4.dat; do
	copy_if_different "$BOOT_DIR/$f" "$SDCARD/$f" "$f"
done
# Device trees: too many to name, and a new firmware may add one.
for dtb in "$BOOT_DIR"/bcm2711-rpi-4-b.dtb "$BOOT_DIR"/bcm2711-rpi-400.dtb \
	   "$BOOT_DIR"/bcm2711-rpi-cm4.dtb "$BOOT_DIR"/bcm2711-rpi-cm4s.dtb; do
	[ -f "$dtb" ] || continue
	copy_if_different "$dtb" "$SDCARD/$(basename "$dtb")" "$(basename "$dtb")"
done

# ---------------------------------------------------------------------------
# 3. Directories that must exist, and nothing else
# ---------------------------------------------------------------------------
#
# mkdir -p on a FAT card is safe: it does nothing if the directory is there.  It
# is the only operation this script performs that can touch an existing
# directory, and it never removes or renames anything inside one.

say ""
say "-- directorios (sólo se crean los que falten)"
for d in dos dosbox dosbox/system dosbox/saves dosbox/system/soundfonts; do
	if [ -d "$SDCARD/$d" ]; then
		step "existe" "$d/"
	else
		step "crea"   "$d/"
		[ "$DRY_RUN" -eq 1 ] || mkdir -p "$SDCARD/$d"
	fi
done

# dosbox.conf is not in the list above on purpose: it is configuration, it lives
# at dosbox/dosbox.conf, and it belongs to whoever set the mounts and cycles.
if [ -f "$SDCARD/dosbox/dosbox.conf" ]; then
	say ""
	step "respeta" "dosbox/dosbox.conf (tuyo; no se toca)"
elif [ -f "$REPO_ROOT/sd/dosbox.conf" ]; then
	say ""
	step "nuevo"   "dosbox/dosbox.conf (el de por defecto)"
	[ "$DRY_RUN" -eq 1 ] || cp "$REPO_ROOT/sd/dosbox.conf" "$SDCARD/dosbox/dosbox.conf"
fi

# ---------------------------------------------------------------------------
# 4. What was NOT touched, spelled out
# ---------------------------------------------------------------------------

say ""
n_games=0
if [ -d "$SDCARD/dos" ]; then
	n_games="$(find "$SDCARD/dos" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
fi
n_saves=0
if [ -d "$SDCARD/dosbox/saves" ]; then
	n_saves="$(find "$SDCARD/dosbox/saves" -mindepth 1 2>/dev/null | wc -l)"
fi

step "intacto" "dos/ ($n_games entradas)"
step "intacto" "dosbox/saves/ ($n_saves ficheros)"

say ""
if [ "$DRY_RUN" -eq 1 ]; then
	say "  PLAN       $N_CREATED nuevos, $N_CHANGED cambios, $N_SAME ya estaban bien"
	say "             nada escrito (--dry-run). Quita la opción para hacerlo."
else
	sync
	say "  HECHO      $N_CREATED nuevos, $N_CHANGED cambios, $N_SAME ya estaban bien"
	say "             hecho 'sync'; puedes sacar la tarjeta"
fi
