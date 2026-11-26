/* Drifter - chaotic drone engine. MIT.
 *
 * Inspired by the REON driftbox SIII. Not affiliated with or endorsed by
 * REON. A clean-room implementation from the published feature list and
 * general DSP technique; no firmware or circuit is reproduced.
 *
 * Not a keyboard synth: it drones while Move's transport runs and is silent
 * while it is stopped. MIDI notes, velocity and pitch bend are ignored.
 *
 *   MOTION (slow sines + smooth random) --> FM / SYNC / VCF mod / cutoff / ratio
 *   VCO1 (square, sync master) <-> VCO2 (saw, sync slave)
 *        cross FM both ways from each VCO's triangle, hard sync 1 -> 2
 *   MIX --> 24 dB ladder VCF (cutoff modulated at audio rate: DRIFT Y)
 *   + CLEAN SUB (sine, VCO1's pitch folded into 40..80 Hz, no FM)
 *   + DIRTY SUB (same phase, phase modulated by VCO1, saturated, low passed)
 *   --> DELAY (one macro) --> transport fade --> safety --> VOLUME
 *
 * Oscillators, mixer, VCF and both subs run at DB_OS x the host rate and are
 * decimated once; delay and output run at the host rate.
 * No allocation, no I/O: db_engine_render is called from the audio callback.
 */
#ifndef DB_ENGINE_H
#define DB_ENGINE_H

#include <stdint.h>
#include "oscillator.h"
#include "filter.h"
#include "delay.h"

#define DB_OS 2
#define DB_MAX_FRAMES 256
#define DB_HB_TAPS 31              /* halfband decimator length (odd) */

/* Parameter indices. ORDER IS the chain_params order in module.json
 * (scripts/metadata.py fails the build on drift) and the state blob's. */
enum {
    /* MAIN */
    DP_VOLUME, DP_DRIFT_X, DP_DRIFT_Y, DP_FM_DRIVE,
    DP_SYNC_SWEEP, DP_VCF_CUT, DP_VCF_RESO, DP_MOTION,
    /* SETUP */
    DP_VCO1_FREQ, DP_VCO2_FREQ, DP_VCO_MIX, DP_BASS,
    DP_DIRTY_SUB, DP_DELAY, DP_CHARACTER, DP_RANDOM_STYLE,
    DP_COUNT
};

enum { DB_CHAR_FM, DB_CHAR_SYNC, DB_CHAR_FMSYNC };
enum { DB_STYLE_CHAOTIC, DB_STYLE_DARK, DB_STYLE_INDUSTRIAL };

typedef struct {
    const char *key;
    float min, max, def;
    int is_int;
} db_param_info;

extern const db_param_info db_params[DP_COUNT];

/* ---- ranges (README "Parameters"); exported so tests measure the same maps ---- */
#define DB_VCO_LO_HZ 27.5f         /* VCO FREQ: A0 .. A4, 4 octaves */
#define DB_VCO_OCT 4.0f
#define DB_SUB_LO_HZ 40.0f         /* the subs fold VCO1's pitch into 40..80 Hz */
#define DB_FM_OCT 6.0f             /* FM DRIVE = 1: VCO2 -> VCO1 +-6 octaves */
#define DB_FM_BACK 0.8f            /* VCO1 -> VCO2 is this much of it */
#define DB_FM_DOWN_OCT 1.5f        /* FM never takes a VCO further below its pitch */
#define DB_FM_UP_OCT 5.0f          /* ...or further above it */
#define DB_SYNC_OCT 5.0f           /* SYNC SWEEP = 1: slave 5 octaves up */
#define DB_SWEEP_FM_OCT 3.0f       /* ...or, in FM mode, VCO2 3 octaves up */
#define DB_DRIFT_X_DN_OCT 1.0f     /* DRIFT X: VCO2 against VCO1, -1 .. */
#define DB_DRIFT_X_UP_OCT 3.0f     /* .. +3 octaves (the subs keep the bass) */
#define DB_DRIFT_Y_OCT 5.0f        /* DRIFT Y: audio-rate VCF mod, +-5 octaves */
#define DB_DRIFT_Y_OPEN_OCT 4.0f   /* +Y also opens the cutoff up to 4 octaves */
#define DB_FM_OPEN_OCT 1.0f        /* FM DRIVE and SYNC SWEEP each open the */
#define DB_SWEEP_OPEN_OCT 1.0f     /* cutoff by up to 1 octave, so what they add is heard */
#define DB_VCF_LO_HZ 40.0f         /* CUTOFF: 40 Hz .. 16 kHz */
#define DB_VCF_OCT 8.6439f
#define DB_RESO_K 4.2f             /* self-oscillates above 4 (RESO ~0.95) */
#define DB_FADE_IN_S 0.020f        /* transport start / stop fades */
#define DB_FADE_OUT_S 0.030f

