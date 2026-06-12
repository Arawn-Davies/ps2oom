#!/usr/bin/env bash
#
# Build the PlayStation 2 port of doomgeneric inside the ps2dev Docker
# toolchain (see Dockerfile). The port itself lives in ps2/.
#
# Usage:
#   ./build.sh                  # build ps2/doomgeneric.elf  (no WAD baked in;
#                               #   supply a WAD at runtime, e.g. via hostfs)
#   ./build.sh EMBED_WAD=1      # also embed ps2/DOOM1.WAD (shareware) as a
#                               #   built-in fallback IWAD -- for convenience
#   ./build.sh stable [args]    # tried-and-true build: native gsKit video + 480p
#                               #   + shareware WAD (GSKIT_VIDEO=1 GS480P=1 EMBED_WAD=1)
#   ./build.sh gl [args]        # experimental ps2gl hardware world renderer
#                               #   (GL_VIDEO=1 EMBED_WAD=1)
#   ./build.sh clean            # remove build artifacts
#   ./build.sh shell            # interactive shell inside the toolchain (cwd=ps2/)
#
# Raw make args still work directly, e.g.:  ./build.sh GSKIT_VIDEO=1 EMBED_WAD=1
# NB: switching video backend (gl <-> stable) needs a `clean` first -- make does
# not track CFLAGS changes.
#
# Artifacts (objects in ps2/build/, the ELF as ps2/doomgeneric.elf) are owned
# by your host user, not root.
set -euo pipefail

IMAGE="ps2dock:local"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build the local image (ps2dev + make/bash) on first use.
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
  echo ">> building ${IMAGE} ..."
  docker build -t "${IMAGE}" "${HERE}"
fi

# Mount the repo at /work and run in ps2/ as the host user.
common=(--rm -u "$(id -u):$(id -g)" -v "${HERE}:/work" -w /work/ps2)

if [[ "${1:-}" == "shell" ]]; then
  exec docker run -it "${common[@]}" "${IMAGE}" /bin/bash
fi

# Named presets: the tried-and-true gsKit build vs the experimental GL renderer.
# Extra args after the preset are appended (e.g. `./build.sh gl clean`).
case "${1:-}" in
  stable|gskit)
    shift
    exec docker run "${common[@]}" "${IMAGE}" make GSKIT_VIDEO=1 GS480P=1 EMBED_WAD=1 "$@"
    ;;
  gl)
    shift
    exec docker run "${common[@]}" "${IMAGE}" make GL_VIDEO=1 EMBED_WAD=1 "$@"
    ;;
  spumusic)
    # Native SPU2 hardware-voice synth for music (i_spu2music.c) instead of the
    # default OPL2 FM engine. SFX stay on audsrv either way. Omit this preset
    # (e.g. `./build.sh EMBED_WAD=1`) for the original OPL music + audsrv SFX.
    shift
    exec docker run "${common[@]}" "${IMAGE}" make SPU_MUSIC=1 EMBED_WAD=1 "$@"
    ;;
  iso)
    # Pack the CURRENT ps2/doomgeneric.elf + a WAD into a bootable PS2 ISO.
    #   ./build.sh stable && ./build.sh iso [wadname]   (default freedoom1.wad)
    # The WAD comes from the user's WAD folder and is placed on the disc as
    # DOOM.WAD; the ELF reads it on demand via cdfs (see ps2_cdfs.c).
    shift
    WAD="${1:-freedoom1.wad}"
    WADDIR="/mnt/c/Users/azama/Downloads/doom"
    if [[ ! -f "${HERE}/ps2/doomgeneric.elf" ]]; then
      echo "no ps2/doomgeneric.elf -- build one first (e.g. ./build.sh stable)"; exit 1
    fi
    exec docker run "${common[@]}" -e "WAD=${WAD}" -v "${WADDIR}:/wads" "${IMAGE}" bash -c '
      set -e
      rm -rf /tmp/iso && mkdir -p /tmp/iso
      cp /work/ps2/SYSTEM.CNF      /tmp/iso/SYSTEM.CNF
      cp /work/ps2/doomgeneric.elf /tmp/iso/DOOMGEN.ELF
      cp "/wads/$WAD"              /tmp/iso/DOOM.WAD
      mkisofs -quiet -l -V DOOM -o /wads/doom.iso /tmp/iso
      echo "ISO -> /wads/doom.iso  (boot=DOOMGEN.ELF, WAD=$WAD as DOOM.WAD)"
    '
    ;;
esac

# Everything else is passed straight to make (default target = doomgeneric.elf).
exec docker run "${common[@]}" "${IMAGE}" make "$@"
