#!/usr/bin/env bash
#
# Build the PlayStation 2 port of doomgeneric inside the ps2dev Docker
# toolchain (see Dockerfile). The port itself lives in ps2/.
#
# Usage:
#   ./build.sh                  # build ps2/ps2oom.elf  (no WAD baked in;
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
# Artifacts (objects in ps2/build/, the ELF as ps2/ps2oom.elf) are owned
# by your host user, not root.
set -euo pipefail

IMAGE="ps2dock:local"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
  cat <<'EOF'
ps2oom build — DOOM for the PlayStation 2 (builds in the ps2dev Docker image).

Usage: ./build.sh <preset|make-args>

Presets:
  stable          gsKit video build (GSKIT_VIDEO=1 GS480P=1 EMBED_WAD=1)
  gl              experimental native VU1+DMA hardware renderer (GL_VIDEO=1 EMBED_WAD=1)
  spumusic        default the menu's Music row to the SPU2 synth (SPU_MUSIC=1 EMBED_WAD=1)
  iso             build ALL THREE renderer ELFs (DOOMSDL/DOOMGS/DOOMGL) and pack
                  them + every WAD in the WAD folder into a bootable doom.iso
  clean           remove build artifacts
  shell           interactive shell inside the toolchain (cwd = ps2/)

Raw make flags also work, e.g.:
  ./build.sh                     (no preset) -> just build ps2/ps2oom.elf (SDL2, no WAD)
  ./build.sh EMBED_WAD=1         build + embed shareware DOOM1.WAD
  ./build.sh GSKIT_VIDEO=1 GS480P=1 EMBED_WAD=1
  ./build.sh GL_VIDEO=1 EMBED_WAD=1

Notes:
  - Renderer / music / video mode are chosen at RUNTIME on the setup menu;
    the build flags only set the defaults a given ELF starts with.
  - Switching video backend (e.g. gl <-> stable) needs a `clean` first --
    make does not track CFLAGS changes.
  - To run + debug the result in Windows PCSX2 from WSL:  ./run.sh   (see run.sh -h)

Most builds output ps2/ps2oom.elf; `iso` outputs doom.iso in the WAD folder.
EOF
}

# No args (or an explicit help request): show usage instead of silently building.
case "${1:-}" in
  ""|-h|--help|help|usage) usage; exit 0 ;;
esac

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
  gsiso)
    # FAST ITERATION: build ONLY the gsKit hi-res renderer and pack a small ISO
    # that boots straight to it, with just a few test WADs -- ~1 compile + a
    # ~45 MB pack instead of the full 'iso' (3 compiles + ~400 MB). No renderer
    # switching (gsKit only). Use the full 'iso' for the shippable disc.
    #   ./build.sh gsiso
    shift
    WADDIR="/mnt/c/Users/azama/Downloads/doom"
    echo ">> building SDL2 launcher + gsKit hi-res ELFs ..."
    docker run "${common[@]}" "${IMAGE}" bash -c '
      set -e
      make clean >/dev/null
      make "'"$*"'"                                 # SDL2 launcher / 320 (boot ELF)
      cp ps2oom.elf DOOMSDL.ELF
      make clean >/dev/null
      make GSKIT_VIDEO=1 GS480P=1 HIRES=1 "'"$*"'"   # gsKit hi-res 640x400
      cp ps2oom.elf DOOMGS.ELF
    '
    echo ">> packing minimal ISO ..."
    exec docker run "${common[@]}" -v "${WADDIR}:/wads" "${IMAGE}" bash -c '
      set -e
      # Boots the SDL2 launcher (the setup menu); the Render row switches to
      # gsKit. (GL omitted from the fast disc -- dont pick it here.)
      GRAFT="SYSTEM.CNF=/work/ps2/SYSTEM.CNF DOOMSDL.ELF=/work/ps2/DOOMSDL.ELF DOOMGS.ELF=/work/ps2/DOOMGS.ELF"
      for w in DOOM.WAD DOOM1.WAD DOOM2.WAD SIGIL_COMPAT NUTS; do
        f=$(ls /wads/$w /wads/$w.WAD /wads/$w.wad 2>/dev/null | head -1)
        [ -n "$f" ] && b=$(basename "$f" | tr "[:lower:]" "[:upper:]") && GRAFT="$GRAFT $b=$f" && echo "  + $b"
      done
      mkisofs -quiet -graft-points -l -V DOOMGS -o /wads/doom.iso $GRAFT
      echo "ISO -> /wads/doom.iso  ($(du -h /wads/doom.iso | cut -f1))"
    '
    ;;
  iso)
    # Build BOTH renderer ELFs (SDL2 -> DOOMSDL.ELF, gsKit -> DOOMGS.ELF) and
    # pack them + ALL WADs into a bootable PS2 ISO. Boots DOOMSDL.ELF; the setup
    # menu's "Render" row LoadExec's the other ELF. Each WAD is placed on the
    # disc under its UPPERCASE name so the cdfs WAD picker finds it.
    #   ./build.sh iso
    shift
    WADDIR="/mnt/c/Users/azama/Downloads/doom"
    EXTRA="$*"                                 # forwarded to every renderer build (e.g. HIRES=1)
    echo ">> building all three renderer ELFs (SDL2 + gsKit + GL) ${EXTRA:+[$EXTRA]} ..."
    docker run "${common[@]}" -e EXTRA="$EXTRA" "${IMAGE}" bash -c '
      set -e
      make clean >/dev/null
      make $EXTRA                              # SDL2 (software) -- 320x200 (full speed)
      cp ps2oom.elf DOOMSDL.ELF
      make clean >/dev/null
      make GSKIT_VIDEO=1 GS480P=1 HIRES=1 $EXTRA   # gsKit (software, 480p) -- 640x400 hi-res default
      cp ps2oom.elf DOOMGS.ELF
      make clean >/dev/null
      make GL_VIDEO=1 $EXTRA                    # GL (hardware geometry) -- 320x200
      cp ps2oom.elf DOOMGL.ELF
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

# Everything else is passed straight to make (default target = ps2oom.elf).
exec docker run "${common[@]}" "${IMAGE}" make "$@"
