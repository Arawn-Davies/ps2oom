// spusynth.irx -- PS2 SPU2 hardware synth (IOP side).
//
// Drives the SPU2's hardware ADPCM voices via libsd (sceSd*). It does NOT
// initialise the SPU2 itself: the EE first brings up the proven audsrv stack
// (which powers the chip on real hardware and under PCSX2), and this module
// keys voices on the now-live chip, sharing the same libsd.irx.
//
// S1: one tone voice sounds (CORE 1 + MMIX SndL|SndR = 0xC00).
// S2: polyphony + sceSdNote2Pitch + ADSR. The libsd gotcha that ate a session:
//     sceSdVoiceTrans uses the *pointer value* of spuaddr as the SPU byte
//     address (it never dereferences), so the sample must be uploaded with
//     (u32*)SPU_ADDR, not &var. See [[spu2-synth-routing]].
// S3 (this file): a SIF RPC server receives a flattened MIDI event stream from
//     the EE music module; a sequencer thread plays it with DelayThread,
//     allocating from a pool of CORE 1 voices (note->voice map, oldest-steal).

#include <irx.h>
#include <loadcore.h>      // MODULE_RESIDENT_END
#include <stdio.h>         // printf -> IOP console
#include <thbase.h>        // CreateThread/StartThread/DelayThread/GetThreadId
#include <sifcmd.h>        // sceSif* RPC
#include <sysclib.h>       // memcpy/memset
#include <libsd.h>         // sceSd*, SD_*, ADPCM_LOOP_* (via libsd-common.h)

#include "spusynth_rpc.h"

IRX_ID("spusynth", 1, 3);

// --- SPU2 routing (see [[spu2-synth-routing]]) -----------------------------
#define CORE         1          // audsrv's live DAC core
#define MMIX_SND_DRY 0xC00      // DryGate.SndL|SndR: route voices to L/R output

// --- voice pool ------------------------------------------------------------
#define FIRST_VOICE  1          // voice 0 left alone; audsrv streams via Ext
#define NPOLY        20         // 20-note polyphony (of core 1's 24 voices)
#define VOICE_MASK   (((1u << NPOLY) - 1u) << FIRST_VOICE)

// --- the shared looping square-wave sample (S4 replaces with real instrs) ---
#define NBLK     256
#define SMPBYTES (NBLK * 16)
#define SPU_ADDR 0x5000          // byte offset into SPU RAM (keep < 64 KB)

// The square's loop is 28 samples -> ~1714 Hz at unity pitch (~MIDI note 92.5),
// so telling sceSdNote2Pitch that centre yields correct musical frequencies.
#define BASE_NOTE 92
#define BASE_FINE 64

static u8 adpcm[SMPBYTES] __attribute__((aligned(64)));

// ===========================================================================
// Sample + raw voice control
// ===========================================================================

static void build_square(void)
{
    int b, i;
    for (b = 0; b < NBLK; b++)
    {
        u8 *blk = adpcm + b * 16;
        blk[0] = 0x00;                                   // shift 0, filter 0
        blk[1] = (b == 0)        ? ADPCM_LOOP_START
               : (b == NBLK - 1) ? (ADPCM_LOOP_END | ADPCM_LOOP)
               :                    ADPCM_LOOP;
        for (i = 0; i < 7; i++) blk[2 + i] = 0x77;       // 14 samples = +7
        for (i = 0; i < 7; i++) blk[9 + i] = 0x88;       // 14 samples = -8
    }
}

static void upload_sample(void)
{
    // libsd uses the *pointer value* of spuaddr as the destination byte address
    // (no deref), and the IO path casts it to u16 -- so pass (u32*)SPU_ADDR and
    // keep it < 64 KB.
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_IO,
                    adpcm, (u32 *) SPU_ADDR, SMPBYTES);
    sceSdVoiceTransStatus(0, 1);
}

