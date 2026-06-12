// spusynth.irx -- PS2 SPU2 hardware synth (IOP side).
//
// Drives the SPU2's hardware ADPCM voices via libsd (sceSd*). It does NOT
// initialise the SPU2 itself: the EE first brings up the proven audsrv stack
// (which powers the chip), and this module keys voices on the now-live chip.
//
// S1: one tone voice (CORE 1 + MMIX SndL|SndR = 0xC00).
// S2: polyphony + sceSdNote2Pitch + ADSR. libsd gotcha: sceSdVoiceTrans uses
//     the *pointer value* of spuaddr as the SPU byte address (no deref), so
//     upload with (u32*)addr, addr < 64 KB. See [[spu2-synth-routing]].
// S3: SIF RPC server + IOP sequencer plays a flattened MIDI event stream.
// S4 (this file): a small synthesized waveform bank (square/saw/triangle/sine/
//     pulse) + a GM-family patch map. PROGRAM_CHANGE picks each channel's
//     waveform + ADSR; channel 9 plays the noise generator as percussion.

#include <irx.h>
#include <loadcore.h>
#include <stdio.h>
#include <thbase.h>
#include <sifcmd.h>
#include <sysclib.h>          // memcpy
#include <libsd.h>

#include "spusynth_rpc.h"

IRX_ID("spusynth", 1, 4);

// --- SPU2 routing ----------------------------------------------------------
#define CORE         1
#define MMIX_SND_DRY 0xC00

// --- voice pool ------------------------------------------------------------
#define FIRST_VOICE  1
#define NPOLY        20
#define VOICE_MASK   (((1u << NPOLY) - 1u) << FIRST_VOICE)

// --- waveform bank ---------------------------------------------------------
// Each waveform is NBLK identical 1-cycle blocks (28 samples) looped, giving a
// ~1714 Hz base at unity pitch (~MIDI note 92.5) so sceSdNote2Pitch tunes them.
#define NBLK       64
#define WAVE_BYTES (NBLK * 16)
#define BASE_NOTE  92
#define BASE_FINE  64

enum { W_SQUARE, W_SAW, W_TRI, W_SINE, W_PULSE, W_NOISE, NWAVES };

// Distinct SPU RAM addresses (1 KB apart, all < 64 KB for the IO upload path).
static const u32 wave_addr[NWAVES] = { 0x5000, 0x5400, 0x5800, 0x5C00, 0x6000, 0x6400 };

static u8 wavebuf[WAVE_BYTES] __attribute__((aligned(64)));

// One cycle of sine as signed 4-bit nibbles (28 samples, peak +-7).
static const signed char sine_tab[28] = {
    0, 2, 3, 4, 5, 6, 7, 7, 7, 6, 5, 4, 3, 2,
    0,-2,-3,-4,-5,-6,-7,-7,-7,-6,-5,-4,-3,-2
};

static signed char cycle_nibble(int kind, int i)   // i = 0..27 -> -8..7
{
    switch (kind)
    {
        case W_SQUARE: return i < 14 ? 7 : -8;
        case W_SAW:    return (signed char)(-8 + (i * 15) / 27);
        case W_TRI:    return i < 14 ? (signed char)(-8 + (i * 15) / 13)
                                     : (signed char)( 7 - ((i - 14) * 15) / 13);
        case W_PULSE:  return i < 7 ? 7 : -8;          // 25% duty
        case W_SINE:   return sine_tab[i];
    }
    return 0;
}

static unsigned int rng_state = 0x13572468;
static u8 rnd_byte(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (u8)(rng_state >> 16);     // two random nibbles
}

static void build_and_upload_wave(int kind)
{
    int b, j;

    if (kind == W_NOISE)
    {
        // White noise: every block holds *different* random nibbles (not a
        // replicated cycle), so the looped sample sounds like noise rather than
        // a pitched buzz. Played by percussion with a fast-decay envelope.
        for (b = 0; b < NBLK; b++)
        {
            u8 *blk = wavebuf + b * 16;
            blk[0] = 0x00;
            blk[1] = (b == 0)        ? ADPCM_LOOP_START
                   : (b == NBLK - 1) ? (ADPCM_LOOP_END | ADPCM_LOOP)
                   :                    ADPCM_LOOP;
            for (j = 0; j < 14; j++) blk[2 + j] = rnd_byte();
        }
    }
    else
    {
        u8 cyc[14];
        for (j = 0; j < 14; j++)
        {
            signed char n0 = cycle_nibble(kind, 2 * j);
            signed char n1 = cycle_nibble(kind, 2 * j + 1);
            cyc[j] = (u8)((n0 & 0x0F) | ((n1 & 0x0F) << 4));
        }
        for (b = 0; b < NBLK; b++)
        {
            u8 *blk = wavebuf + b * 16;
            blk[0] = 0x00;                              // shift 0, filter 0
            blk[1] = (b == 0)        ? ADPCM_LOOP_START
                   : (b == NBLK - 1) ? (ADPCM_LOOP_END | ADPCM_LOOP)
                   :                    ADPCM_LOOP;
            memcpy(blk + 2, cyc, 14);
        }
    }

    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_IO,
                    wavebuf, (u32 *) wave_addr[kind], WAVE_BYTES);
    sceSdVoiceTransStatus(0, 1);
}

