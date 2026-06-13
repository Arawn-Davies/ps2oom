// doomgeneric PS2 backend -- NATIVE VU1 + DMA video (M2, post-ps2gl).
//
// Brings the game up (libSDL2main entry, audsrv, the game loop, the boot
// console) and hands per-frame rendering to the native VU1+DMA world renderer
// in r_gl.c (GS Z-buffer, custom VU1 microprogram). ps2gl is gone -- its baked-in
// geometry buffer overflowed in heavy rooms; we now own the DMA packets.
//
// Input is intentionally not wired here yet (the demo runs without it). The 2D
// status bar / HUD overlay is composited on top in a later milestone.

#include <stdio.h>

#include <SDL.h>            // libSDL2main entry + SDL timer

extern "C" {
#include "doomkeys.h"
#include "doomgeneric.h"
}

extern "C" void BootScr_Begin(void);
extern "C" void BootScr_End(void);
extern "C" void PS2Audio_Init(void);
extern "C" void I_ResetBaseTime(void);
extern "C" volatile int g_audio_gate;   // i_audsrvsound.c
extern "C" void RGL_Init(void);         // r_gl.c -- native GS/VU bring-up + projection
extern "C" void RGL_DrawFrame(void);    // r_gl.c -- native world render (clear + draw + vsync)
extern "C" void PS2Pad_Poll(void (*emit)(int pressed, unsigned char doomkey));  // ps2_pad.c

#ifndef BOOT_LOG_HOLD_MS
#define BOOT_LOG_HOLD_MS 3000
#endif

static int    gfx_ready = 0;
static int    g_argc = 0;
static char **g_argv = 0;

// Pad -> Doom key events, queued once per frame and drained by DG_GetKey.
#define KQ_SIZE 64
static struct { int pressed; unsigned char key; } kq[KQ_SIZE];
static int kq_head, kq_tail;
static void kq_emit(int pressed, unsigned char key)
{
    int n = (kq_tail + 1) & (KQ_SIZE - 1);
    if (n != kq_head) { kq[kq_tail].pressed = pressed; kq[kq_tail].key = key; kq_tail = n; }
}

static void EnsureGfx(void)
{
    if (gfx_ready)
        return;

    SDL_InitSubSystem(SDL_INIT_TIMER | SDL_INIT_EVENTS);

    printf("\n>>> build %s %s  (native VU1+DMA video, M2) <<<\n",
           __DATE__, __TIME__);
    SDL_Delay(BOOT_LOG_HOLD_MS);
    I_ResetBaseTime();
    BootScr_End();

    RGL_Init();         // native GS + VU1 bring-up, projection (owns the GS now)

    gfx_ready = 1;
    g_audio_gate = 1;   // GS now shows the game -> start audio in sync
}

extern "C" void DG_Init(void) { /* native GS/VU is brought up lazily in EnsureGfx */ }

// USB keyboard via SDL events (WASD + arrows + the usual). On PCSX2 the host
// keyboard maps to the emulated PAD instead, so this is mainly for real USB
// keyboards; both paths feed the same queue.
static void poll_sdl(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        unsigned char k = 0;
        int pressed;
        if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP)
            continue;
        pressed = (e.type == SDL_KEYDOWN);
        switch (e.key.keysym.sym) {
            case SDLK_w: case SDLK_UP:          k = KEY_UPARROW;    break;
            case SDLK_s: case SDLK_DOWN:        k = KEY_DOWNARROW;  break;
            case SDLK_a: case SDLK_LEFT:        k = KEY_LEFTARROW;  break;
            case SDLK_d: case SDLK_RIGHT:       k = KEY_RIGHTARROW; break;
            case SDLK_q:                        k = KEY_STRAFE_L;   break;
            case SDLK_e:                        k = KEY_STRAFE_R;   break;
            case SDLK_LCTRL: case SDLK_RCTRL:   k = KEY_FIRE;       break;
            case SDLK_SPACE:                    k = KEY_USE;        break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: k = KEY_RSHIFT;     break;
            case SDLK_RETURN:                   k = KEY_ENTER;      break;
            case SDLK_ESCAPE:                   k = KEY_ESCAPE;     break;
            case SDLK_TAB:                      k = KEY_TAB;        break;
            case SDLK_y:                        k = 'y';            break;
            case SDLK_n:                        k = 'n';            break;
            default:
                if (e.key.keysym.sym >= SDLK_1 && e.key.keysym.sym <= SDLK_9)
                    k = (unsigned char)('1' + (e.key.keysym.sym - SDLK_1));
                break;
        }
        if (k)
            kq_emit(pressed, k);
    }
}

extern "C" void DG_DrawFrame(void)
{
    EnsureGfx();
    PS2Pad_Poll(kq_emit);   // DualShock (and PCSX2 host-key -> pad mapping)
    poll_sdl();             // USB keyboard via SDL (WASD)
    RGL_DrawFrame();
}

extern "C" void DG_SleepMs(uint32_t ms)   { SDL_Delay(ms); }
extern "C" uint32_t DG_GetTicksMs(void)   { return SDL_GetTicks(); }
extern "C" void DG_SetWindowTitle(const char *t) { (void) t; }
extern "C" int  DG_GetKey(int *pressed, unsigned char *key)
{
    if (kq_head == kq_tail)
        return 0;
    *pressed = kq[kq_head].pressed;
    *key     = kq[kq_head].key;
    kq_head  = (kq_head + 1) & (KQ_SIZE - 1);
    return 1;
}

extern "C" int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;

    BootScr_Begin();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n=== doomgeneric for PlayStation 2 (native VU1+DMA video, M2) ===\n");

    PS2Audio_Init();
    doomgeneric_Create(argc, argv);

    // Cap the render loop to ~60 fps (pglWaitForVSync already paces it; this is
    // belt-and-braces). Fixed cadence, resyncs on a long frame.
    {
        const uint32_t FRAME_MS = 1000 / 60;
        uint32_t next = SDL_GetTicks();
        for (;;)
        {
            doomgeneric_Tick();
            next += FRAME_MS;
            uint32_t now = SDL_GetTicks();
            if ((int32_t)(next - now) > 0)
                SDL_Delay((uint32_t)(next - now));
            else
                next = now;
        }
    }

    return 0;
}