// Key a specific SPU voice for a MIDI note at the given voice volume.
static void voice_key_on(int voice, int midinote, int vol)
{
    int e = SD_VOICE(CORE, voice);
    u16 pitch = sceSdNote2Pitch(BASE_NOTE, BASE_FINE, (u16) midinote, 0);

    sceSdSetParam(e | SD_VPARAM_VOLL,  (u16) vol);
    sceSdSetParam(e | SD_VPARAM_VOLR,  (u16) vol);
    sceSdSetParam(e | SD_VPARAM_PITCH, pitch);
    sceSdSetParam(e | SD_VPARAM_ADSR1, SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0x00, 0x00, 0x0F));
    sceSdSetParam(e | SD_VPARAM_ADSR2, SD_SET_ADSR2(SD_ADSR_SR_EXPd, 0x7F, SD_ADSR_RR_LINEARd, 0x0C));
    sceSdSetAddr (e | SD_VADDR_SSA,    SPU_ADDR);
    sceSdSetSwitch(SD_SWITCH_KON | CORE, 1u << voice);
}

static void voice_key_off(int voice)
{
    sceSdSetSwitch(SD_SWITCH_KOFF | CORE, 1u << voice);
}

// ===========================================================================
// Voice allocator: map (chan,note) -> a pooled SPU voice, steal oldest if full
// ===========================================================================

static signed char vc_note[NPOLY];   // MIDI note on this voice, -1 = free
static unsigned char vc_chan[NPOLY];
static unsigned int  vc_age[NPOLY];   // alloc serial, for oldest-steal
static unsigned int  vc_serial;

static void alloc_reset(void)
{
    int i;
    for (i = 0; i < NPOLY; i++) { vc_note[i] = -1; vc_chan[i] = 0; vc_age[i] = 0; }
    vc_serial = 0;
}

// Velocity (1..127) -> per-voice volume, with polyphony headroom so a handful
// of simultaneous full-scale squares don't clip the core mix.
static int vel_to_vol(int vel)
{
    int v = (vel * 0x0E00) / 127;
    if (v < 0) v = 0;
    if (v > 0x3FFF) v = 0x3FFF;
    return v;
}

static void synth_note_on(int chan, int note, int vel)
{
    int i, pick = 0;
    unsigned int oldest = 0xFFFFFFFFu;

    if (vel <= 0) { /* note-on vel 0 == note-off */ return; }

    for (i = 0; i < NPOLY; i++)         // prefer a free voice
        if (vc_note[i] < 0) { pick = i; goto got; }
    for (i = 0; i < NPOLY; i++)         // else steal the oldest
        if (vc_age[i] < oldest) { oldest = vc_age[i]; pick = i; }
got:
    vc_note[pick] = (signed char) note;
    vc_chan[pick] = (unsigned char) chan;
    vc_age[pick]  = ++vc_serial;
    voice_key_on(FIRST_VOICE + pick, note, vel_to_vol(vel));
}

static void synth_note_off(int chan, int note)
{
    int i;
    for (i = 0; i < NPOLY; i++)
        if (vc_note[i] == note && vc_chan[i] == chan)
        {
            voice_key_off(FIRST_VOICE + i);
            vc_note[i] = -1;
            return;
        }
}

static void synth_all_off(void)
{
    int i;
    for (i = 0; i < NPOLY; i++) { voice_key_off(FIRST_VOICE + i); vc_note[i] = -1; }
}

// ===========================================================================
// Song storage + sequencer thread
// ===========================================================================

#define MAX_EVENTS 20000

static spusynth_ev_t g_song[MAX_EVENTS];
static volatile int  g_song_count = 0;
static volatile int  g_play = 0;     // 1 while a song should be playing
static volatile int  g_loop = 0;
static volatile int  g_gen  = 0;     // bumped on PLAY/STOP/RESET to interrupt seq

// Delay in <=50 ms chunks so STOP/new-PLAY (g_gen change) is noticed promptly.
static void seq_wait(unsigned int us, int gen)
{
    while (us > 0 && g_gen == gen && g_play)
    {
        unsigned int chunk = us > 50000 ? 50000 : us;
        DelayThread(chunk);
        us -= chunk;
    }
}

