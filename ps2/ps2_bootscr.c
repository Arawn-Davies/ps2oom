// PS2 boot text console.
//
// Brings up the GS debug text framebuffer (libdebug) and redirects the EE
// stdout/stderr to it, so Doom's normal boot log is visible on screen for a
// few seconds before SDL takes over the GS for the game's framebuffer.
//
// ps2sdk's default printf goes to the SIO serial port, which emulators don't
// show -- hence this on-screen route. (This was briefly suspected of slowing
// boot; the real culprit was waitUntilDeviceIsReady, see ps2_drivers_stub.c.)

#include <stdio.h>
#include <string.h>
#include <debug.h>      // init_scr, scr_printf

static int g_scr_active = 0;

void BootScr_Begin(void)
{
    init_scr();
    // libdebug draws from the very top row, which sits in the TV's top overscan
    // and clips the first line. Start a couple of rows down so it's readable.
    scr_setXY(0, 2);
    g_scr_active = 1;
}

void BootScr_End(void)
{
    g_scr_active = 0;
}

// Fatal-error screen. Doom's I_Error normally exits silently (on PS2 that just
// looks like a reboot to the setup menu), hiding which limit/error was hit --
// the boot console is long gone by gameplay. This re-takes the GS with the
// libdebug text console, shows the message, and waits for a button to return to
// the setup menu. Called from I_Error (i_system.c) on __PS2__; does not return.
void PS2_ErrorScreen(const char *msg)
{
    extern void PS2_ReturnToLauncher(void);
    extern int  PS2Pad_AnyPressed(void);   // START or X currently down

    g_scr_active = 0;        // stop the _write redirect; we draw directly now
    init_scr();              // take the GS back from the game renderer
    scr_setXY(0, 2);
    scr_printf("\n");
    scr_printf("  ================ DOOM ERROR ================\n\n");
    scr_printf("  %s\n\n", msg ? msg : "(unknown)");
    scr_printf("  ===========================================\n\n");
    scr_printf("  Press START or X to return to the setup menu.\n");

    // Debounce: wait for any held button to release, then for a fresh press.
    while (PS2Pad_AnyPressed()) { }
    while (!PS2Pad_AnyPressed()) { }

    PS2_ReturnToLauncher();  // noreturn
    for (;;) { }             // belt and braces
}

// Case-insensitive substring test.
static int ci_contains(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    if (nl == 0 || nl > n)
        return 0;
    for (i = 0; i + nl <= n; ++i)
    {
        for (j = 0; j < nl; ++j)
        {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

// Only the lines we actually want during boot reach the (slow) GS console;
// the rest of Doom's startup spam is dropped -- which also speeds boot.
static int want_line(const char *p, size_t n)
{
    static const char *keys[] = {
        "audio", "snd", "audsrv", "libsd", "iwad",
        "build", "game:", "error", ">>>", "===",
        "opl", "music", "adlib", "genmidi", "midi", "dude",
        "spu", "menu", NULL
    };
    int k;
    for (k = 0; keys[k] != NULL; ++k)
        if (ci_contains(p, n, keys[k]))
            return 1;
    return 0;
}

// Override the newlib EE syscall so stdout/stderr land on the debug screen
// while the boot console is active. scr_printf wants a NUL-terminated string,
// so copy the write payload out in bounded chunks.
int _write(int fd, const void *buf, size_t nbyte)
{
    if ((fd == 1 || fd == 2) && g_scr_active && want_line((const char *) buf, nbyte))
    {
        const char *p = (const char *) buf;
        char tmp[120];
        size_t i = 0;

        while (i < nbyte)
        {
            size_t n = nbyte - i;
            if (n > sizeof(tmp) - 1)
                n = sizeof(tmp) - 1;
            memcpy(tmp, p + i, n);
            tmp[n] = '\0';
            scr_printf("%s", tmp);
            i += n;
        }
    }

    // We have no filesystem; other fds are no-ops.
    return (int) nbyte;
}
