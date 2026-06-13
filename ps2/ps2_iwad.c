// PS2 IWAD selection.
//
// Prefers an IWAD on the host filesystem (hostfs) next to the ELF, showing a
// controller menu if several are present; otherwise falls back to the
// shareware IWAD embedded in the executable (EMBED_WAD builds only).
//
// Called from the one PS2 hook in the upstream d_main.c (guarded by __PS2__).

#include <stdio.h>

#include "ps2_menu.h"   // PS2_SettingsMenu, ps2_setting_t

// Music engine chosen at the startup menu: 0 = OPL/FM (audsrv), 1 = SPU2 synth.
// Default OPL (the classic sound); the menu's second option is SPU2. Read by
// i_sound.c's InitMusicModule.
static int g_music_engine = 0;
int PS2_MusicEngine(void) { return g_music_engine; }

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
        // One setup page: IWAD on one row (Left/Right cycles the found WADs),
        // music engine on the next. Start/X launches. See PS2_SettingsMenu.
        static char  *eng[] = { "OPL / FM (AdLib)", "SPU2 hardware synth" };
        ps2_setting_t settings[2];
        char         *wad;

        if (n == 0)
        {
            printf("\n  *** No IWAD found ***\n");
            printf("  Supply a WAD on hostfs (host:) or a cdfs disc, or build EMBED_WAD=1.\n");
            for (;;) { }   // halt visibly instead of dropping to the PS2 BIOS
        }

        settings[0].label = "IWAD";  settings[0].values = labels; settings[0].count = n; settings[0].cur = 0;
        settings[1].label = "Music"; settings[1].values = eng;    settings[1].count = 2; settings[1].cur = g_music_engine;

        PS2_SettingsMenu("PS2OOM  --  setup", settings, 2);

        wad            = paths[settings[0].cur];
        g_music_engine = settings[1].cur;
        printf("IWAD: %s   music: %s\n", wad, eng[g_music_engine]);
        return wad;
    }
}