static void seq_thread(void *arg)
{
    (void) arg;
    printf("spusynth: sequencer thread up\n");

    for (;;)
    {
        int gen, i;

        if (!g_play) { DelayThread(20000); continue; }

        gen = g_gen;
        synth_all_off();
        alloc_reset();

        for (i = 0; i < g_song_count && g_gen == gen && g_play; )
        {
            spusynth_ev_t ev = g_song[i];

            seq_wait(ev.delay_us, gen);
            if (g_gen != gen || !g_play) break;

            switch (ev.cmd)
            {
                case SPUSYNTH_EV_NOTE_ON:  synth_note_on(ev.chan, ev.note, ev.vel); break;
                case SPUSYNTH_EV_NOTE_OFF: synth_note_off(ev.chan, ev.note);        break;
                case SPUSYNTH_EV_END:
                    if (g_loop) { synth_all_off(); alloc_reset(); i = 0; continue; }
                    g_play = 0;
                    break;
            }
            i++;
        }

        if (g_gen == gen) { g_play = 0; synth_all_off(); }
    }
}

// ===========================================================================
// SIF RPC server: EE music module -> song load / transport control
// ===========================================================================

static SifRpcDataQueue_t   g_qd;
static SifRpcServerData_t  g_sd;
static int g_rpc_buf[2048] __attribute__((aligned(64)));   // 8 KB RPC buffer

static void *rpc_command(int func, void *data, int size)
{
    u32 *u = (u32 *) data;
    (void) size;

    switch (func)
    {
        case SPUSYNTH_RESET:
            g_play = 0; g_gen++; synth_all_off(); g_song_count = 0;
            break;

        case SPUSYNTH_LOAD:
        {
            unsigned int n = u[0];
            spusynth_ev_t *src = (spusynth_ev_t *) &u[1];
            unsigned int k;
            for (k = 0; k < n && g_song_count < MAX_EVENTS; k++)
                g_song[g_song_count++] = src[k];
            break;
        }

        case SPUSYNTH_PLAY:
            g_loop = (int) u[0];
            g_play = 1; g_gen++;
            break;

        case SPUSYNTH_STOP:
            g_play = 0; g_gen++; synth_all_off();
            break;

        case SPUSYNTH_VOLUME:
            sceSdSetParam(CORE | SD_PARAM_MVOLL, (u16) u[0]);
            sceSdSetParam(CORE | SD_PARAM_MVOLR, (u16) u[0]);
            break;
    }

    return data;
}

static void rpc_thread(void *arg)
{
    (void) arg;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&g_qd, GetThreadId());
    sceSifRegisterRpc(&g_sd, SPUSYNTH_RPC_ID, rpc_command, g_rpc_buf, NULL, NULL, &g_qd);
    printf("spusynth: RPC server registered (id 0x%x)\n", SPUSYNTH_RPC_ID);
    sceSifRpcLoop(&g_qd);
}

// ===========================================================================
// Bring-up
// ===========================================================================

static int spawn(void (*entry)(void *), int prio, int stack)
{
    iop_thread_t th;
    int tid;
    th.attr = TH_C; th.option = 0; th.thread = entry;
    th.stacksize = stack; th.priority = prio;
    tid = CreateThread(&th);
    if (tid > 0) StartThread(tid, NULL);
    return tid;
}

int _start(int argc, char **argv)
{
    u16 mmix;

    (void) argc; (void) argv;
    printf("spusynth: start (S3 RPC synth, core %d, %d voices)\n", CORE, NPOLY);

    build_square();
    alloc_reset();

    // SPU2 is already up (audsrv). Assert core 1 master volume and OR the
    // voice-dry gates into its mixer without disturbing audsrv's Ext routing.
    sceSdSetParam(CORE | SD_PARAM_MVOLL, 0x3FFF);
    sceSdSetParam(CORE | SD_PARAM_MVOLR, 0x3FFF);
    mmix = sceSdGetParam(CORE | SD_PARAM_MMIX);
    sceSdSetParam(CORE | SD_PARAM_MMIX, mmix | MMIX_SND_DRY);

    upload_sample();

    // Route the whole voice pool into the L/R mix once; ADSR gates each voice.
    sceSdSetSwitch(SD_SWITCH_VMIXL | CORE, VOICE_MASK);
    sceSdSetSwitch(SD_SWITCH_VMIXR | CORE, VOICE_MASK);

    spawn(seq_thread, 0x40, 0x1000);
    spawn(rpc_thread, 0x30, 0x1800);

    printf("spusynth: ready (voices 0x%x @ spu 0x%x)\n", VOICE_MASK, SPU_ADDR);
    return MODULE_RESIDENT_END;
}
