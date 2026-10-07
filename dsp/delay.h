/* Driftbox SIII - delay. MIT.
 *
 * TIME and FEEDBACK only. What makes it less than a clean digital echo, and
 * nothing more:
 *   - the delay time GLIDES (one-pole, ~80 ms), so turning TIME -- or DRIFT X
 *     on D.TIME -- bends the repeats' pitch instead of clicking;
 *   - the repeats are darker the longer the delay (a bucket-brigade trait);
 *   - the feedback path saturates softly, so FEEDBACK at the top builds into a
 *     bounded, crumbling wash instead of running away.
 * A high pass in the loop keeps DC (a pulse wave through a low pass) from
 * piling up at high feedback.
 *
 * The SIII's delay type and time range are not published (README); 10 ms to
 * 1 s is an assumption.
 */
#ifndef DB_DELAY_H
#define DB_DELAY_H

#include "db_math.h"

#define DB_DELAY_LEN 131072          /* power of two; 1.36 s at 96 kHz */
#define DB_DELAY_MASK (DB_DELAY_LEN - 1)

typedef struct {
    float buf[DB_DELAY_LEN];
    int w;
    float t_s;       /* smoothed delay, samples */
    float lp;        /* darkening low pass state */
    float hp_x, hp_y;
} db_delay;

static inline void delay_reset(db_delay *d, float t_samples) {
    for (int i = 0; i < DB_DELAY_LEN; ++i) d->buf[i] = 0.0f;
    d->w = 0;
    d->t_s = t_samples;
    d->lp = d->hp_x = d->hp_y = 0.0f;
}

/* One sample. t: target delay in samples (clamped here), fb: loop gain
 * (0..~1.1), t_coef: glide coefficient, lp_coef: darkening, hp_r: DC pole.
 * Returns the delayed (wet) signal. */
static inline float delay_process(db_delay *d, float in, float t, float fb,
                                  float t_coef, float lp_coef, float hp_r) {
    t = db_clampf(t, 2.0f, (float)(DB_DELAY_LEN - 4));
    d->t_s += (t - d->t_s) * t_coef;
    float rp = (float)d->w - d->t_s;
    if (rp < 0.0f) rp += (float)DB_DELAY_LEN;
    int i0 = (int)rp;
    float fr = rp - (float)i0;
    float a = d->buf[i0 & DB_DELAY_MASK], b = d->buf[(i0 + 1) & DB_DELAY_MASK];
    float y = a + (b - a) * fr;

    d->lp = db_flush(d->lp + (y - d->lp) * lp_coef);
    float h = d->lp - d->hp_x + hp_r * d->hp_y;
    d->hp_x = d->lp;
    d->hp_y = db_flush(h);

    /* soft ceiling at 1.2: slope 1 in normal use, bounded at any gain */
    float wv = 1.2f * db_tanh((in + fb * d->hp_y) * (1.0f / 1.2f));
    d->buf[d->w] = db_flush(wv);
    d->w = (d->w + 1) & DB_DELAY_MASK;
    return d->lp;
}

#endif
