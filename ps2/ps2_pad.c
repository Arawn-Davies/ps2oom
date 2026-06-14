// PS2 controller input via libpad: dual analog sticks + a modern pad layout.
//
// Analog is forced ON and LOCKED (padSetMainMode DUALSHOCK/LOCK) so the sticks
// always work without pressing the analog button.
//
//   Left stick    move forward/back + strafe   (fed as ev_joystick axes)
//   Right stick   turn  (PROPORTIONAL)          (fed as ev_joystick turn mag)
//   R2 / Square   fire
//   Cross (X)     use (doors/switches) + menu CONFIRM
//   Circle (O)    menu CANCEL / open-close menu (escape)
//   L2            run (hold)
//   L1 / R1       previous / next weapon
//   Triangle      automap (tab)
//   Start         escape (open/close menu)
//   Select        automap (tab)
//   D-pad         menu navigation (also digital move/turn in-game)
//
// Five settings (live-adjustable from the in-game Options -> Controller page,
// persisted via m_config; see PS2Pad_BindConfig + m_config.c):
//   ps2_turn_sensitivity  1..20  right-stick turn speed (10 = ~vanilla fast)
//   ps2_always_run        0/1    always run (vs push-stick-far to run)
//   ps2_stick_deadzone    px     centre deadzone (25 small / 40 / 60 large)
//   ps2_invert_y          0/1    invert forward/back
//   ps2_southpaw          0/1    swap sticks (move<->turn)
//
// Movement goes in as ev_joystick events (D_PostEvent); buttons go in as Doom
// key events via the emit callback (the video backend's key queue). Polled once
// per frame. Kept out of SDL so the gsKit backend (no SDL event loop) works too.

#include <tamtypes.h>
#include <stdio.h>      // printf -> EE serial console (ps2_bootscr.c routes to SIO)
#include <loadfile.h>   // SifLoadModule
#include <libpad.h>

#include "doomkeys.h"
#include "d_event.h"    // event_t, ev_joystick, D_PostEvent
#include "m_config.h"   // M_BindVariable (persistence)

// Log controller input to the EE serial console (PCSX2 log) for debugging:
// button press/release edges and analog direction changes (not per-frame, to
// avoid flooding). Set to 0 to silence.
#define PAD_LOG 1

// --- live settings (bound to the config file by PS2Pad_BindConfig) ----------
// Non-static so m_config (bind) and m_menu (the Controller page) can reach them.
int ps2_turn_sensitivity = 10;   // 1..20
int ps2_always_run       = 1;    // 0/1  (default ON -- modern feel)
int ps2_stick_deadzone   = 40;   // px around centre (128)
int ps2_invert_y         = 0;    // 0/1
int ps2_southpaw         = 0;    // 0/1

static char g_padBuf[256] __attribute__((aligned(64)));
static int  g_inited  = 0;
static int  g_analog  = 0;       // analog mode requested yet?
static u16  g_prev    = 0xFFFF;  // button state, active-low (1 == released)
static int  g_run     = 0;       // "run" currently held (stick or always-run)?
static int  g_first   = 1;       // first poll: adopt the held state, emit no edges

#define RUNMAG 88                // stick deflection past this = run (when not always-run)

// Register the controller settings with Doom's config system so they save to
// and load from default.cfg. Called from D_BindVariables (before M_LoadDefaults).
void PS2Pad_BindConfig(void)
{
    M_BindVariable("ps2_turn_sensitivity", &ps2_turn_sensitivity);
    M_BindVariable("ps2_always_run",       &ps2_always_run);
    M_BindVariable("ps2_stick_deadzone",   &ps2_stick_deadzone);
    M_BindVariable("ps2_invert_y",         &ps2_invert_y);
    M_BindVariable("ps2_southpaw",         &ps2_southpaw);
}

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
    { PAD_R2,       KEY_FIRE       },   // R2: shoot
    { PAD_SQUARE,   KEY_USE        },   // []: open/use (alongside X)
    { PAD_CROSS,    KEY_USE        },   // X: open/use (in-game) ...
    { PAD_CROSS,    KEY_ENTER      },   //    ... and CONFIRM (menus)
    { PAD_L2,       KEY_RSHIFT     },   // L2: run (hold)
    { PAD_CIRCLE,   KEY_BACKSPACE  },   // O: BACK one menu level (close at top)
    { PAD_START,    KEY_ESCAPE     },   // Start: open / close-at-top only
    { PAD_SELECT,   KEY_TAB        },   // automap
    // Triangle = jump (key_jump) -- emitted below, it's a runtime-bound key.
};