// --- envelope presets ------------------------------------------------------
#define A1_SUS   SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0x00, 0x00, 0x0F)
#define A2_SUS   SD_SET_ADSR2(SD_ADSR_SR_EXPd, 0x7F, SD_ADSR_RR_LINEARd, 0x0C)
#define A1_PLUCK SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0x00, 0x0A, 0x04)
#define A2_PLUCK SD_SET_ADSR2(SD_ADSR_SR_LINEARd, 0x14, SD_ADSR_RR_LINEARd, 0x10)
#define A1_SOFT  SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0x40, 0x00, 0x0F)
#define A2_SOFT  A2_SUS
// Drums: instant hit at full volume, no decay (full sustain) so the noise burst
// is loud, with a release on note-off. The drum's note duration shapes it.
// (A decay-to-low-sustain version sounded worse -- too quiet.)
#define A1_DRUM  SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0x00, 0x00, 0x0F)
#define A2_DRUM  SD_SET_ADSR2(SD_ADSR_SR_EXPd, 0x7F, SD_ADSR_RR_LINEARd, 0x0C)

// --- GM-family patch map (program >> 3 -> waveform + envelope) -------------
typedef struct { u8 wave; u16 a1; u16 a2; } patch_t;

static const patch_t family_patch[16] = {
    { W_TRI,   A1_PLUCK, A2_PLUCK },  // 0  Piano
    { W_SQUARE,A1_PLUCK, A2_PLUCK },  // 1  Chromatic percussion
    { W_SQUARE,A1_SUS,   A2_SUS   },  // 2  Organ
    { W_SAW,   A1_PLUCK, A2_PLUCK },  // 3  Guitar
    { W_TRI,   A1_SUS,   A2_SUS   },  // 4  Bass
    { W_SAW,   A1_SOFT,  A2_SOFT  },  // 5  Strings
    { W_SAW,   A1_SOFT,  A2_SOFT  },  // 6  Ensemble
    { W_PULSE, A1_SUS,   A2_SUS   },  // 7  Brass
    { W_SQUARE,A1_SUS,   A2_SUS   },  // 8  Reed
    { W_SINE,  A1_SUS,   A2_SUS   },  // 9  Pipe
    { W_SAW,   A1_SUS,   A2_SUS   },  // 10 Synth lead
    { W_SINE,  A1_SOFT,  A2_SOFT  },  // 11 Synth pad
    { W_SINE,  A1_SUS,   A2_SUS   },  // 12 Synth FX
    { W_SAW,   A1_PLUCK, A2_PLUCK },  // 13 Ethnic
    { W_TRI,   A1_PLUCK, A2_PLUCK },  // 14 Percussive
    { W_SQUARE,A1_PLUCK, A2_PLUCK },  // 15 Sound FX
};

// ===========================================================================
// Raw voice control
// ===========================================================================

// fixed_pitch != 0 overrides the note->pitch mapping (used by percussion, whose
// MIDI "notes" are drum selectors, not pitches to transpose).
static void voice_key_on(int voice, int midinote, int vol,
                         u32 addr, u16 a1, u16 a2, u16 fixed_pitch)
{
    int e = SD_VOICE(CORE, voice);
    u16 pitch = fixed_pitch ? fixed_pitch
                            : sceSdNote2Pitch(BASE_NOTE, BASE_FINE, (u16) midinote, 0);

    sceSdSetParam(e | SD_VPARAM_VOLL,  (u16) vol);
    sceSdSetParam(e | SD_VPARAM_VOLR,  (u16) vol);
    sceSdSetParam(e | SD_VPARAM_PITCH, pitch);
    sceSdSetParam(e | SD_VPARAM_ADSR1, a1);
    sceSdSetParam(e | SD_VPARAM_ADSR2, a2);
    sceSdSetAddr (e | SD_VADDR_SSA,    addr);
    sceSdSetSwitch(SD_SWITCH_KON | CORE, 1u << voice);
}

