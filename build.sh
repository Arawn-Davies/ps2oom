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
    # Build BOTH renderer ELFs (SDL2 -> DOOMSDL.ELF, gsKit -> DOOMGS.ELF) and
    # pack them + ALL WADs into a bootable PS2 ISO. Boots DOOMSDL.ELF; the setup
    # menu's "Render" row LoadExec's the other ELF. Each WAD is placed on the
    # disc under its UPPERCASE name so the cdfs WAD picker finds it.
    #   ./build.sh iso
    shift
    WADDIR="/mnt/c/Users/azama/Downloads/doom"
    echo ">> building all three renderer ELFs (SDL2 + gsKit + GL) ..."
    docker run "${common[@]}" "${IMAGE}" bash -c '
      set -e
      make clean >/dev/null
      make                                    # SDL2 (software)
      cp doomgeneric.elf DOOMSDL.ELF
      make clean >/dev/null
      make GSKIT_VIDEO=1 GS480P=1             # gsKit (software, 480p)
      cp doomgeneric.elf DOOMGS.ELF
      make clean >/dev/null
      make GL_VIDEO=1                          # GL (hardware geometry)
      cp doomgeneric.elf DOOMGL.ELF
    '
    echo ">> packing ISO ..."
    exec docker run "${common[@]}" -v "${WADDIR}:/wads" "${IMAGE}" bash -c '
      set -e
      # graft-points map files straight into the ISO (no big cp). ISO level 2
      # (-l) allows 8+ char names like FREEDOOM1.WAD.
      GRAFT="SYSTEM.CNF=/work/ps2/SYSTEM.CNF"
      GRAFT="$GRAFT DOOMSDL.ELF=/work/ps2/DOOMSDL.ELF DOOMGS.ELF=/work/ps2/DOOMGS.ELF DOOMGL.ELF=/work/ps2/DOOMGL.ELF"
      for w in $(ls /wads/*.wad /wads/*.WAD 2>/dev/null | sort -u); do
        b=$(basename "$w" | tr "[:lower:]" "[:upper:]")
        GRAFT="$GRAFT $b=$w"
        echo "  + $b"
      done
      mkisofs -quiet -graft-points -l -V DOOM -o /wads/doom.iso $GRAFT
      echo "ISO -> /wads/doom.iso  ($(du -h /wads/doom.iso | cut -f1))"
    '
    ;;
esac

# Everything else is passed straight to make (default target = doomgeneric.elf).
exec docker run "${common[@]}" "${IMAGE}" make "$@"