static int axis(unsigned char v)   // 0..255, centre 128 -> -1 / 0 / +1
{
    int dz = ps2_stick_deadzone;
    if (v > 128 + dz) return 1;
    if (v < 128 - dz) return -1;
    return 0;
}

// Proportional right-stick turn. joyxmove is the signed, deadzoned stick value
// (~ -128..127) carried in ev_joystick.data2; g_game.c's BuildTiccmd subtracts
// this from cmd->angleturn (positive => turn right). Scaled by sensitivity:
// full deflection at sensitivity 10 ~= vanilla "fast" turn (1280 BAM/tic).
int PS2_JoyTurn(int joyxmove)
{
    long t = (long)joyxmove * 1280 * ps2_turn_sensitivity;
    return (int)(t / (128 * 10));
}

// Is START or X currently held? Used by the fatal-error screen (ps2_bootscr.c)
// to wait for acknowledgement. Reads the already-open port; safe if called
// before the first PS2Pad_Poll (init is lazy).
int PS2Pad_AnyPressed(void)
{
    struct padButtonStatus btn;
    int s;

    PS2Pad_Init();
    s = padGetState(0, 0);
    if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1)
        return 0;
    if (padRead(0, 0, &btn) == 0)
        return 0;
    // active-low: a 0 bit means the button is down.
    return ((btn.btns & PAD_START) == 0) || ((btn.btns & PAD_CROSS) == 0);
}

