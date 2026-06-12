// PS2 SPU2 hardware-synth music backend (EE side) -- milestone S3.
//
// A music_module_t alternative to the OPL backend. Instead of software-
// rendering FM into the audsrv stream, it parses the MIDI song (reusing
// midifile.c + mus2mid), flattens every track into a timed event stream
// (applying tempo), and ships it over SIF RPC to spusynth.irx, which sequences
// it on the IOP and drives the SPU2's hardware ADPCM voices. See
// [[spu2-synth-routing]] and ps2/iop/spusynth/.

#include <stdio.h>
#include <string.h>
#include <stdint.h>           // uint64_t
#include <kernel.h>            // FlushCache
#include <sifrpc.h>           // sceSif* RPC (EE client)
#include <loadfile.h>         // SifExecModuleBuffer

#include "doomtype.h"
#include "i_sound.h"
#include "midifile.h"
#include "mus2mid.h"
#include "memio.h"

#include "iop/spusynth/spusynth_rpc.h"

#define MAXMIDLENGTH (4 * 1024 * 1024)
#define MAX_TRACKS   64

// Embedded IRX (bin2c: spusynth_irx.c).
extern unsigned char spusynth_irx[];
extern unsigned int  size_spusynth_irx;

static struct t_SifRpcClientData cd0 __attribute__((aligned(64)));
static unsigned int sbuff[2048] __attribute__((aligned(64)));  // RPC send/recv

static boolean spu2_initialised = false;
static boolean spu2_bound = false;
static boolean spu2_playing = false;

// ---------------------------------------------------------------------------
// RPC plumbing
// ---------------------------------------------------------------------------

static boolean BindServer(void)
{
    int tries = 0;

    if (spu2_bound)
        return true;

    while (tries++ < 1000)
    {
        if (sceSifBindRpc(&cd0, SPUSYNTH_RPC_ID, 0) < 0)
            return false;
        if (cd0.server != 0)
        {
            spu2_bound = true;
            return true;
        }
        // server not up yet -- spin briefly
        { volatile int i; for (i = 0; i < 100000; ++i) ; }
    }
    return false;
}

// Single-word command (RESET/PLAY/STOP/VOLUME).
static void RpcCmd(int func, unsigned int arg)
{
    if (!spu2_bound) return;
    sbuff[0] = arg;
    sceSifCallRpc(&cd0, func, 0, sbuff, 4, sbuff, 4, NULL, NULL);
}

// ---------------------------------------------------------------------------
// MIDI -> flattened event stream, batched to the IOP over LOAD calls
// ---------------------------------------------------------------------------

static spusynth_ev_t ev_batch[SPUSYNTH_LOAD_MAX];
static int ev_batch_n;

static void FlushBatch(void)
{
    int words;
    if (ev_batch_n == 0 || !spu2_bound) { ev_batch_n = 0; return; }

    sbuff[0] = (unsigned int) ev_batch_n;
    memcpy(&sbuff[1], ev_batch, ev_batch_n * sizeof(spusynth_ev_t));
    words = 1 + ev_batch_n * 2;                 // 1 count word + 2 words/event
    sceSifCallRpc(&cd0, SPUSYNTH_LOAD, 0, sbuff, words * 4, sbuff, 4, NULL, NULL);
    ev_batch_n = 0;
}

static void Emit(unsigned int delay_us, int cmd, int chan, int note, int vel)
{
    spusynth_ev_t *e = &ev_batch[ev_batch_n];
    e->delay_us = delay_us;
    e->cmd      = (unsigned char) cmd;
    e->chan     = (unsigned char) chan;
    e->note     = (unsigned char) note;
    e->vel      = (unsigned char) vel;
    if (++ev_batch_n >= SPUSYNTH_LOAD_MAX)
        FlushBatch();
}