// Map a GM drum note to a fixed playback pitch so the noise burst lands in an
// audible, percussive range (kick low, snare ~unity, hats bright).
static u16 drum_pitch(int note)
{
    if (note <= 37) return 0x0480;   // kick / low toms (deep thud)
    if (note <= 47) return 0x0900;   // snare / mid toms
    return 0x1000;                   // hats / cymbals (unity; was too bright)
}

static void voice_key_off(int voice)
{
    sceSdSetSwitch(SD_SWITCH_KOFF | CORE, 1u << voice);
}

// ===========================================================================
// Voice allocator + per-channel program state
// ===========================================================================

static signed char  vc_note[NPOLY];
static unsigned char vc_chan[NPOLY];
static unsigned int  vc_age[NPOLY];
static unsigned int  vc_serial;

static unsigned char chan_program[16];   // current GM program per MIDI channel

static void alloc_reset(void)
{
    int i;
    for (i = 0; i < NPOLY; i++) { vc_note[i] = -1; vc_chan[i] = 0; vc_age[i] = 0; }
    for (i = 0; i < 16; i++) chan_program[i] = 0;
    vc_serial = 0;
}

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

    if (vel <= 0) return;

    for (i = 0; i < NPOLY; i++)
        if (vc_note[i] < 0) { pick = i; goto got; }
    for (i = 0; i < NPOLY; i++)
        if (vc_age[i] < oldest) { oldest = vc_age[i]; pick = i; }
got:
    vc_note[pick] = (signed char) note;
    vc_chan[pick] = (unsigned char) chan;
    vc_age[pick]  = ++vc_serial;

    if (chan == 9)
    {
        // Percussion: the sampled-noise waveform at a fixed per-drum pitch
        // (NOT transposed by the note), fast-decaying.
        static int dbg_drum;
        if (dbg_drum < 12)
        {
            printf("spusynth: DRUM note=%d pitch=0x%x vol=%d voice=%d\n",
                   note, drum_pitch(note), vel_to_vol(vel), FIRST_VOICE + pick);
            dbg_drum++;
        }
        voice_key_on(FIRST_VOICE + pick, note, vel_to_vol(vel),
                     wave_addr[W_NOISE], A1_DRUM, A2_DRUM, drum_pitch(note));
    }
    else
    {
        const patch_t *p = &family_patch[chan_program[chan] >> 3];
        voice_key_on(FIRST_VOICE + pick, note, vel_to_vol(vel),
                     wave_addr[p->wave], p->a1, p->a2, 0);
    }
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
static volatile int  g_play = 0;
static volatile int  g_loop = 0;
static volatile int  g_gen  = 0;

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
                case SPUSYNTH_EV_PROGRAM:  chan_program[ev.chan & 15] = ev.note;    break;
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
// SIF RPC server
// ===========================================================================

static SifRpcDataQueue_t   g_qd;
static SifRpcServerData_t  g_sd;
static int g_rpc_buf[2048] __attribute__((aligned(64)));

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
            unsigned int n = u[0], k;
            spusynth_ev_t *src = (spusynth_ev_t *) &u[1];
            for (k = 0; k < n && g_song_count < MAX_EVENTS; k++)
                g_song[g_song_count++] = src[k];
            break;
        }

        case SPUSYNTH_PLAY:
            g_loop = (int) u[0]; g_play = 1; g_gen++;
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
    int w;

    (void) argc; (void) argv;
    printf("spusynth: start (S4 GM bank, core %d, %d voices)\n", CORE, NPOLY);

    // SPU2 is already up (audsrv). Assert core 1 master volume + voice gates.
    sceSdSetParam(CORE | SD_PARAM_MVOLL, 0x3FFF);
    sceSdSetParam(CORE | SD_PARAM_MVOLR, 0x3FFF);
    mmix = sceSdGetParam(CORE | SD_PARAM_MMIX);
    sceSdSetParam(CORE | SD_PARAM_MMIX, mmix | MMIX_SND_DRY);

    for (w = 0; w < NWAVES; w++)
        build_and_upload_wave(w);

    alloc_reset();

    sceSdSetSwitch(SD_SWITCH_VMIXL | CORE, VOICE_MASK);
    sceSdSetSwitch(SD_SWITCH_VMIXR | CORE, VOICE_MASK);

    spawn(seq_thread, 0x40, 0x1000);
    spawn(rpc_thread, 0x30, 0x1800);

    printf("spusynth: ready (%d waveforms, voices 0x%x)\n", NWAVES, VOICE_MASK);
    return MODULE_RESIDENT_END;
}
