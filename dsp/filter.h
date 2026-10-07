/* DRIFTER - VCF: nonlinear 4-pole (24 dB/oct) ladder low pass. MIT.
 *
 * Zero-delay-feedback (TPT) ladder after Zavalishin, "The Art of VA Filter
 * Design": the global feedback is solved for the current sample, so the
 * cutoff can be modulated at audio rate without the unit-delay ladder's
 * detuning and instability. The nonlinearity is a tanh on the solved input
 * stage (the ladder's differential pair), which bounds self-oscillation and
 * adds drive -- a one-step approximation of the fully implicit solution.
 *
 * Why a ladder: REON documents the driftbox S rev.2 filter as a "24dB/oct
 * ladder type" (and elsewhere names the SSM2044). No primary source was found
 * for the SIII's circuit, so this is an ASSUMPTION (README). The whole filter
 * is this struct and one function; a different topology replaces this file.
 */
#ifndef DB_FILTER_H
#define DB_FILTER_H

#include "db_math.h"

/* Ceiling of the input stage's tanh. It sets the self-oscillation level
 * (the loop settles where the saturated gain falls to 1) as well as how hard
 * a loud mix is driven. */
#define DB_LADDER_CEIL 2.0f

typedef struct {
    float s[4];      /* TPT integrator states */
} db_ladder;

static inline void ladder_reset(db_ladder *f) {
    f->s[0] = f->s[1] = f->s[2] = f->s[3] = 0.0f;
}

/* g = tan(pi * fc / fs) (prewarped), k = 0..~4.4 (self-oscillates above 4).
 * Returns the 4th pole output. */
static inline float ladder_process(db_ladder *f, float x, float g, float k) {
    const float G = g / (1.0f + g);
    const float ig = 1.0f / (1.0f + g);
    const float b0 = f->s[0] * ig, b1 = f->s[1] * ig, b2 = f->s[2] * ig, b3 = f->s[3] * ig;
    const float S = G * (G * (G * b0 + b1) + b2) + b3;   /* y4 = G^4 u + S */
    const float G4 = G * G * G * G;
    float u = (x - k * S) / (1.0f + k * G4);
    u = DB_LADDER_CEIL * db_tanh(u * (1.0f / DB_LADDER_CEIL));
    float v, y = u;
    for (int i = 0; i < 4; ++i) {
        v = (y - f->s[i]) * G;
        y = v + f->s[i];
        f->s[i] = y + v;
    }
    return y;
}

#endif
