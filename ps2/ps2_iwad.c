// PS2 IWAD selection.
//
// Prefers an IWAD on the host filesystem (hostfs) next to the ELF, showing a
// controller menu if several are present; otherwise falls back to the
// shareware IWAD embedded in the executable (EMBED_WAD builds only).
//
// Called from the one PS2 hook in the upstream d_main.c (guarded by __PS2__).

#include <stdio.h>
#include <stdlib.h>     // atoi
#include <kernel.h>     // LoadExecPS2

#include "ps2_menu.h"   // PS2_SettingsMenu, ps2_setting_t
#include "m_argv.h"     // M_CheckParmWithArgs, myargv

// This ELF's video renderer (set at build time). The three backends live in
// three ELFs on the disc and share one setup screen; choosing a different one
// LoadExec's its ELF with the chosen settings. 0 = SDL2 (software), 1 = gsKit
// (software), 2 = GL (hardware geometry).
#if defined(RENDERER_GL)
#define THIS_RENDERER 2
#elif defined(RENDERER_GS)
#define THIS_RENDERER 1
#else
#define THIS_RENDERER 0
#endif

static const char *g_renderer_elf[3] = {
    "cdrom0:\\DOOMSDL.ELF;1",
    "cdrom0:\\DOOMGS.ELF;1",
    "cdrom0:\\DOOMGL.ELF;1",
};

// Music engine chosen at the startup menu: 0 = OPL/FM (audsrv), 1 = SPU2 synth.
// Default OPL (the classic sound); the menu's second option is SPU2. Read by
// i_sound.c's InitMusicModule.
static int g_music_engine = 0;
int PS2_MusicEngine(void) { return g_music_engine; }

// Called from I_Quit (DOOM "quit to DOS"): re-exec the boot ELF (SDL2) with no
// -iwad arg, so PS2_GetIWAD shows the setup menu again instead of the PS2 BIOS.
void PS2_ReturnToLauncher(void)
{
    char *args[1];
    args[0] = "doom";
    LoadExecPS2(g_renderer_elf[0], 1, args);   // DOOMSDL.ELF, noreturn
}

// Candidate IWADs to probe. cdfs (disc/ISO) and hostfs (host:) are scanned into
// one list; the user picks from whatever is actually present. PWADs (SIGIL,
// mods, ...) are not IWADs and aren't listed here.
static char *cd_iwads[] = {
    "cdfs:/DOOM.WAD", "cdfs:/DOOM2.WAD", "cdfs:/DOOM1.WAD",
    "cdfs:/PLUTONIA.WAD", "cdfs:/TNT.WAD",
    "cdfs:/FREEDOOM1.WAD", "cdfs:/FREEDOOM2.WAD", "cdfs:/FREEDM.WAD", NULL
};
static char *host_iwads[] = {
    "host:DOOM.WAD", "host:DOOM2.WAD", "host:DOOM1.WAD",
    "host:PLUTONIA.WAD", "host:TNT.WAD", "host:DOOM64.WAD",
    "host:freedoom1.wad", "host:freedoom2.wad", "host:freedm.wad", NULL
};

