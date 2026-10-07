/* DRIFTER - one VCO. MIT.
 *
 * Five selectable waveforms per VCO. REON confirms "5 waveforms each,
 * including noise" for the SIII, but not which five. The set below is a
 * PROVISIONAL assumption (README, "Unconfirmed hardware details"): the enum is
 * positional (WAVE_1..WAVE_5) and each slot's shape lives in exactly one
 * switch in osc_render(), so a confirmed set replaces one case each without
 * touching the parameter IDs or saved states.
 *
 * Band limiting: PolyBLEP on the saw / square / pulse edges, at the engine's
 * 2x oversampled rate. A hard-sync reset is NOT band-limited (a documented
 * compromise, see README "Audio quality"); the reset is at least placed at the
 * master's fractional wrap time, so the synced pitch is exact.
 */
#ifndef DB_OSCILLATOR_H
#define DB_OSCILLATOR_H

#include "db_math.h"

enum { DB_WAVE_1, DB_WAVE_2, DB_WAVE_3, DB_WAVE_4, DB_WAVE_5, DB_WAVE_COUNT };

/* Provisional identities of the five positions. */
#define DB_WAVE_SAW   DB_WAVE_1
#define DB_WAVE_TRI   DB_WAVE_2
#define DB_WAVE_SQR   DB_WAVE_3
#define DB_WAVE_PULSE DB_WAVE_4
#define DB_WAVE_NOISE DB_WAVE_5

#define DB_PULSE_WIDTH 0.2f

typedef struct {
    float phase;     /* 0..1 */
    float age;       /* samples elapsed since the last natural wrap, at the end
                        of the sample in which it happened (0..1) */
    int wrapped;     /* a natural wrap happened in the last rendered sample */
    uint32_t rng;    /* noise waveform */
} db_osc;

/* Two-sample polynomial band-limited step. t, dt in cycles. */
static inline float db_blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline void osc_reset(db_osc *o, float phase, uint32_t seed) {
    o->phase = phase;
    o->age = 0.0f;
    o->wrapped = 0;
    o->rng = seed ? seed : 0x9E3779B9u;
}

/* Advance one sample and return the waveform, -1..1.
 * inc: cycles per sample, 0 < inc <= 0.45.
 * sync: the master wrapped during the PREVIOUS sample, master_age samples
 * before that sample ended -- the slave restarts from that instant. Only a
 * NATURAL wrap is reported to others: a synced reset is not, or two VCOs
 * syncing each other would reset one another every sample. */
static inline float osc_render(db_osc *o, int wave, float inc, int sync, float master_age) {
    float ph;
    int synced = 0;
    if (sync) {
        ph = (master_age + 1.0f) * inc;
        synced = 1;
    } else {
        ph = o->phase + inc;
    }
    o->wrapped = 0;
    if (ph >= 1.0f) {
        ph -= 1.0f;
        if (ph >= 1.0f) ph = 0.0f;
        if (!synced) {
            o->wrapped = 1;
            o->age = ph / inc;
        }
    }
    o->phase = ph;

    float y;
    switch (wave) {
    case DB_WAVE_SAW:
        y = 2.0f * ph - 1.0f;
        if (!synced) y -= db_blep(ph, inc);
        break;
    case DB_WAVE_TRI:
        y = 1.0f - 4.0f * fabsf(ph - 0.5f);
        break;
    case DB_WAVE_SQR: {
        y = ph < 0.5f ? 1.0f : -1.0f;
        if (!synced) {
            float t2 = ph + 0.5f;
            if (t2 >= 1.0f) t2 -= 1.0f;
            y += db_blep(ph, inc) - db_blep(t2, inc);
        }
        break;
    }
    case DB_WAVE_PULSE: {
        y = ph < DB_PULSE_WIDTH ? 1.0f : -1.0f;
        if (!synced) {
            float t2 = ph + (1.0f - DB_PULSE_WIDTH);
            if (t2 >= 1.0f) t2 -= 1.0f;
            y += db_blep(ph, inc) - db_blep(t2, inc);
        }
        /* centred: as an FM source it must not bend the other VCO's pitch */
        y -= 2.0f * DB_PULSE_WIDTH - 1.0f;
        break;
    }
    case DB_WAVE_NOISE:
    default:
        y = db_noise(&o->rng);
        break;
    }
    return y;
}

#endif