float db_vco_hz(float v);          /* FREQ knob -> Hz */
float db_sub_hz(float vco1);       /* VCO1 FREQ knob -> both subs' Hz */
float db_cut_hz(float v);
float db_fm_oct(float v);          /* FM DRIVE knob -> octaves */
float db_delay_s(float v);         /* DELAY macro -> time, feedback, wet */
float db_delay_fb(float v);
float db_delay_wet(float v);

/* MOTION: five slow bipolar signals, each a sine plus a smooth random walk. */
#define DB_MOTION_N 5
enum { DB_MO_FM, DB_MO_SYNC, DB_MO_VCF, DB_MO_CUT, DB_MO_RATIO };
typedef struct {
    float sin_ph[DB_MOTION_N];
    float rw_from[DB_MOTION_N], rw_to[DB_MOTION_N], rw_t[DB_MOTION_N], rw_len[DB_MOTION_N];
    float jit_from[DB_MOTION_N], jit_to[DB_MOTION_N], jit_t;   /* faster term above 0.6 */
    uint32_t rng;
    float out[DB_MOTION_N];         /* -1..1 times the MOTION depth */
} db_motion;

typedef struct {
    float sr, osr;
    float param[DP_COUNT];

    db_osc osc[2];
    float tri[2];           /* each VCO's triangle (its FM signal), last sample */
    float tri2_dc, dc_coef; /* VCO2's triangle has a DC bias once synced: removed */
    db_ladder vcf;
    uint32_t rng;           /* VCF seed noise */
    db_motion mo;
    float mo_prev[DB_MOTION_N];  /* MOTION at the previous block (ramped across) */

    /* smoothed controls (per oversampled sample) */
    float p_oct[2];         /* log2 Hz */
    float sub_oct;
    float cut_oct;
    float fm_s[2];          /* FM depth in octaves into VCO1 / into VCO2 */
    float vcfm_s;           /* signed DRIFT Y depth, octaves */
    float mix_s;
    float clean_s, dirty_s, dirty_amt_s;
    float sm_coef, slow_coef;

    /* subs */
    float sub_phase;
    float dsub_lp;          /* dirty sub's own low pass */
    float wander_ph;        /* slow index wander of the dirty sub */

    /* CHARACTER: switched under a short duck so the swap is never a click */
    int character;          /* the mode in use */
    float duck, duck_coef;

    /* transport */
    int transport;          /* Move's transport is running */
    int idle;               /* stopped and fully faded: nothing is rendered */
    float fade, fade_in, fade_out;

    /* delay (host rate) */
    db_delay dly;
    float fb_s, wet_s, fb_coef, t_coef;

    /* output */
    float hp_b0, hp_b1, hp_b2, hp_a1, hp_a2, hp_z1, hp_z2;  /* 25 Hz 2-pole HP */
    float lim_gain, lim_rel;
    float vol_s, vol_coef;
    unsigned resets;        /* NaN/Inf recoveries (diagnostic) */
    unsigned lim_hits;      /* samples the emergency limiter reduced */

    float hb[DB_HB_TAPS];
    float hist[2 * DB_HB_TAPS];
    int hpos;
} db_engine;

void db_engine_init(db_engine *e, float sample_rate);
/* Clears all audio state (phases, filter, delay line); parameters and the
 * MOTION walk are kept. */
void db_engine_restart(db_engine *e);
void db_engine_set_param(db_engine *e, int idx, float v);
/* Move's transport, once per block: running = drone, stopped = fade out. */
void db_engine_set_transport(db_engine *e, int running);
/* Advance MOTION by `seconds` (called by render once per block; exported so
 * the tests can watch it alone). */
void db_motion_step(db_motion *m, float motion, float seconds);
/* frames <= DB_MAX_FRAMES; writes -1..1 mono floats. */
void db_engine_render(db_engine *e, float *out, int frames);

#endif
