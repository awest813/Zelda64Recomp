#!/usr/bin/env bash
#
# Build a bootable Dreamcast CDI disc image for Zelda 64: Recompiled.
#
# Uses mkdcdisc (https://gitlab.com/simulant/mkdcdisc), which takes the
# *unscrambled* ELF produced by the Dreamcast CMake build and handles IP.BIN
# generation and 1ST_READ.BIN scrambling internally. The game expects its ROM
# at the disc root as rom.z64 (see DC_ROM_PATH in include/dreamcast_platform.h).
#
# Usage:
#   tools/dreamcast/make_disc.sh \
#       -e build-dreamcast/Zelda64Recompiled \
#       -r /path/to/mm.us.rev1.z64 \
#       -o zelda64recomp.cdi \
#       [-a /path/to/assets-dir] [-n "ZELDA64 RECOMP"]
#
set -euo pipefail

usage() {
    grep '^#' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 1
}

elf=""
rom=""
out="zelda64recomp.cdi"
name="ZELDA64 RECOMP"
assets=""

while getopts "e:r:o:n:a:h" opt; do
    case "$opt" in
        e) elf="$OPTARG" ;;
        r) rom="$OPTARG" ;;
        o) out="$OPTARG" ;;
        n) name="$OPTARG" ;;
        a) assets="$OPTARG" ;;
        *) usage ;;
    esac
done

[[ -n "$elf" && -n "$rom" ]] || usage

if ! command -v mkdcdisc >/dev/null 2>&1; then
    echo "error: mkdcdisc not found on PATH." >&2
    echo "       Build it from https://gitlab.com/simulant/mkdcdisc" >&2
    exit 1
fi

[[ -f "$elf" ]] || { echo "error: ELF not found: $elf" >&2; exit 1; }
[[ -f "$rom" ]] || { echo "error: ROM not found: $rom" >&2; exit 1; }

# The post-build step also emits a raw 1ST_READ.BIN next to the ELF; make sure
# the caller passed the ELF (mkdcdisc scrambles it itself).
if ! head -c 4 "$elf" | cmp -s - <(printf '\x7fELF'); then
    echo "error: $elf is not an ELF file." >&2
    echo "       Pass the Zelda64Recompiled ELF, not 1ST_READ.BIN" >&2
    echo "       (mkdcdisc performs the scrambling itself)." >&2
    exit 1
fi

# Stage the disc filesystem: the game looks for /cd/rom.z64 (and optionally
# /cd/assets/).
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

cp "$rom" "$stage/rom.z64"
if [[ -n "$assets" ]]; then
    [[ -d "$assets" ]] || { echo "error: assets dir not found: $assets" >&2; exit 1; }
    mkdir -p "$stage/assets"
    cp -r "$assets"/. "$stage/assets/"
fi

echo "Building $out ..."
mkdcdisc -e "$elf" -d "$stage" -n "$name" -o "$out"

echo
echo "Done: $out"
echo "Boot it in an emulator (Flycast/lxdream) or burn it for a real console."
