/* Drifter - the two VCOs. MIT.
 *
 * VCO1 is a square and the sync MASTER; VCO2 is a saw and the sync SLAVE.
 * Sync only ever runs 1 -> 2: two VCOs resetting each other fall into a
 * period-chasing buzz that no knob can steer, so FM is the only way back.
 *
 * Each VCO also offers the TRIANGLE of its own phase as its FM signal: a
 * square as a modulator only flips the other VCO between two pitches, while
 * a triangle sweeps it, which keeps the low end of FM DRIVE gentle.
 *
 * Band limiting, at the engine's 2x rate:
 *   VCO1  PolyBLEP on both square edges.
 *   VCO2  PolyBLEP on its natural wrap AND on the sync reset. The reset's
 *         step size is only known once it has happened, so VCO2 is output ONE
 *         SAMPLE LATE and the half of the correction that belongs before the
 *         step is added to the sample still held back (11 us at 88.2 kHz).
 */
#ifndef DB_OSCILLATOR_H
#define DB_OSCILLATOR_H

#include "db_math.h"

typedef struct {
    float phase;     /* 0..1 */
    float age;       /* VCO1: samples since its last wrap, at the end of the
                        sample in which it happened (0..1) */
    int wrapped;     /* VCO1: a wrap happened in the last rendered sample */
    float pending;   /* VCO2: the held-back sample, corrections included */
} db_osc;

/* Two-sample polynomial band-limited step. t, dt in cycles. */
static inline float db_blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline void osc_reset(db_osc *o, float phase) {
    o->phase = phase;
    o->age = 0.0f;
    o->wrapped = 0;
    o->pending = 0.0f;
}

static inline float osc_tri(float ph) { return 1.0f - 4.0f * fabsf(ph - 0.5f); }

/* VCO1: one sample of the band-limited square. inc: cycles per sample,
 * 0 < inc <= 0.45. */
static inline float vco1_render(db_osc *o, float inc) {
    float ph = o->phase + inc;
    o->wrapped = 0;
    if (ph >= 1.0f) {
        ph -= 1.0f;
        if (ph >= 1.0f) ph = 0.0f;
        o->wrapped = 1;
        o->age = ph / inc;
    }
    o->phase = ph;
    float y = ph < 0.5f ? 1.0f : -1.0f;
    float t2 = ph + 0.5f;
    if (t2 >= 1.0f) t2 -= 1.0f;
    return y + db_blep(ph, inc) - db_blep(t2, inc);
}

/* A step of height h, d samples (0..1) before the current sample: the two
 * halves of the PolyBLEP residual, for the held-back and the current one. */
static inline void step_residual(float *before, float *now, float h, float d) {
    d = db_clampf(d, 0.0f, 1.0f);
    *before += h * 0.5f * d * d;
    *now -= h * 0.5f * (1.0f - d) * (1.0f - d);
}

/* VCO2: advance one sample and return the PREVIOUS one (see above).
 * sync: the master wrapped in this same sample, master_age samples before its
 * end -- the slave restarts from that instant. */
static inline float vco2_render(db_osc *o, float inc, int sync, float master_age) {
    float before = o->pending, now = 0.0f;
    float ph;
    if (sync) {
        float a = db_clampf(master_age, 0.0f, 1.0f);
        float ev = o->phase + inc * (1.0f - a);       /* the slave at the reset */
        if (ev >= 1.0f) {                             /* it wrapped first */
            ev -= 1.0f;
            if (ev >= 1.0f) ev = 0.0f;
            step_residual(&before, &now, -2.0f, a + ev / inc);
        }
        step_residual(&before, &now, -2.0f * ev, a);  /* 2ev-1 -> -1 */
        ph = inc * a;
    } else {
        ph = o->phase + inc;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            if (ph >= 1.0f) ph = 0.0f;
            step_residual(&before, &now, -2.0f, ph / inc);
        }
    }
    o->phase = ph;
    o->pending = 2.0f * ph - 1.0f + now;
    return before;
}

#endif
