# PlayStation 2 build of doomgeneric.
#
#   make            -> ps2oom.elf
#   make clean
#
# Design: the upstream doomgeneric sources in ../doomgeneric are kept PRISTINE
# and compiled as-is (via VPATH). All PS2-specific code lives here in ps2/.
# Two upstream files are *replaced* rather than edited:
#     doomgeneric_sdl.c  -> doomgeneric_ps2.c   (our SDL2 backend)
#     w_file.c           -> w_file_ps2.c        (serves the embedded WAD)
# and there are two small `#ifdef __PS2__` hooks in upstream d_main.c and
# i_sdlsound.c. See README.md.
#
# libSDL2main provides the PS2 entry point (IOP reset + module load) and calls
# our main() (renamed SDL_main under __PS2__). SDL2's PS2 video driver renders
# through gsKit; SDL2_mixer + audsrv drive the SPU2.

VB = @

EE_CC  = mips64r5900el-ps2-elf-gcc
EE_CXX = mips64r5900el-ps2-elf-g++
EE_LD  = $(EE_CC)        # final link; GL_VIDEO switches this to g++ (ps2gl is C++)

DG  = ../doomgeneric        # pristine upstream sources
BIN2C = $(PS2SDK)/bin/bin2c

# Find core sources in ../doomgeneric and PS2 sources in the current dir.
VPATH = $(DG)

# Internal render resolution. HIRES=1 doubles it to 640x400 (4x the pixels, ~4x
# EE software-render cost -- the renderer is scalable, see i_video.h). RESX/RESY
# (the framebuffer doomgeneric expects) and SCREENWIDTH/HEIGHT (Doom's render
# dims) must match. Default 320x200.
RESX ?= 320
RESY ?= 200
ifdef HIRES
RESX = 640
RESY = 400
# -DHAVE_TITLE_IMAGE: blit the embedded 640x400 title art (title_image.o) instead
# of the low-res TITLEPIC. The .raw/.c are gitignored (id box art, personal use).
EE_CFLAGS_HIRES = -DHIRES -DHAVE_TITLE_IMAGE
endif

EE_CFLAGS = -D_EE -O2 -G0 -Wall -DHAVE_CONFIG_H -D__PS2__ -DFEATURE_SOUND \
            -DDOOMGENERIC_RESX=$(RESX) -DDOOMGENERIC_RESY=$(RESY) \
            -DSCREENWIDTH=$(RESX) -DSCREENHEIGHT=$(RESY) $(EE_CFLAGS_HIRES) \
            -DDEFAULT_RAM=18 -DMIN_RAM=8 \
            -fdata-sections -ffunction-sections

# Copyright: by default NO WAD is embedded, so the ELF contains no game data
# and a WAD must be supplied at runtime via hostfs (drop it next to the ELF).
# `make EMBED_WAD=1` additionally bakes in DOOM1.WAD (shareware, which id
# Software permits redistributing) as a built-in fallback -- for personal
# convenience. Never embed a commercial WAD you intend to distribute.
ifdef EMBED_WAD
EE_CFLAGS += -DEMBED_WAD
endif

# GSKIT_VIDEO=1 swaps the SDL2 video backend (doomgeneric_ps2.c) for the native
# gsKit PSMT8+CLUT one (doomgeneric_ps2_gs.c) and builds Doom with an 8-bit
# indexed framebuffer (CMAP256) so the GS does the palette expand + upscale.
ifdef GL_VIDEO
# GL_VIDEO=1 -> the NATIVE VU1+DMA hardware world renderer (doomgeneric_ps2_gl.cpp
# backend + r_gl.c + the draw_3D.vsm VU1 microprogram). Renders straight to the
# GS with a Z-buffer via PS2SDK's libdraw/libgraph/libdma/libpacket2/libmath3d
# (no ps2gl). 8-bit framebuffer (CMAP256) feeds the BSP-visibility hooks; the
# backend is C++ so the whole ELF links with g++. (The pre-M1 gsKit-scaled
# software build, GSKIT_VIDEO, is a separate fork and untouched.)
EE_CFLAGS += -DUSE_GL -DCMAP256 -DRENDERER_GL
VIDEO_OBJ  = doomgeneric_ps2_gl.o r_gl.o r_native.o draw_3D.o
EE_LD      = $(EE_CXX)
GL_LIBS    = -ldraw -lgraph -lmath3d -lpacket2 -ldma
EE_DVP     = $(PS2SDK)/../dvp/bin/dvp-as
else ifdef GSKIT_VIDEO
EE_CFLAGS += -DUSE_GSKIT -DCMAP256 -DRENDERER_GS
VIDEO_OBJ  = doomgeneric_ps2_gs.o
else
VIDEO_OBJ  = doomgeneric_ps2.o
endif

