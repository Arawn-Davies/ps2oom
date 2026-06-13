// PS2 controller input via libpad: dual analog sticks + a modern pad layout.
//
// Analog is forced ON and LOCKED (padSetMainMode DUALSHOCK/LOCK) so the sticks
// always work without pressing the analog button.
//
//   Left stick    move forward/back + strafe   (fed as ev_joystick axes)
//   Right stick   turn                          (fed as ev_joystick axis)
//   push far      run (auto speed)              (KEY_RSHIFT while deflected hard)
//   R2 / Square   fire
//   Cross         use (doors/switches)
//   L2            run (hold)
//   L1 / R1       previous / next weapon
//   Triangle      enter / confirm
//   Circle / Start  escape (open/close menu)
//   Select        automap (tab)
//   D-pad         menu navigation (also digital move/turn in-game)
//
// Movement goes in as ev_joystick events (D_PostEvent); buttons go in as Doom
// key events via the emit callback (the video backend's key queue). Polled once
// per frame. Kept out of SDL so the gsKit backend (no SDL event loop) works too.

#include <tamtypes.h>
#include <loadfile.h>   // SifLoadModule
#include <libpad.h>

#include "doomkeys.h"
#include "d_event.h"    // event_t, ev_joystick, D_PostEvent

static char g_padBuf[256] __attribute__((aligned(64)));
static int  g_inited  = 0;
static int  g_analog  = 0;       // analog mode requested yet?
static u16  g_prev    = 0xFFFF;  // button state, active-low (1 == released)
static int  g_run     = 0;       // stick-deflection "run" currently held?
static int  g_weapon  = 1;       // local weapon index for L1/R1 cycling (1..7)

#define DEAD   40                // stick deadzone around centre (128)
#define RUNMAG 88                // left-stick deflection past this = run

void PS2Pad_Init(void)
{
    if (g_inited)
        return;

    // SIO2 + pad managers (harmless if already loaded by something else).
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    padInit(0);
    padPortOpen(0, 0, g_padBuf);
    g_inited = 1;
}

// Simple buttons -> Doom keys (L1/R1 and the sticks are handled separately).
static const struct { u16 mask; unsigned char key; } g_map[] = {
    { PAD_UP,       KEY_UPARROW    },
    { PAD_DOWN,     KEY_DOWNARROW  },
    { PAD_LEFT,     KEY_LEFTARROW  },
    { PAD_RIGHT,    KEY_RIGHTARROW },
    { PAD_R2,       KEY_FIRE       },
    { PAD_SQUARE,   KEY_FIRE       },
    { PAD_CROSS,    KEY_USE        },   // A: use (in-game) ...
    { PAD_CROSS,    KEY_ENTER      },   //    ... and confirm (menus)
    { PAD_L2,       KEY_RSHIFT     },   // run (hold)
    { PAD_TRIANGLE, KEY_TAB        },   // automap
    { PAD_CIRCLE,   KEY_ESCAPE     },   // B: back / menu
    { PAD_START,    KEY_ESCAPE     },
    { PAD_SELECT,   KEY_TAB        },
};

static int axis(unsigned char v)   // 0..255, centre 128 -> -1 / 0 / +1
{
    if (v > 128 + DEAD) return 1;
    if (v < 128 - DEAD) return -1;
    return 0;
}

void PS2Pad_Poll(void (*emit)(int pressed, unsigned char doomkey))
{
    struct padButtonStatus btn;
    event_t ev;
    int s, i, want_run, lmag, vmag;
    u16 now, changed;

    PS2Pad_Init();   // lazy, one-time

    s = padGetState(0, 0);
    if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1)
        return;      // not ready yet (no controller / still detecting)

    // Force the pad into locked analog mode once it's readable.
    if (!g_analog)
    {
        padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        g_analog = 1;
    }

    if (padRead(0, 0, &btn) == 0)
        return;

    // --- analog movement -> ev_joystick (Doom uses the sign of each axis) ----
    // ljoy_v: up (small value) = forward, and Doom wants joyymove < 0 to go
    // forward, so axis() maps up -> -1 naturally.
    ev.type  = ev_joystick;
    ev.data1 = 0;                          // actions come in as keys, not joybuttons
    ev.data2 = axis(btn.rjoy_h);           // turn  (right stick X)
    ev.data3 = axis(btn.ljoy_v);           // forward/back (left stick Y)
    ev.data4 = axis(btn.ljoy_h);           // strafe (left stick X)
    D_PostEvent(&ev);

    // Push the left stick far = run (edge-triggered so the queue isn't spammed).
    lmag = btn.ljoy_h > 128 ? btn.ljoy_h - 128 : 128 - btn.ljoy_h;
    vmag = btn.ljoy_v > 128 ? btn.ljoy_v - 128 : 128 - btn.ljoy_v;
    if (vmag > lmag) lmag = vmag;
    want_run = (lmag > RUNMAG);
    if (want_run != g_run)
    {
        emit(want_run, KEY_RSHIFT);
        g_run = want_run;
    }

    // --- buttons -> keys -----------------------------------------------------
    now     = btn.btns;          // active-low: a 0 bit means that button is down
    changed = g_prev ^ now;

    for (i = 0; i < (int) (sizeof(g_map) / sizeof(g_map[0])); ++i)
        if (changed & g_map[i].mask)
            emit((now & g_map[i].mask) == 0, g_map[i].key);

    // L1 / R1: previous / next weapon (tracked locally, tapped as a number key).
    if ((changed & PAD_L1) && (now & PAD_L1) == 0)
    {
        g_weapon = (g_weapon <= 1) ? 7 : g_weapon - 1;
        emit(1, (unsigned char)('0' + g_weapon));
        emit(0, (unsigned char)('0' + g_weapon));
    }
    if ((changed & PAD_R1) && (now & PAD_R1) == 0)
    {
        g_weapon = (g_weapon >= 7) ? 1 : g_weapon + 1;
        emit(1, (unsigned char)('0' + g_weapon));
        emit(0, (unsigned char)('0' + g_weapon));
    }

    g_prev = now;
}
