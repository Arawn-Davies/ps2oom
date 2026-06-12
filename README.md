# DOOM — PlayStation 2 (ps2oom)

A native PlayStation 2 port of DOOM: full speed, with hardware (gsKit) video,
native SPU2 audio, and **two selectable music engines** — the classic
AdLib / Sound-Blaster **OPL FM soundtrack**, or an experimental **native SPU2
hardware-voice synth** that sequences the MIDI on the IOP and plays it through
the PS2 sound chip's own ADPCM voices.

Specialised for the PS2, built from
[doomgeneric](https://github.com/ozkl/doomgeneric) (and through it
[Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) and
id Software's original DOOM source).

## Features

- **Native gsKit video** — Doom's 8-bit framebuffer is uploaded as a PSMT8
  texture + CLUT and the GS does the palette expansion and bilinear upscale in
  hardware. Full speed at 320×200.
- **480p progressive output** (`GS480P=1`) for component / YPbPr displays, plus
  the standard NTSC 640×448 interlaced mode.
- **Native audsrv audio** — sound effects mixed on the EE and streamed to the
  SPU2's PCM source (no SDL audio).
- **Two music engines** (pick at build time):
  - **OPL / FM (DBOPL)** — AdLib-style synthesis driven from the IWAD's GENMIDI
    lump, mixed into the audsrv stream. *Default.*
  - **SPU2 hardware-voice synth** — the MIDI is parsed and flattened on the EE,
    shipped to an IOP module over SIF RPC, and sequenced there onto the SPU2's
    hardware ADPCM voices. Opt-in via `./build.sh spumusic`. See below.
- **Controller input** via libpad (DualShock).
- **Flexible WAD loading** — embedded shareware DOOM1.WAD, a WAD on hostfs
  (PCSX2 `host:`), or a WAD on a cdfs disc/ISO (read on demand). A controller
  menu picks between several.
- **Fast boot** (~4 s to gameplay). The previous ~30 s stall was a
  libps2_drivers `waitUntilDeviceIsReady` device-probe timeout, now overridden.

## The SPU2 hardware-voice synth (experimental)

Instead of software-rendering FM into the audio stream, this backend drives the
SPU2's **48 hardware ADPCM voices** as a sample-based MIDI synthesiser — the most
"native" way to make music on the PS2. It is built in milestones:

- **S1** — one hardware voice keyed end-to-end. The hard-won fixes: key voices on
  **Core 1** (the live DAC core; audsrv zeroes Core 0's master volume), open the
  **`MMIX` voice-dry gates `0xC00`**, and pass the SPU upload address *as*
  `sceSdVoiceTrans`'s pointer argument — libsd uses the pointer's value as the
  destination address and never dereferences it, so `&var` uploads to a stack
  address.
- **S2** — polyphony, real note pitches (`sceSdNote2Pitch`), and ADSR envelopes.
- **S3** — a full MIDI player: `midifile.c` parses the song (MUS converted to
  MIDI in memory), every track is flattened in absolute-tick order with tempo
  applied into a timed event stream, shipped to the IOP over **SIF RPC**, and a
  sequencer thread there plays it with `DelayThread` timing onto a 20-voice pool.
- **S4** — a synthesised **General-MIDI waveform bank** (square / saw / triangle
  / sine / pulse + a noise sample for drums) plus a GM-family **patch map**
  (`PROGRAM_CHANGE` selects each channel's waveform + envelope); channel 9 plays
  the noise sample as percussion.

The result is the DOOM soundtrack played on actual SPU2 voices — a chiptune-ish
rendering today (synthesised waveforms, not recorded instruments). It coexists
with audsrv (which powers the chip up) rather than driving the SPU2 standalone.

## Building

Everything builds in the official ps2dev toolchain through Docker (the ps2dock
image):

```sh
./build.sh                                   # ps2/doomgeneric.elf, no WAD baked in
./build.sh EMBED_WAD=1                        # + embed shareware DOOM1.WAD (OPL music)
./build.sh spumusic                           # SPU2 hardware-voice synth + embedded WAD
./build.sh stable                             # native gsKit video, 480p, embedded WAD
./build.sh gl                                 # experimental ps2gl world renderer
./build.sh iso [WAD]                          # pack the current ELF + a WAD into a
                                              #   bootable PS2 ISO (default freedoom1.wad)
./build.sh clean                              # remove build artifacts
./build.sh shell                              # interactive toolchain shell (cwd = ps2/)
```

Raw `make` flags also work (`./build.sh SPU_MUSIC=1 EMBED_WAD=1`). The music
engine is selected at build time: omit `spumusic` / `SPU_MUSIC=1` for OPL music.

Run the resulting `ps2/doomgeneric.elf` (or the ISO) in PCSX2 or on real
hardware. See [`ps2/README.md`](ps2/README.md) for the technical design.

## Controls

D-pad move / turn · **✕** fire · **○** use · **□** run · **L1/R1** strafe ·
**△** Enter · **Start** menu · **Select** automap

## WADs & copyright

No game data is committed to this repository (`*.wad` is git-ignored). The
shareware **DOOM1.WAD** (which id Software permits redistributing) can be
embedded for convenience; commercial IWADs (DOOM.WAD, DOOM2.WAD) are never
included — supply your own, via hostfs or on an ISO.

## Credits & licence

Released under the **GPLv2** (see [`LICENSE`](LICENSE)). This port stands on:

- [doomgeneric](https://github.com/ozkl/doomgeneric) by ozkl
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom)
- id Software's original DOOM source
- the **DBOPL** OPL2/OPL3 emulator (from DOSBox)
- [ps2sdk](https://github.com/ps2dev/ps2sdk) and
  [gsKit](https://github.com/ps2dev/gsKit) (ps2dev)