# GS480P=1 (with GSKIT_VIDEO) outputs 480p progressive (DTV, 31kHz) instead of
# NTSC 640x448 interlaced -- sharp, flicker-free; needs component/YPbPr (or PCSX2).
ifdef GS480P
EE_CFLAGS += -DGS_OUTPUT_480P
endif

EE_INCS = -I. -I$(DG) \
          -I$(GSKIT)/include \
          -I$(PS2SDK)/ports/include/SDL2 \
          -I$(PS2SDK)/ports/include \
          -I$(PS2SDK)/ee/include \
          -I$(PS2SDK)/common/include

EE_LDFLAGS = -L$(PS2SDK)/ee/lib -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib \
             -Wl,--gc-sections

# SDL2 (video only) + PS2 backends. Audio is native (i_audsrvsound.c -> audsrv),
# so SDL2_mixer and the audio codec libs are gone. libSDL2main pulls in
# libps2_drivers; SDL2 video needs gsKit/dmaKit; libdebug = GS text console;
# libaudsrv drives the SPU2.
EE_LIBS = -lSDL2main -lSDL2 -lps2_drivers \
          -lgskit -ldmakit $(GL_LIBS) \
          -ldebug -laudsrv -lpoweroff \
          -lfileXio -lpatches -lpad -lmc -lkbd \
          -lm -lc -lkernel

# Overridable so each build config (SDL2 / gsKit-HIRES / GL) can keep its OWN
# object dir + ELF. That lets build.sh compile the configs into separate dirs
# WITHOUT a `make clean` between them, so unchanged files are skipped (the same
# source compiled with different -D flags can't share one dir, which is why the
# old flow cleaned every time).
EE_OBJDIR ?= build
EE_BIN ?= ps2oom.elf

# --- Pristine upstream objects (compiled from ../doomgeneric) -------------
# Note: doomgeneric_sdl.o and w_file.o are intentionally omitted; the PS2
# replacements below stand in for them.
CORE_OBJS = dummy.o am_map.o doomdef.o doomstat.o dstrings.o d_event.o d_items.o d_iwad.o d_loop.o d_main.o d_mode.o d_net.o \
	f_finale.o f_wipe.o g_game.o hu_lib.o hu_stuff.o info.o i_cdmus.o i_endoom.o i_joystick.o i_scale.o i_sound.o i_system.o \
	i_timer.o memio.o m_argv.o m_bbox.o m_cheat.o m_config.o m_controls.o m_fixed.o m_menu.o m_misc.o m_random.o \
	p_ceilng.o p_doors.o p_enemy.o p_floor.o p_inter.o p_lights.o p_map.o p_maputl.o p_mobj.o p_plats.o p_pspr.o \
	p_saveg.o p_setup.o p_sight.o p_spec.o p_switch.o p_telept.o p_tick.o p_user.o r_bsp.o r_data.o r_draw.o r_main.o \
	r_plane.o r_segs.o r_sky.o r_things.o sha1.o sounds.o statdump.o st_lib.o st_stuff.o s_sound.o tables.o v_video.o \
	w_checksum.o w_main.o w_wad.o z_zone.o w_file_stdc.o i_input.o i_video.o doomgeneric.o \
	wi_stuff.o gusconf.o mus2mid.o

# --- PS2-specific objects (compiled from ./) ------------------------------
# ps2_audio_driver.o overrides libps2_drivers' init_audio_driver so we control
# the audsrv bring-up; libsd_irx.o + audsrv_irx.o are the IRX modules it loads.
# Both music engines are linked in -- the startup menu picks one at runtime
# (PS2_MusicEngine(); see ps2_iwad.c + i_sound.c). OPL/FM = i_oplmusic + opl +
# dbopl; SPU2 synth = i_spu2music + spusynth_irx (the embedded IOP module).
PS2_OBJS = $(VIDEO_OBJ) w_file_ps2.o w_file_cdfs.o ps2_iwad.o ps2_cdfs.o \
	ps2_bootscr.o ps2_audio.o ps2_audio_driver.o ps2_drivers_stub.o ps2_menu.o ps2_pad.o ps2_mcsave.o \
	i_audsrvsound.o audsrv_irx.o libsd_irx.o poweroff_irx.o \
	i_oplmusic.o midifile.o opl.o dbopl.o opl_queue.o opl_ps2.o \
	i_spu2music.o spusynth_irx.o

# The embedded shareware WAD is only compiled in for EMBED_WAD builds.
ifdef EMBED_WAD
PS2_OBJS += w_file_mem.o doom1_wad.o
endif

