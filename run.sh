#!/usr/bin/env bash
#
# Launch + debug a PS2 build in Windows PCSX2, from WSL.
#
# Boots the disc/ELF with -fastboot, writes PCSX2's log to the WAD folder, then
# live-tails it filtered to the lines that actually matter (the IOP console:
# disc/WAD scan, audsrv bring-up, video mode, pad, EE exceptions, and the
# `# Restart.` LoadExec markers). NB: our EE printf goes to the on-screen GS
# console, NOT this log -- the log is the IOP side + EE exceptions.
#
# Usage:
#   ./run.sh                      # boot ps2oom.iso (from `./build.sh iso/fastiso`) + tail log
#   ./run.sh bin/ps2oom.elf       # boot a specific ELF (copied to the PCSX2 folder)
#   ./run.sh path/to/foo.iso      # boot a specific ISO
#   ./run.sh --log                # don't launch; just summarise the existing log
#   ./run.sh -h | --help
#
# Override paths via env: PCSX2=... OUTDIR=...
set -uo pipefail

PCSX2="${PCSX2:-/mnt/c/Program Files/PCSX2/pcsx2-qt.exe}"
OUTDIR="${OUTDIR:-/mnt/c/Users/azama/Downloads/doom}"   # Windows-visible; WADs live here
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$OUTDIR/pcsx2.log"

# Lines worth seeing while debugging (IOP console + EE exceptions).
PATTERNS='cdvd|iso|SYSTEM\.CNF|cdrom0|DOOMSDL|\.ELF|cdfs:/|IWAD|PWAD|FREESD|libsd|audsrv|mc:|Mode Changed|GS CRTC|target speed|Pad:|# Restart|exception|tlb|address error|DOOM ERROR|EE'

usage() { sed -n '2,/^set /p' "$0" | sed 's/^# \{0,1\}//; /^set /d'; }

show_summary() {
  if [[ ! -f "$LOG" ]]; then echo "no log yet at $LOG"; exit 1; fi
  echo ">> summary of $LOG"
  grep -inE "$PATTERNS" "$LOG" | tail -40
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --log)     show_summary; exit 0 ;;
esac

# Resolve the boot target: default to the deployed ISO. If it's missing but the
# repo-local bin/ps2oom.iso exists (e.g. PS2OOM_DEPLOY wasn't set), copy it into
# the Windows-visible OUTDIR so PCSX2 can open it.
TARGET="${1:-$OUTDIR/ps2oom.iso}"
if [[ -z "${1:-}" && ! -e "$TARGET" && -f "$HERE/bin/ps2oom.iso" ]]; then
  cp -f "$HERE/bin/ps2oom.iso" "$TARGET" && echo ">> copied bin/ps2oom.iso -> $TARGET"
fi
if [[ ! -e "$TARGET" ]]; then
  echo "!! not found: $TARGET"
  echo "   build one first:  ./build.sh fastiso   (or pass an .elf/.iso path)"
  exit 1
fi
[[ -x "$PCSX2" ]] || { echo "!! PCSX2 not found at: $PCSX2 (set PCSX2=...)"; exit 1; }

# PCSX2 is a Windows process and can't read WSL paths -- copy an ELF that lives
# outside the Windows-visible OUTDIR into it first. (ISOs are already there.)
case "$TARGET" in
  *.elf|*.ELF)
    if [[ "$TARGET" != "$OUTDIR"/* ]]; then
      cp -f "$TARGET" "$OUTDIR/" && echo ">> copied $(basename "$TARGET") -> $OUTDIR/"
      TARGET="$OUTDIR/$(basename "$TARGET")"
    fi
    BOOTARGS=(-elf "$(wslpath -w "$TARGET")")
    ;;
  *)
    BOOTARGS=("$(wslpath -w "$TARGET")")   # ISO / disc image = positional
    ;;
esac

# Don't fight an existing instance (PCSX2 single-instance + shared MC/settings).
if tasklist.exe /FI "IMAGENAME eq pcsx2-qt.exe" /NH 2>/dev/null | grep -qi pcsx2; then
  echo "!! PCSX2 is already running -- close it first (this script won't kill it)."
  exit 1
fi

LOG_WIN="$(wslpath -w "$LOG")"
: > "$LOG"   # truncate so the tail below shows only this run

echo ">> booting: $TARGET"
echo ">> log:     $LOG"
nohup "$PCSX2" -fastboot -logfile "$LOG_WIN" "${BOOTARGS[@]}" >/dev/null 2>&1 &
disown

echo ">> following the log (Ctrl-C to stop; PCSX2 keeps running). Filtered lines:"
echo "------------------------------------------------------------------------"
# Wait for PCSX2 to (re)create the file, then follow it filtered.
for _ in $(seq 1 50); do [[ -s "$LOG" ]] && break; sleep 0.2; done
exec tail -n +1 -F "$LOG" 2>/dev/null | grep --line-buffered -iE "$PATTERNS"