// Walk all tracks in absolute-tick order, convert tick deltas to microseconds
// using the running tempo, and emit note on/off events to the IOP.
static void FlattenAndPlay(midi_file_t *file, boolean looping)
{
    unsigned int ntracks = MIDI_NumTracks(file);
    unsigned int tpq     = MIDI_GetFileTimeDivision(file);
    midi_track_iter_t *iter[MAX_TRACKS];
    unsigned int next_tick[MAX_TRACKS];
    int          active[MAX_TRACKS];
    unsigned int us_per_beat = 500000;          // default 120 BPM
    unsigned int last_tick = 0;
    unsigned int pending_us = 0;
    unsigned int t;

    if (ntracks > MAX_TRACKS) ntracks = MAX_TRACKS;
    if (tpq == 0) tpq = 96;

    for (t = 0; t < ntracks; ++t)
    {
        iter[t] = MIDI_IterateTrack(file, t);
        next_tick[t] = MIDI_GetDeltaTime(iter[t]);   // abs tick of first event
        active[t] = (iter[t] != NULL);
    }

    RpcCmd(SPUSYNTH_RESET, 0);
    ev_batch_n = 0;

    for (;;)
    {
        unsigned int best = 0xFFFFFFFFu, at;
        int sel = -1;
        midi_event_t *ev;

        for (t = 0; t < ntracks; ++t)
            if (active[t] && next_tick[t] <= best) { best = next_tick[t]; sel = t; }
        if (sel < 0) break;

        at = next_tick[sel];
        pending_us += (unsigned int) (((uint64_t)(at - last_tick) * us_per_beat) / tpq);
        last_tick = at;

        if (!MIDI_GetNextEvent(iter[sel], &ev)) { active[sel] = 0; continue; }

        switch (ev->event_type)
        {
            case MIDI_EVENT_NOTE_ON:
                if (ev->data.channel.param2 > 0)
                    Emit(pending_us, SPUSYNTH_EV_NOTE_ON, ev->data.channel.channel,
                         ev->data.channel.param1, ev->data.channel.param2);
                else
                    Emit(pending_us, SPUSYNTH_EV_NOTE_OFF, ev->data.channel.channel,
                         ev->data.channel.param1, 0);
                pending_us = 0;
                break;

            case MIDI_EVENT_NOTE_OFF:
                Emit(pending_us, SPUSYNTH_EV_NOTE_OFF, ev->data.channel.channel,
                     ev->data.channel.param1, 0);
                pending_us = 0;
                break;

            case MIDI_EVENT_PROGRAM_CHANGE:
                Emit(pending_us, SPUSYNTH_EV_PROGRAM, ev->data.channel.channel,
                     ev->data.channel.param1, 0);
                pending_us = 0;
                break;

            case MIDI_EVENT_META:
                if (ev->data.meta.type == MIDI_META_SET_TEMPO && ev->data.meta.length == 3)
                {
                    byte *d = ev->data.meta.data;
                    us_per_beat = (d[0] << 16) | (d[1] << 8) | d[2];
                }
                else if (ev->data.meta.type == MIDI_META_END_OF_TRACK)
                {
                    active[sel] = 0;
                }
                break;

            default:
                break;   // controllers / pitch bend: ignored for now
        }

        next_tick[sel] = at + MIDI_GetDeltaTime(iter[sel]);
    }

    Emit(0, SPUSYNTH_EV_END, 0, 0, 0);
    FlushBatch();

    for (t = 0; t < ntracks; ++t)
        if (iter[t]) MIDI_FreeIterator(iter[t]);

    RpcCmd(SPUSYNTH_PLAY, looping ? 1 : 0);
    spu2_playing = true;
}

// ---------------------------------------------------------------------------
// music_module_t implementation
// ---------------------------------------------------------------------------

