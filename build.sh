#!/usr/bin/env bash
#
# Build the PlayStation 2 port ("PS2 Doom Definitive Edition", codename ps2oom)
# inside the ps2dev Docker toolchain (see Dockerfile). The port lives in ps2/.
#
# OUTPUT — everything lands in ./bin/ (repo-local, gitignored), so it's the same
# place whether you build on WSL2, pure Linux, or Windows+Cygwin:
#   bin/ps2oom.elf          single-renderer ELF (plain builds / presets)
#   bin/DOOMSDL.ELF …       per-renderer ELFs (iso / fastiso)
#   bin/ps2oom.iso          bootable disc      (iso / fastiso)
#
# WADs — the iso/fastiso/freeiso targets graft WAD files from $WADDIR. Resolution order:
#   1. $PS2OOM_WADDIR (or legacy $WADDIR) if set
#   2. ~/Downloads/doom if it exists, else ./wads
# No game data is committed; drop your own WADs in whichever you use.
#
# DEPLOY (optional) — building an ISO works on any host with Docker (WSL2, pure
# Linux, Cygwin); the ISO always lands in bin/. ONLY if $PS2OOM_DEPLOY is set is
# the ISO also copied there (e.g. a Windows-visible /mnt/c folder for PCSX2).
# Nothing host-specific is committed. For a personal setup, make a gitignored
# wrapper (e.g. ./maykr.sh) that exports PS2OOM_WADDIR / PS2OOM_DEPLOY and execs
# this script -- so your paths stay out of git but cloners get clean defaults.
#
# Usage:
#   ./build.sh                  # bin/ps2oom.elf  (SDL2, no WAD baked in)
#   ./build.sh EMBED_WAD=1      # + embed ps2/DOOM1.WAD (shareware) fallback IWAD
#   ./build.sh stable [args]    # gsKit video + 480p + shareware WAD
#   ./build.sh gl [args]        # experimental ps2gl hardware world renderer
#   ./build.sh spumusic [args]  # default the Music row to the SPU2 synth
#   ./build.sh fastiso          # FAST disc: SDL2 launcher + gsKit hi-res, SIGIL ep.5
#   ./build.sh freeiso          # redistributable disc: shareware + Freedoom WADs
#   ./build.sh iso              # full disc: all three renderers + every WAD
#   ./build.sh clean            # remove build artifacts (incl. bin/)
#   ./build.sh shell            # interactive toolchain shell (cwd=ps2/)
#
# Raw make args still work, e.g.:  ./build.sh GSKIT_VIDEO=1 EMBED_WAD=1
# NB: switching video backend needs a `clean` first (make doesn't track CFLAGS).
set -euo pipefail

IMAGE="ps2dock:local"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${HERE}/bin"
mkdir -p "${BIN}"

# ---- helpers --------------------------------------------------------------

# Where the iso/fastiso/freeiso targets look for WAD files (overridable; see header).
resolve_waddir() {
  local pref="${PS2OOM_WADDIR:-${WADDIR:-}}"
  if [ -n "$pref" ]; then printf '%s\n' "$pref"; return; fi
  [ -d "${HOME}/Downloads/doom" ] && { printf '%s\n' "${HOME}/Downloads/doom"; return; }
  printf '%s\n' "${HERE}/wads"
}

# Mirror the freshly built ps2/ps2oom.elf into bin/ (plain builds / presets).
sync_elf() {
  if [ -f "${HERE}/ps2/ps2oom.elf" ]; then
    cp -f "${HERE}/ps2/ps2oom.elf" "${BIN}/ps2oom.elf"
    echo "ELF -> ${BIN}/ps2oom.elf  ($(du -h "${BIN}/ps2oom.elf" | cut -f1))"
  fi
}

# The ISO always lands in bin/ (any host). ONLY if PS2OOM_DEPLOY is set (e.g. by
# your gitignored maykr.sh wrapper) is it also copied there -- e.g. a Windows-
# visible /mnt/c folder for PCSX2. Fresh clones don't set it, so they never copy.
deploy_iso() {
  local iso="${BIN}/ps2oom.iso"
  [ -f "$iso" ] || return 0
  echo "ISO -> ${iso}  ($(du -h "$iso" | cut -f1))"
  [ -n "${PS2OOM_DEPLOY:-}" ] || return 0
  if ! mkdir -p "${PS2OOM_DEPLOY}" 2>/dev/null; then
    echo "!! deploy: cannot create ${PS2OOM_DEPLOY} (ISO is still in bin/)" >&2
    return 0
  fi
  if cp -f "$iso" "${PS2OOM_DEPLOY}/ps2oom.iso" 2>/dev/null; then
    echo ">> deployed -> ${PS2OOM_DEPLOY}/ps2oom.iso  (open this in PCSX2)"
  else
    echo "!! deploy FAILED: could not overwrite ${PS2OOM_DEPLOY}/ps2oom.iso" >&2
    echo "!! it's probably locked by a running PCSX2 -- close PCSX2 and re-run," >&2
    echo "!! or just boot bin/ps2oom.iso directly. The fresh ISO IS in bin/." >&2
  fi
}

# Build ONE renderer config into its own object dir + ELF, then mirror the ELF
# into bin/. Each config keeps a separate build dir so the SAME sources compiled
# with different -D flags don't collide -- which means NO `make clean` between
# configs, so unchanged files are skipped (incremental).
#   build_cfg <objdir> <elf> <make-args...>
build_cfg() {
  local objdir="$1" elf="$2"; shift 2
  echo ">> building ${elf} [${objdir}] ${*:+($*)} ..."
  docker run "${common[@]}" -e OBJDIR="$objdir" -e ELF="$elf" -e A="$*" "${IMAGE}" bash -c '
    set -e
    make EE_OBJDIR="$OBJDIR" EE_BIN="$ELF" $A
    mkdir -p /work/bin && cp -f "$ELF" /work/bin/
  '
}

