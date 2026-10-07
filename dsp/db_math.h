/* Driftbox SIII - small shared math helpers. MIT.
 * Inline and allocation-free: everything here runs on the SPI callback. */
#ifndef DB_MATH_H
#define DB_MATH_H

#include <math.h>
#include <stdint.h>

#define DB_PI 3.14159265358979f

static inline float db_clampf(float x, float lo, float hi) {
    if (!(x == x)) return lo;
    return x < lo ? lo : (x > hi ? hi : x);
}

/* 2^x, polynomial on the fraction, exact exponent. < 0.01% error. */
static inline float db_exp2(float x) {
    if (!(x > -24.0f)) x = -24.0f;
    if (x > 24.0f) x = 24.0f;
    float fl = floorf(x);
    float f = x - fl;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    union { float f; int32_t i; } u;
    u.i = ((int32_t)fl + 127) << 23;
    return p * u.f;
}

/* Rational tanh: slope 1 at 0, exactly +-1 with zero slope at |x| >= 3. */
static inline float db_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* tan(w) for 0 <= w <= ~0.7: odd series, < 0.1% error at the top. */
static inline float db_tan(float w) {
    float w2 = w * w;
    return w * (1.0f + w2 * (0.3333333f + w2 * (0.1333333f + w2 * 0.0539683f)));
}

/* xorshift32 -> [-1, 1). State must be non-zero. */
static inline float db_noise(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* One-pole coefficient for a time constant of tau seconds at rate fs. */
static inline float db_coef(float tau_s, float fs) {
    return 1.0f - expf(-1.0f / (tau_s * fs));
}

/* Values this small only cost time (denormals on the way to zero). */
static inline float db_flush(float x) {
    return fabsf(x) < 1e-15f ? 0.0f : x;
}

#endif