char *PS2_GetIWAD(void)
{
#ifdef EMBED_WAD
    static char embedded_iwad[] = "doom1.wad";   // served from the baked-in array
#endif
    extern void PS2Cdfs_Init(void);
    extern int  PS2Cdfs_Exists(const char *path);

    char *labels[24];     // shown in the menu
    char *paths[24];      // returned to the WAD loader
    int   n = 0;
    int   i;
    FILE *f;

#ifdef SPU_BEEP
    // S1: prove the SPU2 hardware-voice path in isolation, before any audio
    // stack is up, then halt so the tone is all we hear.
    {
        extern void PS2Spu_BeepTest(void);
        printf("\n=== SPU2 synth S1: hardware voice self-test ===\n");
        PS2Spu_BeepTest();
        printf("spu: halted -- you should hear a steady tone.\n");
        for (;;) { }
    }
#endif

    // Launched by the OTHER renderer's ELF (a renderer switch) with explicit
    // settings? Use them and skip the setup menu -- no second screen.
    {
        int pi = M_CheckParmWithArgs("-iwad", 1);
        if (pi > 0)
        {
            int pm = M_CheckParmWithArgs("-music", 1);
            if (pm > 0) g_music_engine = atoi(myargv[pm + 1]);
            printf("IWAD (from launcher): %s   music: %d\n", myargv[pi + 1], g_music_engine);
            return myargv[pi + 1];
        }
    }

    // Scan the disc (cdfs). PS2Cdfs_Init() is quick now that the boot-device
    // wait is gone (see ps2_drivers_stub.c); a missing disc just probes empty.
    printf("IWAD: scanning disc (cdfs)...\n");
    PS2Cdfs_Init();
    for (i = 0; cd_iwads[i] != NULL && n < 24; ++i)
    {
        if (PS2Cdfs_Exists(cd_iwads[i]))
        {
            printf("  %-24s [found]\n", cd_iwads[i]);
            labels[n] = cd_iwads[i];
            paths[n]  = cd_iwads[i];
            n++;
        }
    }

    // Scan hostfs (host:, e.g. PCSX2's mapped folder, next to the ELF).
    printf("IWAD: scanning hostfs...\n");
    for (i = 0; host_iwads[i] != NULL && n < 24; ++i)
    {
        f = fopen(host_iwads[i], "rb");
        if (f != NULL)
        {
            printf("  %-24s [found]\n", host_iwads[i]);
            fclose(f);
            labels[n] = host_iwads[i];
            paths[n]  = host_iwads[i];
            n++;
        }
    }

#ifdef EMBED_WAD
    if (n < 24)
    {
        labels[n] = "Embedded shareware DOOM1.WAD";
        paths[n]  = embedded_iwad;
        n++;
    }
#endif

    {
        // One setup page: IWAD, music engine, and renderer. Up/Down pick a row,
        // Left/Right change it, Start/X launches. See PS2_SettingsMenu.
        static char  *eng[]  = { "OPL / FM (AdLib)", "SPU2 hardware synth" };
        static char  *rend[] = { "SDL2 (software)", "gsKit (software)", "GL (hardware)" };
        ps2_setting_t settings[3];
        char         *wad;

        if (n == 0)
        {
            printf("\n  *** No IWAD found ***\n");
            printf("  Supply a WAD on hostfs (host:) or a cdfs disc, or build EMBED_WAD=1.\n");
            for (;;) { }   // halt visibly instead of dropping to the PS2 BIOS
        }

        settings[0].label = "IWAD";   settings[0].values = labels; settings[0].count = n; settings[0].cur = 0;
        settings[1].label = "Music";  settings[1].values = eng;    settings[1].count = 2; settings[1].cur = g_music_engine;
        settings[2].label = "Render"; settings[2].values = rend;   settings[2].count = 3; settings[2].cur = THIS_RENDERER;

        PS2_SettingsMenu("PS2OOM  --  setup", settings, 3);

        wad            = paths[settings[0].cur];
        g_music_engine = settings[1].cur;

        // If the user picked a DIFFERENT renderer, hand off to its ELF on the
        // disc with these settings (it reads -iwad/-music and skips the menu).
        if (settings[2].cur != THIS_RENDERER)
        {
            static char musbuf[4];
            char *args[5];
            musbuf[0] = (char)('0' + (g_music_engine & 1));
            musbuf[1] = '\0';
            args[0] = "doom"; args[1] = "-iwad"; args[2] = wad;
            args[3] = "-music"; args[4] = musbuf;
            printf("renderer switch -> %s\n", g_renderer_elf[settings[2].cur]);
            LoadExecPS2(g_renderer_elf[settings[2].cur], 5, args);   // noreturn
        }

        printf("IWAD: %s   music: %s\n", wad, eng[g_music_engine]);
        return wad;
    }
}