void PS2Pad_Poll(void (*emit)(int pressed, unsigned char doomkey))
{
    struct padButtonStatus btn;
    event_t ev;
    int s, i, want_run, mag, vmag;
    int turn_h, move_h, move_v, fwd;
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

    // Pick which stick turns vs moves (southpaw swaps them).
    if (ps2_southpaw)
    {
        turn_h = btn.ljoy_h;            // left stick turns
        move_h = btn.rjoy_h;            // right stick moves/strafes
        move_v = btn.rjoy_v;
    }
    else
    {
        turn_h = btn.rjoy_h;            // right stick turns
        move_h = btn.ljoy_h;            // left stick moves/strafes
        move_v = btn.ljoy_v;
    }

    // --- analog movement -> ev_joystick --------------------------------------
    // forward: up (small value) = forward; Doom wants joyymove < 0 to go
    // forward, so axis() maps up -> -1 naturally. invert_y flips that.
    fwd = axis(move_v);
    if (ps2_invert_y) fwd = -fwd;

    // turn: proportional signed magnitude, consumed by PS2_JoyTurn. Past the
    // deadzone we RESCALE [dz..127] -> [0..127] so crossing the deadzone starts
    // from zero instead of snapping to ~a third of full turn, then apply a mild
    // ease-in so small deflections aim finely while full throw still hits max
    // speed. (The old raw-deflection version felt twitchy / too strong.)
    {
        int t    = turn_h - 128;                  // -128..127
        int dz   = ps2_stick_deadzone;
        int sign = (t < 0) ? -1 : 1;
        int mag  = (t < 0) ? -t : t;

        if (mag <= dz || dz >= 127)
        {
            t = 0;
        }
        else
        {
            mag = (mag - dz) * 127 / (127 - dz);  // rescale to 0..127
            mag = (mag + mag * mag / 127) / 2;     // ease-in: half linear, half square
            t   = sign * mag;
        }
        ev.data2 = t;                  // turn (right stick X) -- PROPORTIONAL
    }
    ev.type  = ev_joystick;
    ev.data1 = 0;                      // actions come in as keys, not joybuttons
    ev.data3 = fwd;                    // forward/back (sign)
    ev.data4 = axis(move_h);           // strafe (sign)
    D_PostEvent(&ev);

    // Run: always-run, else push the move stick far. Edge-triggered so the key
    // queue isn't spammed.
    mag  = move_h > 128 ? move_h - 128 : 128 - move_h;
    vmag = move_v > 128 ? move_v - 128 : 128 - move_v;
    if (vmag > mag) mag = vmag;
    want_run = ps2_always_run ? 1 : (mag > RUNMAG);
    if (want_run != g_run)
    {
        emit(want_run, KEY_RSHIFT);
        g_run = want_run;
    }

    // --- buttons -> keys -----------------------------------------------------
    now     = btn.btns;          // active-low: a 0 bit means that button is down
    if (g_first)                 // adopt whatever's held at boot (e.g. the X that
    {                            // launched the setup menu) so it isn't seen as a
        g_prev  = now;           // fresh press -> no phantom ENTER opening the menu
        g_first = 0;
    }
    changed = g_prev ^ now;

    for (i = 0; i < (int) (sizeof(g_map) / sizeof(g_map[0])); ++i)
        if (changed & g_map[i].mask)
        {
            int down = (now & g_map[i].mask) == 0;
            emit(down, g_map[i].key);
#if PAD_LOG
            printf("pad: btn mask=0x%04x key=%d %s\n",
                   g_map[i].mask, g_map[i].key, down ? "down" : "up");
#endif
        }

#if PAD_LOG
    // Analog direction changes only (turn/forward/strafe sign), not every frame.
    {
        static int p_turn = 0, p_fwd = 0, p_str = 0;
        int t = (ev.data2 > 0) - (ev.data2 < 0);
        if (t != p_turn || fwd != p_fwd || ev.data4 != p_str)
        {
            printf("pad: turn=%d fwd=%d strafe=%d (run=%d sens=%d)\n",
                   ev.data2, fwd, ev.data4, g_run, ps2_turn_sensitivity);
            p_turn = t; p_fwd = fwd; p_str = ev.data4;
        }
    }
#endif

    // X also confirms yes/no prompts. Those only accept key_menu_confirm
    // (default 'y'), KEY_ESCAPE, space or abort -- NOT KEY_ENTER -- so without
    // this the controller can't confirm "quit to DOS", overwrite-save, etc.
    // (the pad has no 'y' key). Emit the actual configured confirm key.
    {
        extern int key_menu_confirm;
        if (changed & PAD_CROSS)
            emit((now & PAD_CROSS) == 0, (unsigned char) key_menu_confirm);
    }

    // Triangle = jump (key_jump). No effect unless jump is enabled on the setup
    // menu (p_user.c gates it); runtime-bound key, so not in the static map.
    {
        extern int key_jump;
        if (changed & PAD_TRIANGLE)
            emit((now & PAD_TRIANGLE) == 0, (unsigned char) key_jump);
    }

    // L1 / R1: previous / next weapon. Drive Doom's real owned-weapon cycle
    // (G_NextWeapon -- skips weapons you don't have, includes the SSG) via
    // key_prevweapon/key_nextweapon, instead of blindly tapping number keys
    // (which selected weapons you might not own and desynced).
    {
        extern int key_prevweapon, key_nextweapon;
        if ((changed & PAD_L1) && (now & PAD_L1) == 0)
        {
            emit(1, (unsigned char) key_prevweapon);
            emit(0, (unsigned char) key_prevweapon);
        }
        if ((changed & PAD_R1) && (now & PAD_R1) == 0)
        {
            emit(1, (unsigned char) key_nextweapon);
            emit(0, (unsigned char) key_nextweapon);
        }
    }

    g_prev = now;
}
