// Boot-time picker (IWAD, music engine, ...), drawn on the libdebug text screen
// and driven by the PS2 controller (libpad). Used before SDL takes over the GS.
//
// IMPORTANT: the pad is opened *once*, by ps2_pad.c's PS2Pad_Init(), and shared
// between this menu and in-game input. Opening pad port 0 twice (a menu copy +
// the game copy) left the port in a non-stable state, so in-game input died and
// the game was stuck in the attract/demo loop.
//
// If a controller never becomes ready within a short budget, it falls back to
// the first item so it can never hang.

#include <stdio.h>
#include <tamtypes.h>
#include <debug.h>      // scr_clear, scr_printf, scr_setXY
#include <libpad.h>     // padGetState, padRead, PAD_*

#include "ps2_menu.h"

// Single shared pad bring-up (ps2_pad.c): SIO2MAN/PADMAN + padInit + padPortOpen.
extern void PS2Pad_Init(void);

static void busy_wait(volatile int n)
{
    while (n-- > 0)
        __asm__ volatile ("nop");
}

// Wait (bounded) for the pad port to reach a readable state.
static int pad_wait_ready(void)
{
    int tries;

    for (tries = 0; tries < 250; tries++)
    {
        int s = padGetState(0, 0);
        if (s == PAD_STATE_STABLE || s == PAD_STATE_FINDCTP1)
            return 1;
        busy_wait(2000000);
    }
    return 0;
}

// libdebug draws from the very top row, which sits in the TV's top overscan and
// gets clipped -- and scr_clear resets the origin there. Start a few rows down.
#define MENU_TOP 4

static void draw(const char *title, char **items, int count, int sel)
{
    int i;
    scr_clear();
    scr_setXY(2, MENU_TOP);
    scr_printf("%s", title);
    for (i = 0; i < count; i++)
    {
        scr_setXY(2, MENU_TOP + 2 + i);
        scr_printf("%s %s", (i == sel) ? ">" : " ", items[i]);
    }
    scr_setXY(2, MENU_TOP + 2 + count + 1);
    scr_printf("Up/Down: move    Cross/Start: select");
}

int PS2_SelectMenu(const char *title, char **items, int count)
{
    struct padButtonStatus btn;
    int sel = 0, last = -1;
    u16 prev = 0xFFFF;   // active-low: all released

    PS2Pad_Init();       // shared with in-game input -- do NOT open the pad again

    if (!pad_wait_ready())
    {
        printf("menu: no controller; auto-selecting %s\n", items[0]);
        return 0;
    }

    for (;;)
    {
        // Redraw only when the selection changed -- a full scr_clear every loop
        // makes the text flicker badly.
        if (sel != last)
        {
            draw(title, items, count, sel);
            last = sel;
        }

        // Read controller (btns are active-low: 0 == pressed).
        if (padRead(0, 0, &btn) != 0)
        {
            u16 now     = btn.btns;
            u16 pressed = (prev & ~now);   // 1->0 edge: was up, now down
            prev = now;

            if (pressed & PAD_UP)
                sel = (sel - 1 + count) % count;
            if (pressed & PAD_DOWN)
                sel = (sel + 1) % count;
            if (pressed & (PAD_CROSS | PAD_START))
                break;
        }

        busy_wait(1500000);
    }

    // Clear, but leave the cursor a few rows down so whatever prints next (the
    // next menu, or Doom's continuing boot log) isn't clipped in the overscan.
    scr_clear();
    scr_setXY(0, MENU_TOP);
    return sel;
}

void PS2_SettingsMenu(const char *title, ps2_setting_t *s, int n)
{
    struct padButtonStatus btn;
    int row = 0, dirty = 1, i;
    u16 prev = 0xFFFF;

    PS2Pad_Init();   // shared with in-game input -- do NOT open the pad again

    if (!pad_wait_ready())
    {
        printf("menu: no controller; using defaults\n");
        return;      // leave each setting at its default .cur
    }

    for (;;)
    {
        // Redraw only on change (otherwise the text flickers badly).
        if (dirty)
        {
            scr_clear();
            scr_setXY(2, MENU_TOP);
            scr_printf("%s", title);
            for (i = 0; i < n; i++)
            {
                scr_setXY(2, MENU_TOP + 2 + i);
                scr_printf("%s %-7s  < %s >",
                           (i == row) ? ">" : " ",
                           s[i].label, s[i].values[s[i].cur]);
            }
            scr_setXY(2, MENU_TOP + 3 + n);
            scr_printf("Up/Down: row   Left/Right: change   Start/X: play");
            dirty = 0;
        }

        if (padRead(0, 0, &btn) != 0)
        {
            u16 now     = btn.btns;
            u16 pressed = (prev & ~now);   // 1->0 edge: button went down
            prev = now;

            if (pressed & PAD_UP)    { row = (row - 1 + n) % n; dirty = 1; }
            if (pressed & PAD_DOWN)  { row = (row + 1) % n;     dirty = 1; }
            if (pressed & PAD_LEFT)  { s[row].cur = (s[row].cur - 1 + s[row].count) % s[row].count; dirty = 1; }
            if (pressed & PAD_RIGHT) { s[row].cur = (s[row].cur + 1) % s[row].count; dirty = 1; }
            if (pressed & (PAD_CROSS | PAD_START))
                break;
        }

        busy_wait(1500000);
    }

    scr_clear();
    scr_setXY(0, MENU_TOP);
}
