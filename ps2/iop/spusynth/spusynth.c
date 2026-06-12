// spusynth.irx -- PS2 SPU2 hardware synth (IOP side), libsd/sceSd version.
//
// Drives the SPU2's hardware ADPCM voices via libsd (sceSd*). Crucially it does
// NOT initialise the SPU2 itself: the EE side first brings up the proven audsrv
// stack (which powers + configures the SPU2 on real hardware and under PCSX2),
// and this module then keys extra voices on the now-live chip, sharing the same
// libsd.irx. That sidesteps the standalone bring-up that left a self-contained
// driver completely silent (and the IOP->SPU2 DMA timing out).
//
// MILESTONE S1: key one tone voice (square sample) + one noise voice, as a
// self-test, to confirm a hardware voice sounds before any RPC/MIDI is wired.

#include <irx.h>
#include <loadcore.h>      // MODULE_RESIDENT_END
#include <stdio.h>         // printf -> IOP console
#include <libsd.h>         // sceSd*, SD_*, ADPCM_LOOP_* (via libsd-common.h)

IRX_ID("spusynth", 1, 1);

// --- a hand-built PS-ADPCM square wave (see earlier notes) -----------------
#define NBLK     16
#define SMPBYTES (NBLK * 16)
#define SPU_ADDR 0x5000          // byte offset into the 2 MB SPU RAM

static u8 adpcm[SMPBYTES] __attribute__((aligned(64)));

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

// SPU2 core that actually reaches the DAC. audsrv makes CORE 1 the live
// output core (master volume = MAX, SFX streamed in via its external input)
// and *zeroes CORE 0's master volume*. Keying voices on core 0 (as the first
// attempt did) is therefore silent by construction -- so we use core 1.
#define CORE     1

// MMIX (per-core mixer) gate bits, from the SPU2 register decode:
//   0x800 = DryGate.SndL, 0x400 = DryGate.SndR  -> route the VOICES, dry, to
//   the L/R output. The first attempt wrote 0x00FF, which only sets the
//   Ext/Inp *input* gates (bits 0-7) -- the voices were generated but never
//   mixed to output. These two bits are the fix.
#define MMIX_SND_DRY 0xC00

int _start(int argc, char **argv)
{
    u32 spu = SPU_ADDR;
    u32 m1  = 1u << 1;   // tone  -> voice 1
    u32 m2  = 1u << 2;   // noise -> voice 2  (audsrv streams via Ext, no voices)
    u16 mmix;

    (void) argc; (void) argv;
    printf("spusynth: start (sceSd, piggyback on audsrv, core %d)\n", CORE);

    build_square();

    // SPU2 is already initialised by audsrv. Re-assert core 1 master volume
    // (audsrv set it to MAX) and OR the voice-dry gates into its mixer so our
    // voices reach the output without disturbing audsrv's Ext (SFX) routing.
    sceSdSetParam(CORE | SD_PARAM_MVOLL, 0x3FFF);
    sceSdSetParam(CORE | SD_PARAM_MVOLR, 0x3FFF);
    mmix = sceSdGetParam(CORE | SD_PARAM_MMIX);
    printf("spusynth: core %d MMIX was 0x%03x -> 0x%03x\n",
           CORE, mmix, mmix | MMIX_SND_DRY);
    sceSdSetParam(CORE | SD_PARAM_MMIX, mmix | MMIX_SND_DRY);

    // Upload the sample. IO (PIO) mode -- the IOP->SPU2 DMA path times out here.
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_IO, adpcm, &spu, SMPBYTES);
    sceSdVoiceTransStatus(0, 1);

    // Tone voice (1): full volume, ~1.5 kHz, fast attack, full sustain.
    sceSdSetParam(SD_VOICE(CORE, 1) | SD_VPARAM_VOLL,  0x3FFF);
    sceSdSetParam(SD_VOICE(CORE, 1) | SD_VPARAM_VOLR,  0x3FFF);
    sceSdSetParam(SD_VOICE(CORE, 1) | SD_VPARAM_PITCH, 0x1000);
    sceSdSetParam(SD_VOICE(CORE, 1) | SD_VPARAM_ADSR1, SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0, 0xF, 0xF));
    sceSdSetParam(SD_VOICE(CORE, 1) | SD_VPARAM_ADSR2, SD_SET_ADSR2(SD_ADSR_SR_EXPd, 0x7F, SD_ADSR_RR_EXPd, 0));
    sceSdSetAddr (SD_VOICE(CORE, 1) | SD_VADDR_SSA,    SPU_ADDR);

    // Noise voice (2): diagnostic -- needs no sample. NON routes the noise
    // source into this voice instead of ADPCM playback.
    sceSdSetParam(SD_VOICE(CORE, 2) | SD_VPARAM_VOLL,  0x3FFF);
    sceSdSetParam(SD_VOICE(CORE, 2) | SD_VPARAM_VOLR,  0x3FFF);
    sceSdSetParam(SD_VOICE(CORE, 2) | SD_VPARAM_ADSR1, SD_SET_ADSR1(SD_ADSR_AR_EXPi, 0, 0xF, 0xF));
    sceSdSetParam(SD_VOICE(CORE, 2) | SD_VPARAM_ADSR2, SD_SET_ADSR2(SD_ADSR_SR_EXPd, 0x7F, SD_ADSR_RR_EXPd, 0));
    sceSdSetSwitch(SD_SWITCH_NON | CORE, m2);

    // Route both voices into the L/R mix, then key both on.
    sceSdSetSwitch(SD_SWITCH_VMIXL | CORE, m1 | m2);
    sceSdSetSwitch(SD_SWITCH_VMIXR | CORE, m1 | m2);
    sceSdSetSwitch(SD_SWITCH_KON   | CORE, m1 | m2);

    printf("spusynth: voices keyed on core %d (tone v1 @ spu 0x%x + noise v2)\n",
           CORE, SPU_ADDR);
    return MODULE_RESIDENT_END;
}
