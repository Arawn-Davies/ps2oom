// Shared EE<->IOP protocol for the SPU2 hardware synth (spusynth.irx).
//
// The EE music module parses a MIDI song, flattens every track (applying
// tempo) into a timed event stream, ships it to the IOP over SIF RPC, then
// tells it to PLAY. The IOP runs the sequencer on its own thread (DelayThread
// gives precise timing) and drives the SPU2 voice pool. Included by both the
// IOP IRX and the EE module so the wire format can't drift.

#ifndef SPUSYNTH_RPC_H
#define SPUSYNTH_RPC_H

// SIF RPC server id (arbitrary, must match on both sides). "SPU2".
#define SPUSYNTH_RPC_ID   0x53505532

// RPC function ids.
#define SPUSYNTH_RESET    1   // clear the loaded song (no payload)
#define SPUSYNTH_LOAD     2   // append events: u32 count, then count spusynth_ev_t
#define SPUSYNTH_PLAY     3   // start from event 0: u32 loop (1 = loop forever)
#define SPUSYNTH_STOP     4   // stop + all notes off (no payload)
#define SPUSYNTH_VOLUME   5   // set master volume: u32 vol (0..0x3FFF)

// Max events the EE may send per LOAD call (keeps a call within ~2 KB, well
// inside the RPC buffer). The EE chunks longer songs across several LOADs.
#define SPUSYNTH_LOAD_MAX 200

// One flattened sequencer event (8 bytes). delay_us is the wait *before* this
// event fires, relative to the previous event (already tempo-resolved on EE).
typedef struct
{
    unsigned int   delay_us;   // microseconds to wait before applying this event
    unsigned char  cmd;        // SPUSYNTH_EV_*
    unsigned char  chan;       // MIDI channel 0..15
    unsigned char  note;       // MIDI note 0..127
    unsigned char  vel;        // velocity 1..127 (0 on note-off)
} spusynth_ev_t;

// Event commands.
#define SPUSYNTH_EV_NOTE_OFF 0
#define SPUSYNTH_EV_NOTE_ON  1
#define SPUSYNTH_EV_END      2   // end of song marker (loop or stop here)
#define SPUSYNTH_EV_PROGRAM  3   // channel instrument select (note field = GM program)

#endif // SPUSYNTH_RPC_H