static boolean I_SPU2_InitMusic(void)
{
    int ret = 0;

    // audsrv (which loads libsd.irx and powers the SPU2) must already be up;
    // it is brought up by the SDL audio backend before music init.
    if (SifExecModuleBuffer(spusynth_irx, size_spusynth_irx, 0, NULL, &ret) < 0)
    {
        fprintf(stderr, "I_SPU2_InitMusic: spusynth.irx load FAILED\n");
        return false;
    }
    printf("spu2music: spusynth.irx loaded (ret=%d)\n", ret);

    if (!BindServer())
    {
        fprintf(stderr, "I_SPU2_InitMusic: RPC bind FAILED\n");
        return false;
    }
    printf("spu2music: RPC bound -- SPU2 synth ready\n");

    spu2_initialised = true;
    return true;
}

static void I_SPU2_ShutdownMusic(void)
{
    if (spu2_initialised)
        RpcCmd(SPUSYNTH_STOP, 0);
    spu2_playing = false;
}

static void I_SPU2_SetMusicVolume(int volume)
{
    // DOOM volume 0..127 -> SPU2 master 0..0x3FFF.
    unsigned int v = ((unsigned int) volume * 0x3FFF) / 127;
    if (v > 0x3FFF) v = 0x3FFF;
    RpcCmd(SPUSYNTH_VOLUME, v);
}

static void I_SPU2_PauseSong(void)  { RpcCmd(SPUSYNTH_STOP, 0); spu2_playing = false; }
static void I_SPU2_ResumeSong(void) { RpcCmd(SPUSYNTH_PLAY, 1); spu2_playing = true;  }

static boolean IsMid(byte *mem, int len)
{
    return len > 4 && !memcmp(mem, "MThd", 4);
}

static void *I_SPU2_RegisterSong(void *data, int len)
{
    // Load the song into a midi_file_t entirely from memory (no writable FS):
    // MIDI loads straight from the buffer; MUS is converted in a memio buffer.
    midi_file_t *result = NULL;
    FILE *f;

    if (!spu2_initialised)
        return NULL;

    if (IsMid((byte *) data, len) && len < MAXMIDLENGTH)
    {
        f = fmemopen(data, len, "rb");
        if (f != NULL) { result = MIDI_LoadStream(f); fclose(f); }
    }
    else
    {
        MEMFILE *instream  = mem_fopen_read(data, len);
        MEMFILE *outstream = mem_fopen_write();

        if (mus2mid(instream, outstream) == 0)
        {
            void *midi_data; size_t midi_len;
            mem_get_buf(outstream, &midi_data, &midi_len);
            f = fmemopen(midi_data, midi_len, "rb");
            if (f != NULL) { result = MIDI_LoadStream(f); fclose(f); }
        }
        mem_fclose(instream);
        mem_fclose(outstream);
    }

    if (result == NULL)
        fprintf(stderr, "I_SPU2_RegisterSong: failed to load MIDI\n");

    return result;
}

static void I_SPU2_UnRegisterSong(void *handle)
{
    if (handle != NULL)
        MIDI_FreeFile((midi_file_t *) handle);
}

static void I_SPU2_PlaySong(void *handle, boolean looping)
{
    if (!spu2_initialised || handle == NULL)
        return;
    FlattenAndPlay((midi_file_t *) handle, looping);
}

static void I_SPU2_StopSong(void)
{
    RpcCmd(SPUSYNTH_STOP, 0);
    spu2_playing = false;
}

static boolean I_SPU2_MusicIsPlaying(void)
{
    return spu2_playing;
}

static snddevice_t music_spu2_devices[] =
{
    SNDDEVICE_GENMIDI,
    SNDDEVICE_GUS,
};

music_module_t music_spu2_module =
{
    music_spu2_devices,
    arrlen(music_spu2_devices),
    I_SPU2_InitMusic,
    I_SPU2_ShutdownMusic,
    I_SPU2_SetMusicVolume,
    I_SPU2_PauseSong,
    I_SPU2_ResumeSong,
    I_SPU2_RegisterSong,
    I_SPU2_UnRegisterSong,
    I_SPU2_PlaySong,
    I_SPU2_StopSong,
    I_SPU2_MusicIsPlaying,
    NULL,   // Poll: the IOP sequences, nothing to do on the EE
};