# Hi-res builds embed a 640x400 title image (drawn instead of the low-res
# TITLEPIC; see d_main.c D_PageDrawer). title_image.raw is produced offline.
ifdef HIRES
PS2_OBJS += title_image.o
endif

# SPU_BEEP=1 builds the SPU2 hardware-synth S1 self-test: load spusynth.irx so
# the IOP keys one SPU2 voice, then halt (ps2_spusynth.c + a hook in ps2_iwad.c).
# Runs in isolation, before the audsrv stack -- just to confirm sound on hardware.
ifdef SPU_BEEP
EE_CFLAGS += -DSPU_BEEP
PS2_OBJS  += ps2_spusynth.o          # spusynth_irx.o is already in PS2_OBJS
endif

# SPU_MUSIC=1 just makes the SPU2 synth the *default* music engine in the
# startup menu (both engines are always linked; see PS2_OBJS above).
ifdef SPU_MUSIC
EE_CFLAGS += -DSPU_MUSIC
endif

EE_OBJS = $(addprefix $(EE_OBJDIR)/, $(CORE_OBJS) $(PS2_OBJS))

all: $(EE_BIN)

clean:
	rm -rf build build-sdl build-gs build-gl $(EE_OBJDIR) \
	       ps2oom.elf DOOMSDL.ELF DOOMGS.ELF DOOMGL.ELF *.map

# Embedded assets (generated by bin2c). DOOM1.WAD must be provided here;
# audsrv.irx is copied from the SDK.
doom1_wad.c: DOOM1.WAD
	$(BIN2C) $< $@ doom1_wad

# 640x400 title image (raw 8-bit Doom-palette indices), produced offline from
# the source art. Embedded only in HIRES builds (see PS2_OBJS / D_PageDrawer).
title_image.c: title_image.raw
	$(BIN2C) $< $@ title_image

audsrv.irx:
	cp $(PS2SDK)/iop/irx/audsrv.irx $@

audsrv_irx.c: audsrv.irx
	$(BIN2C) $< $@ audsrv_irx

libsd.irx:
	cp $(PS2SDK)/iop/irx/libsd.irx $@

libsd_irx.c: libsd.irx
	$(BIN2C) $< $@ libsd_irx

# poweroff IOP module -- lets the setup menu power the PS2 down (libpwroff).
poweroff.irx:
	cp $(PS2SDK)/iop/irx/poweroff.irx $@

poweroff_irx.c: poweroff.irx
	$(BIN2C) $< $@ poweroff_irx

# Our SPU2 synth IOP module (iop/spusynth/): build the IRX with the IOP
# toolchain via its own make, then embed it with bin2c like the other modules.
iop/spusynth/spusynth.irx: iop/spusynth/spusynth.c iop/spusynth/spusynth_rpc.h iop/spusynth/imports.lst iop/spusynth/irx_imports.h iop/spusynth/Makefile
	$(MAKE) -C iop/spusynth

spusynth.irx: iop/spusynth/spusynth.irx
	cp $< $@

spusynth_irx.c: spusynth.irx
	$(BIN2C) $< $@ spusynth_irx

$(EE_BIN): $(EE_OBJS)
	@echo [Linking $@]
	$(VB)$(EE_LD) $(EE_CFLAGS) $(EE_INCS) $(EE_LDFLAGS) $(EE_OBJS) \
		-o $(EE_BIN) $(EE_LIBS) -Wl,-Map,$(EE_BIN).map

$(EE_OBJS): | $(EE_OBJDIR)

$(EE_OBJDIR):
	$(VB)mkdir -p $(EE_OBJDIR)

# Hot software-renderer inner loops: compile at -O3 (extra unroll/scheduling).
# The last -O on the command line wins, so this overrides EE_CFLAGS' -O2.
# (The bottleneck at hi-res is memory bandwidth, so the win is modest.)
$(EE_OBJDIR)/r_draw.o $(EE_OBJDIR)/r_plane.o $(EE_OBJDIR)/r_things.o \
$(EE_OBJDIR)/r_segs.o $(EE_OBJDIR)/r_main.o $(EE_OBJDIR)/r_bsp.o: EE_CFLAGS += -O3

$(EE_OBJDIR)/%.o: %.c
	@echo [Compiling $<]
	$(VB)$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(EE_OBJDIR)/%.o: %.cpp
	@echo [Compiling $<]
	$(VB)$(EE_CXX) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# VU1 microprogram (draw_3D.vsm) -> object with VU1Draw3D_CodeStart/End in .vudata
$(EE_OBJDIR)/%.o: %.vsm
	@echo [Assembling $<]
	$(VB)$(EE_DVP) $< -o $@
