// PS2 IWAD + PWAD selection.
//
// Scans cdfs (disc/ISO) and hostfs (host:) for IWADs and PWADs, shows a
// controller setup menu (IWAD / PWAD / Music / Render), and returns the chosen
// IWAD. The chosen PWAD (if any) is merged by d_main.c right after the IWAD
// (see PS2_GetPWAD + the __PS2__ hook there).
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

// PWAD chosen on the setup menu (or passed via -pwad on a renderer switch).
// NULL = none. d_main.c calls PS2_GetPWAD() after loading the IWAD.
static char *g_pwad_path = NULL;
char *PS2_GetPWAD(void) { return g_pwad_path; }

// Called from I_Quit (DOOM "quit to DOS"): re-exec the boot ELF (SDL2) with no
// -iwad arg, so PS2_GetIWAD shows the setup menu again instead of the PS2 BIOS.
void PS2_ReturnToLauncher(void)
{
    char *args[1];
    args[0] = "doom";
    LoadExecPS2(g_renderer_elf[0], 1, args);   // DOOMSDL.ELF, noreturn
}

// Candidate IWADs to probe. cdfs (disc/ISO) and hostfs (host:) are scanned into
// one list; the user picks from whatever is actually present. PWADs are a
// separate list (below) -- this row stays IWAD-only.
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

// Candidate PWADs (episode mods / megawads merged on top of an IWAD). cdfs
// names are UPPERCASE (the ISO graft uppercases them); host names keep the
// on-disk case. SIGIL_COMPAT first, as requested.
static char *cd_pwads[] = {
    "cdfs:/SIGIL_COMPAT.WAD", "cdfs:/SIGIL.WAD", "cdfs:/SIGIL_SHREDS.WAD",
    "cdfs:/NERVE.WAD", "cdfs:/SCYTHE.WAD", "cdfs:/THATCHER.WAD", NULL
};
static char *host_pwads[] = {
    "host:SIGIL_COMPAT.wad", "host:SIGIL.wad", "host:SIGIL_SHREDS.wad",
    "host:NERVE.WAD", "host:SCYTHE.WAD", "host:THATCHER.wad", NULL
};

char *PS2_GetIWAD(void)
{
#ifdef EMBED_WAD
    static char embedded_iwad[] = "doom1.wad";   // served from the baked-in array
#endif
    extern void PS2Cdfs_Init(void);
    extern int  PS2Cdfs_Exists(const char *path);

    char *labels[24];     // IWADs shown in the menu
    char *paths[24];      // IWAD path returned to the WAD loader
    int   n = 0;
    char *pw_labels[24];  // PWADs ("None" + whatever is present)
    char *pw_paths[24];   // matching PWAD paths (pw_paths[0] = NULL = none)
    int   pwn = 0;
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
            int pp = M_CheckParmWithArgs("-pwad", 1);
            if (pm > 0) g_music_engine = atoi(myargv[pm + 1]);
            if (pp > 0) g_pwad_path = myargv[pp + 1];
            printf("IWAD (from launcher): %s   pwad: %s   music: %d\n",
                   myargv[pi + 1], g_pwad_path ? g_pwad_path : "(none)", g_music_engine);
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

    // Scan for PWADs (same cdfs/host probe). Index 0 is always "None".
    pw_labels[0] = "None"; pw_paths[0] = NULL; pwn = 1;
    printf("PWAD: scanning...\n");
    for (i = 0; cd_pwads[i] != NULL && pwn < 24; ++i)
        if (PS2Cdfs_Exists(cd_pwads[i]))
        {
            printf("  %-24s [found]\n", cd_pwads[i]);
            pw_labels[pwn] = cd_pwads[i]; pw_paths[pwn] = cd_pwads[i]; pwn++;
        }
    for (i = 0; host_pwads[i] != NULL && pwn < 24; ++i)
    {
        f = fopen(host_pwads[i], "rb");
        if (f != NULL)
        {
            printf("  %-24s [found]\n", host_pwads[i]);
            fclose(f);
            pw_labels[pwn] = host_pwads[i]; pw_paths[pwn] = host_pwads[i]; pwn++;
        }
    }

    {
        // One setup page: IWAD, PWAD, music engine, and renderer. Up/Down pick a
        // row, Left/Right change it, Start/X launches. See PS2_SettingsMenu.
        static char  *eng[]  = { "OPL / FM (AdLib)", "SPU2 hardware synth" };
        static char  *rend[] = { "SDL2 (software)", "gsKit (software)", "GL (hardware)" };
        ps2_setting_t settings[4];
        char         *wad;

        if (n == 0)
        {
            printf("\n  *** No IWAD found ***\n");
            printf("  Supply a WAD on hostfs (host:) or a cdfs disc, or build EMBED_WAD=1.\n");
            for (;;) { }   // halt visibly instead of dropping to the PS2 BIOS
        }

        settings[0].label = "IWAD";   settings[0].values = labels;    settings[0].count = n;   settings[0].cur = 0;
        settings[1].label = "PWAD";   settings[1].values = pw_labels; settings[1].count = pwn; settings[1].cur = 0;
        settings[2].label = "Music";  settings[2].values = eng;       settings[2].count = 2;   settings[2].cur = g_music_engine;
        settings[3].label = "Render"; settings[3].values = rend;      settings[3].count = 3;   settings[3].cur = THIS_RENDERER;

        PS2_SettingsMenu("PS2OOM  --  setup", settings, 4);

        wad            = paths[settings[0].cur];
        g_pwad_path    = pw_paths[settings[1].cur];   // NULL when "None"
        g_music_engine = settings[2].cur;

        // If the user picked a DIFFERENT renderer, hand off to its ELF on the
        // disc with these settings (it reads -iwad/-pwad/-music, skips the menu).
        if (settings[3].cur != THIS_RENDERER)
        {
            static char musbuf[4];
            char *args[7];
            int   na;
            musbuf[0] = (char)('0' + (g_music_engine & 1));
            musbuf[1] = '\0';
            args[0] = "doom"; args[1] = "-iwad"; args[2] = wad;
            args[3] = "-music"; args[4] = musbuf;
            na = 5;
            if (g_pwad_path) { args[na++] = "-pwad"; args[na++] = g_pwad_path; }
            printf("renderer switch -> %s\n", g_renderer_elf[settings[3].cur]);
            LoadExecPS2(g_renderer_elf[settings[3].cur], na, args);   // noreturn
        }

        printf("IWAD: %s   pwad: %s   music: %s\n",
               wad, g_pwad_path ? g_pwad_path : "(none)", eng[g_music_engine]);
        return wad;
    }
}
