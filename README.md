# PS2 Doom Definitive Edition
#### *(codename **ps2oom**)*

A native PlayStation 2 port of DOOM: full speed, hardware video, native SPU2
audio, dual-analog controller support, a controller-driven setup menu, **two
selectable music engines**, **three selectable video renderers** (incl. a
**640×400 hi-res** software mode), optional **jump**, and limit-removing
rendering so big PWADs (e.g. SIGIL) play.

Specialised for the PS2, built from
[doomgeneric](https://github.com/ozkl/doomgeneric) (and through it
[Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) and
id Software's original DOOM source).

## Features

- **Setup menu at boot** (controller-driven): pick **IWAD**, **PWAD**,
  **Music** engine, **Render** backend, **Video** mode, and **Jump** on one page.
  **✕ confirms, ○ cancels.**
- **Three video renderers**, switchable at runtime from the menu (each is its
  own ELF on the disc; choosing one hands off to it):
  - **SDL2** (software, **320×200**) — the default boot renderer; 32-bit
    framebuffer, so it stays at native res for full speed.
  - **gsKit** (software, **640×400 hi-res**) — Doom's 8-bit framebuffer as a
    PSMT8 texture + CLUT; the GS does the palette expand and upscale in hardware.
    The 8-bit framebuffer is cheap enough to render the world at a true 2× res.
  - **GL** (experimental) — a hand-rolled VU1 + DMA hardware world renderer.
- **640×400 hi-res** (gsKit) — true double-resolution world rendering (sky,
  flats, sprites, HUD all scaled), plus a 640×400 title screen, while keeping
  full speed. SDL2 stays 320×200. The GS hardware upscales the 640×400
  framebuffer to fill a 4:3 480p screen, so 640×400 (the clean 2× of 320×200)
  is the render target, not 640×480.
- **Fullscreen overlay HUD** (gsKit) — Crispy/ZDoom-style corner stats (health,
  armor, ready-weapon ammo, keys) drawn crisp over a borderless view, instead of
  the classic status bar. SDL2 keeps the classic bar.
- **Hi-res menus** (gsKit) — the setup/options menus render as crisp scaled
  text + sliders rather than the chunky 2×-scaled menu graphics.
- **5th episode / SIGIL** — full **SIGIL.WAD** plays as episode 5 (SKY5, D_E5M*
  music, 5th menu entry). Episode names and the episode-end story text are read
  from the WAD's **UMAPINFO** (nothing PWAD-specific is hardcoded).
- **Six GS output modes** (gsKit renderer): NTSC 480i (default, composite-safe),
  NTSC 480p, PAL 576i, PAL 576p, 720p *(experimental)*, 1080i *(experimental)*.
- **Optional jump** (off by default = vanilla; toggle on the setup menu) on the
  **△** button — for maps that want it, à la (G)ZDoom.
- **Native audsrv audio** — sound effects mixed on the EE and streamed to the
  SPU2's PCM source (no SDL audio).
- **Two music engines**, switchable at runtime from the menu:
  - **OPL / FM (DBOPL)** — AdLib-style synthesis from the IWAD's GENMIDI lump,
    mixed into the audsrv stream. *Default.*
  - **SPU2 hardware-voice synth** — the MIDI is parsed/flattened on the EE,
    shipped to an IOP module over SIF RPC, and sequenced onto the SPU2's own
    hardware ADPCM voices. See below.
- **Limit-removing** — the vanilla static renderer limits (visplanes, drawsegs,
  vissprites, …) are raised, so detailed / large maps such as **SIGIL** play
  instead of erroring out. (Boom/MBF map features are still unsupported.)
- **Dual-analog controller** (libpad): modern layout, proportional analog turn,
  plus an **in-game Controller options page** (turn sensitivity, always-run,
  deadzone, invert look, swap sticks) — **saved to the memory card** (failsafe:
  no card → settings just stay for the session).
- **Flexible WAD loading** — IWAD + optional PWAD from a cdfs disc/ISO, from
  hostfs (PCSX2 `host:`), or the embedded shareware DOOM1.WAD; read on demand.
- **60 fps frame cap**; **quit-to-DOS returns to the setup menu** instead of the
  PS2 BIOS; fatal errors are shown on-screen rather than rebooting silently.
- **Fast boot** (~4 s to gameplay). The previous ~30 s stall was a
  libps2_drivers `waitUntilDeviceIsReady` device-probe timeout, now overridden.

## How it flows

The disc boots to a setup menu; the **Render** row hands off to the matching
renderer ELF, and **Quit to DOS** comes back to the menu (never the BIOS).
Controller settings round-trip through the memory card.

```mermaid
flowchart LR
  BOOT(["power on"]) --> MENU{"setup menu<br/>IWAD · PWAD · Music<br/>Render · Video · Jump"}
  MC[("memory card")] -. controller settings .-> MENU
  MENU --> SDL["DOOM — SDL2<br/>320×200"]
  MENU --> GS["DOOM — gsKit<br/>640×400 hi-res"]
  MENU --> GL["DOOM — GL<br/>(experimental)"]
  SDL -- "Quit to DOS" --> MENU
  GS  -- "Quit to DOS" --> MENU
  GS  -. "save settings" .-> MC
```

## Controls

Dual-analog, Xbox-style confirm (**✕** = the bottom face button):

| Input | Action |
|---|---|
| **Left stick** | move / strafe |
| **Right stick** | turn (proportional; sensitivity is adjustable) |
| **R2** | fire |
| **✕ / □** | use / open · ✕ also **confirms** menus & prompts |
| **○** | **cancel** / open-close menu (Escape) |
| **L2** | run (hold) |
| **L1 / R1** | previous / next weapon (cycles owned weapons) |
| **△** | jump *(if enabled on the setup menu)* |
| **Select** | automap |
| **Start** | menu |
| **D-pad** | menu navigation (also digital move/turn) |

Tune the sticks in-game under **Options → Controller** (turn sensitivity,
always-run, deadzone, invert look, swap sticks); changes persist to the memory
card on quit.

## The SPU2 hardware-voice synth (experimental)

Instead of software-rendering FM into the audio stream, this engine drives the
SPU2's **48 hardware ADPCM voices** as a sample-based MIDI synthesiser — the most
"native" way to make music on the PS2.

```mermaid
flowchart LR
  WAD["IWAD MUS/MIDI"] --> EE["EE: midifile.c parse<br/>flatten tracks + tempo"]
  EE -- "SIF RPC: timed events" --> SEQ["IOP: spusynth.irx<br/>sequencer thread"]
  SEQ --> VOICES["SPU2 Core-1<br/>ADPCM voice pool (20)"]
  VOICES --> DAC(["SPU2 DAC → speakers"])
```

The EE parses the song (MUS→MIDI in memory), flattens every track in
absolute-tick order with tempo applied, and ships timed events to the IOP, where
a sequencer thread plays them onto a 20-voice pool using a synthesised
General-MIDI waveform bank (square/saw/triangle/sine/pulse + a noise sample for
drums) and a GM-family patch map. The result is the DOOM soundtrack on actual
SPU2 voices — a chiptune-ish rendering (synthesised waveforms, not recorded
instruments). It coexists with audsrv (which powers the chip up).

Both music engines are always built in; the **Music** row on the setup menu
picks one at runtime.

## Building

Everything builds in the official ps2dev toolchain through Docker (the ps2dock
image — see the root `Dockerfile`):

All build output lands in **`bin/`** (git-ignored) — the same place on WSL2,
pure Linux, or Cygwin. ISO building needs only Docker; nothing is host-specific.

```sh
./build.sh                       # bin/ps2oom.elf (SDL2, no WAD baked in)
./build.sh EMBED_WAD=1           # + embed shareware DOOM1.WAD
./build.sh stable                # gsKit video, 480p default, embedded WAD
./build.sh gl                    # experimental GL hardware world renderer
./build.sh spumusic              # default the menu's Music row to the SPU2 synth
./build.sh iso                   # ALL THREE renderer ELFs (SDL2/gsKit/GL) + every
                                 #   WAD -> bootable bin/ps2oom.iso (Render row switches)
./build.sh fastiso               # FAST disc: SDL2 launcher + gsKit hi-res, DOOM.WAD
                                 #   + SIGIL.wad (episode 5)
./build.sh freeiso               # 100% FOSS disc: Freedoom IWADs only (no game data)
./build.sh clean                 # remove build artifacts (incl. bin/)
./build.sh shell                 # interactive toolchain shell (cwd = ps2/)
```

Builds are **incremental** — each renderer config has its own object dir, so a
one-file edit recompiles just that file (a no-change `iso`/`fastiso` re-pack is
~2 s). Raw `make` flags also work, e.g. `./build.sh GSKIT_VIDEO=1 GS480P=1 HIRES=1`.
The music engine, renderer, video mode, and jump are chosen **at runtime on the
setup menu** — build flags only set the *defaults* an ELF starts with.

**WADs & deploy.** `iso`/`fastiso` graft WADs from `$PS2OOM_WADDIR` (else
`~/Downloads/doom`, else `./wads`). The ISO always lands in `bin/`; set
`$PS2OOM_DEPLOY` to *also* copy it somewhere (e.g. a Windows folder for PCSX2).
Keep host-specific paths in a git-ignored wrapper (e.g. `maykr.run`) that exports
those and execs `build.sh`, so clones stay clean. To run + debug in Windows PCSX2
from WSL, use [`run.sh`](run.sh) (it tails the EE serial console). See
[`ps2/README.md`](ps2/README.md) for the technical design.

## WADs & copyright

No game data is committed to this repository (`*.wad` / `*.WAD` are git-ignored).
The shareware **DOOM1.WAD** (which id Software permits redistributing) may be
embedded for convenience; commercial IWADs (DOOM.WAD, DOOM2.WAD, …) are never
included — supply your own via hostfs or on an ISO. SIGIL needs the Ultimate
**DOOM.WAD** as its IWAD (episode 5) plus the `SIGIL_COMPAT.wad` PWAD.

## Credits & licence

**PS2 Doom Definitive Edition** (codename `ps2oom`) — the PlayStation 2 port and
its hi-res / audio / controller / menu work — is **Copyright © 2026
Arawn-Davies**, released under the **GNU GPL v2** (see [`LICENSE`](LICENSE)) —
the same licence as the Doom source it derives from.

It stands on (all GPLv2):

- [doomgeneric](https://github.com/ozkl/doomgeneric) by ozkl
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom)
- id Software's original DOOM source — © id Software
- the **DBOPL** OPL2/OPL3 emulator (from DOSBox)
- [ps2sdk](https://github.com/ps2dev/ps2sdk) and
  [gsKit](https://github.com/ps2dev/gsKit) (ps2dev)

DOOM and its data/art are © id Software; only id's GPL-released *source code* is
used here — **no game WADs or art are included** (supply your own). The hi-res
title image is public-domain art.