# Build the SDL2 launcher + gsKit hi-res ELFs (the 2-renderer "fast" disc),
# incrementally (separate dirs, no clean).
fast_build_elfs() {
  build_cfg build-sdl DOOMSDL.ELF "$@"
  build_cfg build-gs  DOOMGS.ELF  GSKIT_VIDEO=1 GS480P=1 HIRES=1 "$@"
}

# Pack a 2-renderer disc (boots the SDL2 launcher; Render row -> gsKit) with a
# given volume label and WAD list, into bin/ps2oom.iso, then deploy.
#   pack_fast_iso <waddir> <volume-label> <wad> [wad ...]
pack_fast_iso() {
  local waddir="$1" vol="$2"; shift 2
  echo ">> packing ISO -> bin/ps2oom.iso  (WADs: ${waddir}) ..."
  docker run "${common[@]}" -v "${waddir}:/wads:ro" -e WADS="$*" -e VOL="$vol" "${IMAGE}" bash -c '
    set -e
    GRAFT="SYSTEM.CNF=/work/ps2/SYSTEM.CNF DOOMSDL.ELF=/work/ps2/DOOMSDL.ELF DOOMGS.ELF=/work/ps2/DOOMGS.ELF"
    for w in $WADS; do
      f=$(ls "/wads/$w" 2>/dev/null | head -1)
      [ -n "$f" ] && b=$(basename "$f" | tr "[:lower:]" "[:upper:]") && GRAFT="$GRAFT $b=$f" && echo "  + $b" || echo "  ! missing: $w"
    done
    mkdir -p /work/bin
    mkisofs -quiet -graft-points -l -V "$VOL" -o /work/bin/ps2oom.iso $GRAFT
  '
  deploy_iso
}

usage() {
  sed -n '3,/^set -euo/p' "$0" | sed 's/^#\{0,1\} \{0,1\}//; /^set -euo/d'
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
  clean)
    shift
    docker run "${common[@]}" "${IMAGE}" make clean "$@" || true
    rm -f "${BIN}"/*.elf "${BIN}"/*.ELF "${BIN}"/*.iso 2>/dev/null || true
    echo ">> cleaned build/ and bin/"
    exit 0
    ;;
  stable|gskit)
    shift
    docker run "${common[@]}" "${IMAGE}" make GSKIT_VIDEO=1 GS480P=1 EMBED_WAD=1 "$@"
    sync_elf
    exit 0
    ;;
  gl)
    shift
    docker run "${common[@]}" "${IMAGE}" make GL_VIDEO=1 EMBED_WAD=1 "$@"
    sync_elf
    exit 0
    ;;
  spumusic)
    # Native SPU2 hardware-voice synth for music (i_spu2music.c) instead of the
    # default OPL2 FM engine. SFX stay on audsrv either way.
    shift
    docker run "${common[@]}" "${IMAGE}" make SPU_MUSIC=1 EMBED_WAD=1 "$@"
    sync_elf
    exit 0
    ;;
  fastiso|gsiso)
    # FAST ITERATION disc: SDL2 launcher + gsKit hi-res only (2 compiles, small
    # pack) for testing the hi-res build. Packs DOOM.WAD + SIGIL.wad so the 5th
    # episode is right there. ('gsiso' kept as an alias.)
    #   ./build.sh fastiso
    shift
    WADDIR="$(resolve_waddir)"
    fast_build_elfs "$*"
    pack_fast_iso "${WADDIR}" SIGIL DOOM.WAD SIGIL.wad
    exit 0
    ;;
  freeiso)
    # 100% FOSS disc: SDL2 launcher + gsKit hi-res, packing only the BSD-licensed
    # Freedoom IWADs (Phase 1, Phase 2, FreeDM). No shareware or commercial data,
    # so the whole disc is freely redistributable. Same 2-renderer layout as
    # fastiso.   ./build.sh freeiso
    shift
    WADDIR="$(resolve_waddir)"
    fast_build_elfs "$*"
    pack_fast_iso "${WADDIR}" FREEDOOM freedoom1.wad freedoom2.wad freedm.wad
    exit 0
    ;;
  iso)
    # Build ALL THREE renderer ELFs (SDL2 -> DOOMSDL.ELF, gsKit -> DOOMGS.ELF,
    # GL -> DOOMGL.ELF) and pack them + every WAD into a bootable disc. Boots
    # DOOMSDL.ELF; the menu's "Render" row LoadExec's the others.
    #   ./build.sh iso
    shift
    WADDIR="$(resolve_waddir)"
    echo ">> building all three renderer ELFs (SDL2 + gsKit + GL) ${*:+[$*]} ..."
    build_cfg build-sdl DOOMSDL.ELF "$@"                          # SDL2 software 320x200
    build_cfg build-gs  DOOMGS.ELF  GSKIT_VIDEO=1 GS480P=1 HIRES=1 "$@"  # gsKit hi-res 640x400
    build_cfg build-gl  DOOMGL.ELF  GL_VIDEO=1 "$@"               # GL hardware geometry
    echo ">> packing ISO -> bin/ps2oom.iso  (WADs: ${WADDIR}) ..."
    docker run "${common[@]}" -v "${WADDIR}:/wads:ro" "${IMAGE}" bash -c '
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
      mkdir -p /work/bin
      mkisofs -quiet -graft-points -l -V DOOM -o /work/bin/ps2oom.iso $GRAFT
    '
    deploy_iso "${WADDIR}"
    exit 0
    ;;
esac

# Everything else is passed straight to make (default target = ps2oom.elf).
docker run "${common[@]}" "${IMAGE}" make "$@"
sync_elf
