/* DRIFTER - engine. MIT.
 *
 * Inspired by the REON driftbox SIII. Not affiliated with or endorsed by
 * REON. A clean-room implementation from the published feature list and
 * general DSP technique; no firmware or circuit is reproduced.
 *
 *   LFO --> VCO1 / VCO2 / VCF
 *   VCO1 <-> VCO2   audio-rate cross FM (previous sample), optional hard sync
 *   MIX --> 24 dB ladder VCF (cutoff modulated at audio rate by VCO1/VCO2/LFO)
 *   --> VCA (gate, 3 ms smoothing) --> DELAY (time, feedback) --> VOLUME
 *
 * Monophonic. Oscillators, mixer and VCF run at DB_OS x the host rate and are
 * decimated once; VCA, delay and output run at the host rate.
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
 * (scripts/metadata.py fails the build on drift); the KEYS are the stable
 * IDs and never change. */
enum {
    DP_VCO1_FREQ, DP_VCO1_MOD, DP_VCO1_WAVE, DP_VCO1_MODE, DP_VCO1_LFO, DP_VCO1_VOL,
    DP_VCO2_FREQ, DP_VCO2_MOD, DP_VCO2_WAVE, DP_VCO2_MODE, DP_VCO2_LFO, DP_VCO2_VOL,
    DP_VCF_CUT, DP_VCF_RESO, DP_VCF_MOD, DP_VCF_VCO1, DP_VCF_VCO2, DP_VCF_LFO,
    DP_LFO_RATE, DP_LFO_WAVE,
    DP_DELAY_TIME, DP_DELAY_FB,
    DP_DRIFT_X, DP_DRIFT_X_TGT, DP_DRIFT_Y, DP_DRIFT_Y_TGT,
    DP_GATE_HOLD, DP_TRIGGER, DP_VOLUME,
    DP_COUNT
};

enum { DB_MODE_FM, DB_MODE_SYNC };
enum { DB_LFO_TRI, DB_LFO_SQR };
enum { DB_X_VCO, DB_X_DTIME };
enum { DB_Y_VCF, DB_Y_FB };
enum { DB_HOLD_OFF, DB_HOLD_PLAY, DB_HOLD_ALWAYS };

typedef struct {
    const char *key;
    float min, max, def;
    int is_int;
} db_param_info;

extern const db_param_info db_params[DP_COUNT];

/* ---- ranges (README "Controls"); exported so tests measure the same maps ---- */
#define DB_VCO_LO_HZ 0.3f          /* driftbox S spec: 0.3 Hz .. 14 kHz */
#define DB_VCO_HI_HZ 14000.0f
#define DB_VCO_REF_NOTE 60         /* FREQ is the pitch at this MIDI note */
#define DB_FM_OCT 6.0f             /* MOD = 1: +-6 octaves per unit of source */
#define DB_VCF_LO_HZ 16.0f
#define DB_VCF_OCT 10.1f           /* 16 Hz .. ~17.6 kHz */
#define DB_VCF_MOD_OCT 6.0f
#define DB_RESO_K 4.4f             /* self-oscillates above 4 (RESO ~0.91) */
#define DB_LFO_LO_HZ 0.02f         /* 0.02 .. 200 Hz */
#define DB_LFO_DECADES 4.0f
#define DB_DELAY_LO_S 0.010f       /* 10 ms .. 1 s */
#define DB_DELAY_DECADES 2.0f
#define DB_DRIFT_X_OCT 3.0f        /* X on VCO: +-3 octaves, both VCOs */
#define DB_DRIFT_X_DT_OCT 2.0f     /* X on D.TIME: x1/4 .. x4 */
#define DB_DRIFT_Y_OCT 5.0f        /* Y on VCF: +-5 octaves */
#define DB_DRIFT_Y_FB 0.6f         /* Y on F.B: +-0.6 of feedback */
#define DB_TRIG_S 0.2f             /* TRIGGER opens the gate this long */

float db_vco_hz(float v);          /* FREQ knob -> Hz at the reference note */
float db_cut_hz(float v);
float db_lfo_hz(float v);
float db_delay_s(float v);
float db_mod_depth(float v);       /* knob -> 0..1 depth (squared taper) */

typedef struct {
    float sr, osr;
    float param[DP_COUNT];

    db_osc osc[2];
    float last[2];          /* previous-sample VCO outputs (cross FM) */
    int mod_source[2];      /* which VCO modulates / syncs each VCO */
    float lfo_phase, lfo_sq;
    db_ladder vcf;
    uint32_t rng;           /* VCF seed noise */

    /* smoothed controls (per oversampled sample) */
    float p_oct[2];         /* log2 Hz */
    float cut_oct;
    float vol_mix[2];
    float sm_coef, sq_coef;

    /* gate */
    int gate;               /* a MIDI note is held */
    int note;               /* last note (kept after release) */
    int transport;          /* Move's transport is running (GATE HOLD Play) */
    int32_t trig_left;      /* TRIGGER samples remaining */
    float vca, vca_coef;

    /* delay (host rate) */
    db_delay dly;
    float fb_s, wet_s, fb_coef, t_coef;

    /* output */
    float dc_x, dc_y, dc_r;
    float lim_gain, lim_rel;
    float vol_s, vol_coef;
    unsigned resets;        /* NaN/Inf recoveries (diagnostic) */
    unsigned lim_hits;      /* samples the emergency limiter reduced */

    float hb[DB_HB_TAPS];
    float hist[2 * DB_HB_TAPS];
    int hpos;
} db_engine;

void db_engine_init(db_engine *e, float sample_rate);
/* Clears all audio state (phases, filter, delay line); parameters kept. */
void db_engine_restart(db_engine *e);
void db_engine_set_param(db_engine *e, int idx, float v);
/* gate: any note held; note: the note to sound (last-note priority). */
void db_engine_set_gate(db_engine *e, int gate, int note);
/* Move's transport state, once per block (GATE HOLD Play follows it). */
void db_engine_set_transport(db_engine *e, int running);
void db_engine_trigger(db_engine *e);
/* frames <= DB_MAX_FRAMES; writes -1..1 mono floats. */
void db_engine_render(db_engine *e, float *out, int frames);

#endif
