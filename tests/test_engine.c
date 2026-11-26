/* Drifter engine tests. Built with ASan/UBSan by scripts/test.sh.
 *   test-engine            all tests (includes a 60 s stress render)
 *   test-engine --quick    skips the 60 s stress render
 *   bench --bench          realtime factor
 *   bench --wav out.wav    a short demo
 *
 * The engine is deterministic, so two renders that differ in ONE parameter
 * differ only by what that parameter does: several tests subtract them
 * sample by sample to isolate a single path (the subs, a knob's effect). */
#include "db_engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail = 1; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SR 44100
#define BLOCK 128
#define CAP (SR * 4)

static db_engine E, E2;
static float cap[CAP], cap2[CAP];

typedef struct {
    double rms, peak, dc, diff_rms;
    int nonfinite;
    long n;
} stats;

/* Render `seconds` of e; captures up to CAP samples into dst. */
static stats run_into(db_engine *e, float seconds, float *dst) {
    stats s;
    memset(&s, 0, sizeof(s));
    float buf[BLOCK];
    long total = (long)(seconds * e->sr);
    float last = 0.0f;
    while (s.n < total) {
        db_engine_render(e, buf, BLOCK);
        for (int i = 0; i < BLOCK && s.n < total; ++i, ++s.n) {
            float x = buf[i];
            if (!isfinite(x)) { s.nonfinite++; continue; }
            s.rms += (double)x * x;
            s.dc += x;
            s.diff_rms += (double)(x - last) * (x - last);
            last = x;
            if (fabsf(x) > s.peak) s.peak = fabsf(x);
            if (dst && s.n < CAP) dst[s.n] = x;
        }
    }
    s.rms = sqrt(s.rms / s.n);
    s.diff_rms = sqrt(s.diff_rms / s.n);
    s.dc /= s.n;
    return s;
}
static stats run(db_engine *e, float seconds) { return run_into(e, seconds, cap); }

static void set(db_engine *e, int k, float v) { db_engine_set_param(e, k, v); }
static double db(double x) { return 20.0 * log10(x + 1e-12); }

/* Playing, MOTION off (so a measurement is stationary), delay off. */
static void init_still(db_engine *e, float sr) {
    db_engine_init(e, sr);
    set(e, DP_MOTION, 0.0f);
    set(e, DP_DELAY, 0.0f);
    db_engine_set_transport(e, 1);
}

/* A measurable VCO: no FM, no sync, no subs, VCF open. */
static void init_plain(db_engine *e) {
    init_still(e, SR);
    set(e, DP_FM_DRIVE, 0.0f);
    set(e, DP_SYNC_SWEEP, 0.0f);
    set(e, DP_CHARACTER, DB_CHAR_FM);
    set(e, DP_BASS, 0.0f);
    set(e, DP_VCF_CUT, 1.0f);
    set(e, DP_VCF_RESO, 0.0f);
    set(e, DP_VOLUME, 1.0f);
}

/* Fundamental from the autocorrelation peak of x[from .. from+len). */
static float pitch_hz(const float *x, int from, int len, float sr, float lo_hz, float hi_hz) {
    static double r[8192];
    int lag_lo = (int)(sr / hi_hz), lag_hi = (int)(sr / lo_hz) + 1;
    if (lag_hi > 8190) lag_hi = 8190;
    double best = -1e30;
    for (int lag = lag_lo - 1; lag <= lag_hi + 1; ++lag) {
        double acc = 0.0;
        for (int i = 0; i < len; ++i) acc += (double)x[from + i] * x[from + i + lag];
        r[lag] = acc;
        if (lag >= lag_lo && lag <= lag_hi && acc > best) best = acc;
    }
    int best_lag = lag_lo;
    for (int lag = lag_lo; lag <= lag_hi; ++lag)
        if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { best_lag = lag; break; }
    double a = r[best_lag - 1], b = r[best_lag], c = r[best_lag + 1];
    double den = a - 2 * b + c;
    double off = fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
    return (float)(sr / (best_lag + off));
}

/* Complex Goertzel: amplitude of `hz` in x[from .. from+len). */
static double tone(const float *x, int from, int len, float sr, float hz) {
    double re = 0.0, im = 0.0, w = 2.0 * M_PI * hz / sr;
    for (int i = 0; i < len; ++i) { re += x[from + i] * cos(w * i); im -= x[from + i] * sin(w * i); }
    return 2.0 * sqrt(re * re + im * im) / len;
}

/* Energy below / above a one-pole split, as a fraction of the whole. */
static void bands(const float *x, int from, int len, float sr, float lo_hz, float hi_hz,
                  double *lo, double *hi) {
    double tot = 0, l = 0, h = 0;
    float a = 1.0f - expf(-2.0f * (float)M_PI * lo_hz / sr), sl = 0;
    float b = 1.0f - expf(-2.0f * (float)M_PI * hi_hz / sr), sh = 0;
    for (int i = 0; i < len; ++i) {
        float v = x[from + i];
        sl += (v - sl) * a; l += (double)sl * sl;
        sh += (v - sh) * b; h += (double)(v - sh) * (v - sh);
        tot += (double)v * v;
    }
    *lo = l / (tot + 1e-20);
    *hi = h / (tot + 1e-20);
}

static double window_rms(const float *x, int from, int len) {
    double r = 0.0;
    for (int i = 0; i < len; ++i) r += (double)x[from + i] * x[from + i];
    return sqrt(r / len);
}

/* Magnitude spectrum (Hann, 10 Hz bins, 20 Hz .. 4 kHz) of x[from .. from+8192). */
#define NBIN 399
static void spectrum(const float *x, int from, double *mag) {
    const int n = 8192;
    for (int k = 0; k < NBIN; ++k) {
        double w = 2.0 * M_PI * (20.0 + 10.0 * k) / SR, re = 0, im = 0;
        for (int i = 0; i < n; ++i) {
            double h = 0.5 - 0.5 * cos(2.0 * M_PI * i / n);
            re += h * x[from + i] * cos(w * i);
            im += h * x[from + i] * sin(w * i);
        }
        mag[k] = sqrt(re * re + im * im);
    }
}
/* How different two spectra are, 0 (same) .. 2 (disjoint), level-normalised. */
static double spec_dist(const double *a, const double *b) {
    double sa = 0, sb = 0, d = 0;
    for (int k = 0; k < NBIN; ++k) { sa += a[k]; sb += b[k]; }
    for (int k = 0; k < NBIN; ++k) d += fabs(a[k] / sa - b[k] / sb);
    return d;
}

/* ---- parameters and maps --------------------------------------------------- */
static void test_params(void) {
    db_engine_init(&E, SR);
    for (int i = 0; i < DP_COUNT; ++i)
        CHECK(E.param[i] == db_params[i].def, "default %s", db_params[i].key);
    CHECK(E.param[DP_CHARACTER] == DB_CHAR_FMSYNC, "FM+SYNC is the default CHARACTER");
    CHECK(E.param[DP_RANDOM_STYLE] == DB_STYLE_CHAOTIC, "CHAOTIC is the default RANDOM STYLE");
    set(&E, DP_VCF_CUT, 5.0f);
    CHECK(E.param[DP_VCF_CUT] == 1.0f, "clamped high");
    set(&E, DP_DRIFT_X, -7.0f);
    CHECK(E.param[DP_DRIFT_X] == -1.0f, "DRIFT X clamped to -1 (bipolar)");
    set(&E, DP_DRIFT_Y, 0.5f);
    CHECK(E.param[DP_DRIFT_Y] == 0.5f, "DRIFT Y keeps a positive value");
    set(&E, DP_CHARACTER, 0.6f);
    CHECK(E.param[DP_CHARACTER] == 1.0f, "enum rounds");
    set(&E, DP_VCF_CUT, NAN);
    CHECK(E.param[DP_VCF_CUT] == 1.0f, "NaN ignored");
    set(&E, -1, 0.0f); set(&E, DP_COUNT, 0.0f);   /* out of range: ignored, no crash */

    CHECK(fabsf(db_vco_hz(0.0f) - 27.5f) < 0.01f && fabsf(db_vco_hz(1.0f) - 440.0f) < 0.1f,
          "VCO FREQ A0 .. A4: %f %f", db_vco_hz(0.0f), db_vco_hz(1.0f));
    CHECK(fabsf(db_vco_hz(0.25f) - 55.0f) < 0.05f, "default VCO1 = A1 55 Hz: %f", db_vco_hz(0.25f));
    int sub_ok = 1;
    for (float v = 0.0f; v <= 1.0f; v += 0.001f) {
        float h = db_sub_hz(v), r = h / db_vco_hz(v);
        float o = log2f(r);
        if (!(h >= 40.0f && h < 80.0f) || fabsf(o - roundf(o)) > 1e-3f) sub_ok = 0;
    }
    CHECK(sub_ok, "the subs sit in 40..80 Hz, a whole number of octaves from VCO1");
    CHECK(fabsf(db_cut_hz(0.0f) - 40.0f) < 0.1f && fabsf(db_cut_hz(1.0f) / 16000.0f - 1.0f) < 0.01f,
          "CUTOFF 40 Hz .. 16 kHz");
    CHECK(db_fm_oct(0.0f) == 0.0f && fabsf(db_fm_oct(1.0f) - DB_FM_OCT) < 1e-5f && db_fm_oct(0.3f) > 0.5f,
          "FM DRIVE: 0 none, 0.3 already > half an octave (%f)", db_fm_oct(0.3f));
    CHECK(db_delay_wet(0.0f) == 0.0f && db_delay_fb(1.0f) <= 0.75f + 1e-6f && db_delay_s(1.0f) <= 0.91f
          && db_delay_s(0.0f) >= 0.05f, "DELAY: 0 = dry, feedback never past 0.75");
}

/* ---- transport: drone while playing, fade, idle ---------------------------------- */
static void test_transport(void) {
    db_engine_init(&E, SR);
    stats s = run(&E, 0.5f);
    CHECK(s.peak == 0.0 && E.idle, "stopped: exact silence, nothing rendered");
    db_engine_set_transport(&E, 1);
    s = run(&E, 1.5f);
    CHECK(s.rms > 0.05 && s.nonfinite == 0, "playing: drone (rms %g)", s.rms);
    CHECK(fabsf(cap[0]) < 0.01f && fabsf(cap[1]) < 0.01f, "Play fades in, no click (%g)", cap[1]);
    CHECK(window_rms(cap, 0, 200) < 0.3 * window_rms(cap, SR, 4410), "fade-in rises");
    CHECK(fabs(s.dc) < 0.005, "no DC (%g)", s.dc);
    db_engine_set_transport(&E, 0);
    s = run(&E, 0.06f);
    int n = (int)s.n;
    CHECK(fabsf(cap[n - 1]) == 0.0f && fabsf(cap[n - 300]) == 0.0f, "Stop: silent within ~40 ms (delay too)");
    double jump = 0.0;
    for (int i = 1; i < 200; ++i) jump = fmax(jump, fabsf(cap[i] - cap[i - 1]));
    CHECK(jump < 0.2, "Stop fades, no click (%g)", jump);
    s = run(&E, 0.3f);
    CHECK(s.peak == 0.0 && E.idle, "stays silent and idle");
    /* the next Play starts clean: the delay line was emptied */
    set(&E, DP_DELAY, 1.0f);
    db_engine_set_transport(&E, 1);
    run(&E, 0.2f);
    float sum = 0.0f;
    for (int i = 0; i < DB_DELAY_LEN; ++i) sum += fabsf(E.dly.buf[i]);
    CHECK(sum > 0.0f, "delay running after Play");
}

/* (MIDI notes never reach the engine -- it has no note input at all; that the
 * plugin ignores them is tested in smoke.py.) */

/* ---- VCOs: pitch, DRIFT X on the ratio --------------------------------------------- */
static void test_vco_drift_x(void) {
    /* VCO1 alone (MIX 0) at its FREQ */
    init_plain(&E);
    set(&E, DP_VCO_MIX, 0.0f);
    set(&E, DP_VCO1_FREQ, 0.5f);                 /* 110 Hz */
    db_engine_restart(&E);
    run(&E, 1.0f);
    float hz = pitch_hz(cap, 8000, 8192, SR, 40, 600);
    CHECK(fabsf(hz / 110.0f - 1.0f) < 0.01f, "VCO1 110 Hz: %f", hz);
    /* VCO2 alone, and DRIFT X moves it against VCO1: -1 octave to the left,
     * +3 octaves to the right */
    float want[3] = { 82.5f, 165.0f, 1320.0f }, xs[3] = { -1.0f, 0.0f, 1.0f };
    for (int t = 0; t < 3; ++t) {
        init_plain(&E);
        set(&E, DP_VCO_MIX, 1.0f);
        set(&E, DP_VCO1_FREQ, 0.5f);
        set(&E, DP_VCO2_FREQ, 0.5f + log2f(1.5f) / 4.0f);    /* 165 Hz */
        set(&E, DP_DRIFT_X, xs[t]);
        db_engine_restart(&E);
        run(&E, 1.0f);
        hz = pitch_hz(cap, 8000, 8192, SR, want[t] * 0.7f, want[t] * 1.4f);
        CHECK(fabsf(hz / want[t] - 1.0f) < 0.015f, "DRIFT X %g: VCO2 %f Hz (want %f)", xs[t], hz, want[t]);
    }
    /* small moves near the centre are fine (x(0.3+0.7|x|)) */
    init_plain(&E);
    set(&E, DP_DRIFT_X, 0.1f);
    run(&E, 0.1f);
    float o = E.p_oct[1] - (log2f(db_vco_hz(E.param[DP_VCO2_FREQ])));
    CHECK(fabsf(o - 0.111f) < 0.01f, "DRIFT X 0.1 = %f oct (fine at the centre)", o);
    /* and DRIFT X never touches the subs */
    init_still(&E, SR);
    set(&E, DP_DRIFT_X, 1.0f);
    run(&E, 0.5f);
    CHECK(fabsf(E.sub_oct - log2f(db_sub_hz(E.param[DP_VCO1_FREQ]))) < 1e-3f, "DRIFT X leaves the subs alone");
}

/* ---- FM DRIVE: deeper, more changed, never a level jump ------------------------ */
static void test_fm(void) {
    static double ref[NBIN], mag[NBIN];
    const int modes[2] = { DB_CHAR_FM, DB_CHAR_FMSYNC };
    for (int m = 0; m < 2; ++m) {
        double lv[5], dist[5], spread[5];
        for (int t = 0; t <= 4; ++t) {
            init_still(&E, SR);
            set(&E, DP_BASS, 0.0f);
            set(&E, DP_CHARACTER, (float)modes[m]);
            set(&E, DP_FM_DRIVE, 0.25f * t);
            db_engine_restart(&E);
            run(&E, 0.5f);
            /* VCO1's instantaneous pitch: how far FM throws it (octaves, rms) */
            float buf[1];
            double s1 = 0, s2 = 0, last = E.osc[0].phase;
            const int n = SR;
            for (int i = 0; i < n; ++i) {
                db_engine_render(&E, buf, 1);
                double d = E.osc[0].phase - last;
                if (d < 0) d += 1.0;
                last = E.osc[0].phase;
                double o = log2(d * SR / DB_OS + 1e-3);
                s1 += o; s2 += o * o;
            }
            spread[t] = sqrt(fmax(0.0, s2 / n - (s1 / n) * (s1 / n)));
            stats s = run(&E, 1.0f);
            lv[t] = db(s.rms);
            spectrum(cap, 2000, t ? mag : ref);
            dist[t] = t ? spec_dist(ref, mag) : 0.0;
            CHECK(s.nonfinite == 0 && s.peak <= 1.0, "FM %d/%d finite", m, t);
        }
        int deeper = 1;
        for (int t = 1; t <= 4; ++t) if (!(spread[t] > spread[t - 1])) deeper = 0;
        CHECK(deeper, "mode %d: FM DRIVE throws VCO1's pitch further at every step (%.2f %.2f %.2f %.2f %.2f)",
              modes[m], spread[0], spread[1], spread[2], spread[3], spread[4]);
        CHECK(dist[1] > 0.1 && dist[4] > dist[1], "mode %d: the sound changes from the first quarter on (%.2f .. %.2f)",
              modes[m], dist[1], dist[4]);
        double mn = lv[0], mx = lv[0];
        for (int t = 1; t <= 4; ++t) { mn = fmin(mn, lv[t]); mx = fmax(mx, lv[t]); }
        CHECK(mx - mn < 6.0, "mode %d: FM DRIVE 0..1 level within 6 dB (%.1f .. %.1f dB)", modes[m], mn, mx);
    }
}

/* VCO1's time-averaged pitch (log2 Hz) and its range, with the given FM DRIVE. */
static void vco1_pitch(float drive, int mode, float *mean, float *lo, float *hi) {
    init_still(&E, SR);
    set(&E, DP_CHARACTER, (float)mode);
    set(&E, DP_FM_DRIVE, drive);
    db_engine_restart(&E);
    run(&E, 0.3f);
    float buf[1];
    double acc = 0.0, last = E.osc[0].phase;
    *lo = 1e9f; *hi = -1e9f;
    const long n = SR * 3;
    for (long i = 0; i < n; ++i) {
        db_engine_render(&E, buf, 1);
        double d = E.osc[0].phase - last;
        if (d < 0.0) d += 1.0;
        last = E.osc[0].phase;
        float o = (float)log2(d * SR + 1e-3);
        acc += o;
        if (i % 2) { *lo = fminf(*lo, o); *hi = fmaxf(*hi, o); }
    }
    *mean = (float)(acc / n);
}

static void test_fm_centre(void) {
    const float base = log2f(55.0f);
    for (int m = 0; m < 3; ++m) {
        float mean, lo, hi;
        vco1_pitch(1.0f, m, &mean, &lo, &hi);
        CHECK(mean - base > -0.2f && mean - base < 1.0f,
              "mode %d FM 1: VCO1's pitch centre within an octave above VCO FREQ (%+.2f oct)", m, mean - base);
        if (m != DB_CHAR_SYNC)
            CHECK(lo > base - DB_FM_DOWN_OCT - 0.6f && hi < base + DB_FM_UP_OCT + 0.6f,
                  "mode %d FM 1: VCO1 stays within -%.1f..+%.1f oct (%.2f .. %.2f)", m, DB_FM_DOWN_OCT,
                  DB_FM_UP_OCT, lo - base, hi - base);
    }
}

/* ---- SYNC SWEEP ---------------------------------------------------------------- */
static void test_sync(void) {
    /* SYNC alone on VCO2 (MIX 1): the pitch stays VCO1's while SWEEP
     * changes the overtones */
    double hi_prev = -1.0;
    int brighter = 1;
    for (int t = 0; t <= 3; ++t) {
        init_plain(&E);
        set(&E, DP_CHARACTER, DB_CHAR_SYNC);
        set(&E, DP_VCO_MIX, 1.0f);
        set(&E, DP_VCO1_FREQ, 0.5f);             /* 110 Hz master */
        set(&E, DP_VCO2_FREQ, 0.5f);
        set(&E, DP_SYNC_SWEEP, 0.1f + 0.3f * t);
        db_engine_restart(&E);
        run(&E, 1.0f);
        /* periodic at the MASTER's period: one 110 Hz cycle later it repeats */
        const int lag = (int)lrintf(SR / 110.0f);
        double xy = 0, xx = 0, yy = 0;
        for (int i = 8000; i < 8000 + 8192; ++i) {
            xy += (double)cap[i] * cap[i + lag]; xx += (double)cap[i] * cap[i]; yy += (double)cap[i + lag] * cap[i + lag];
        }
        double rho = xy / sqrt(xx * yy + 1e-20);
        CHECK(rho > 0.8, "SYNC SWEEP %d: repeats at the master's period (r %.2f)", t, rho);
        double lo, hi;
        bands(cap, 8000, 16384, SR, 200, 1500, &lo, &hi);
        if (t && !(hi > hi_prev)) brighter = 0;
        hi_prev = hi;
    }
    CHECK(brighter, "SYNC SWEEP up = more overtones");
    /* FM+SYNC: SWEEP still acts with FM DRIVE fixed, and FM DRIVE with SWEEP fixed */
    float a_[2] = { 0.2f, 0.8f };
    static double sp[2][2][NBIN];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            init_still(&E, SR);
            set(&E, DP_CHARACTER, DB_CHAR_FMSYNC);
            set(&E, DP_FM_DRIVE, a_[i]);
            set(&E, DP_SYNC_SWEEP, a_[j]);
            set(&E, DP_BASS, 0.0f);
            db_engine_restart(&E);
            run(&E, 1.0f);
            spectrum(cap, 8000, sp[i][j]);
        }
    double ds0 = spec_dist(sp[0][0], sp[0][1]), ds1 = spec_dist(sp[1][0], sp[1][1]);
    double df0 = spec_dist(sp[0][0], sp[1][0]), df1 = spec_dist(sp[0][1], sp[1][1]);
    CHECK(ds0 > 0.3 && ds1 > 0.3, "FM+SYNC: SWEEP acts at low and high FM (%.2f %.2f)", ds0, ds1);
    CHECK(df0 > 0.3 && df1 > 0.3, "FM+SYNC: FM DRIVE acts at any SWEEP (%.2f %.2f)", df0, df1);
    /* the sync reset is band limited: SYNC at a high sweep has far less energy
     * near Nyquist than an unlimited reset would (a naive saw reset puts
     * ~-30 dB there); measured as the 18..22 kHz band of a 110 Hz master */
    init_plain(&E);
    set(&E, DP_CHARACTER, DB_CHAR_SYNC);
    set(&E, DP_VCO_MIX, 1.0f);
    set(&E, DP_VCO1_FREQ, 0.5f);
    set(&E, DP_SYNC_SWEEP, 0.7f);
    db_engine_restart(&E);
    stats s = run(&E, 1.0f);
    double lo, top;
    bands(cap, 8000, 16384, SR, 100, 16000, &lo, &top);
    CHECK(top < 0.01, "sync: little energy above 16 kHz (%g)", top);
    CHECK(s.nonfinite == 0, "sync finite");
}

/* ---- CHARACTER switch: no click -------------------------------------------------- */
/* The largest sample-to-sample step a mode makes on its own. */
static double own_step(int mode) {
    init_still(&E2, SR);
    set(&E2, DP_BASS, 0.0f);
    set(&E2, DP_CHARACTER, (float)mode);
    db_engine_restart(&E2);
    run_into(&E2, 1.0f, cap2);
    double j = 0.0;
    for (int i = SR / 2; i < SR; ++i) j = fmax(j, fabsf(cap2[i] - cap2[i - 1]));
    return j;
}

static void test_character_switch(void) {
    int modes[4] = { DB_CHAR_FM, DB_CHAR_SYNC, DB_CHAR_FMSYNC, DB_CHAR_FM };
    double own[3];
    for (int m = 0; m < 3; ++m) own[m] = own_step(m);
    init_still(&E, SR);
    set(&E, DP_BASS, 0.0f);
    run(&E, 1.0f);
    int prev = DB_CHAR_FMSYNC;
    for (int m = 0; m < 4; ++m) {
        set(&E, DP_CHARACTER, (float)modes[m]);
        run(&E, 0.1f);
        double jump = 0.0;
        for (int i = 1; i < 2205; ++i) jump = fmax(jump, fabsf(cap[i] - cap[i - 1]));
        double lim = fmax(own[prev], own[modes[m]]);
        CHECK(jump <= lim * 1.1 + 1e-3, "CHARACTER %d -> %d: no step beyond the modes' own (%g vs %g)",
              prev, modes[m], jump, lim);
        CHECK(E.character == modes[m], "CHARACTER %d in use", modes[m]);
        prev = modes[m];
    }
}

/* ---- DRIFT Y: bipolar audio-rate VCF modulation ----------------------------------- */
static void test_drift_y(void) {
    float ys[3] = { -1.0f, 0.0f, 1.0f };
    double hi[3];
    for (int t = 0; t < 3; ++t) {
        init_still(&E, SR);
        set(&E, DP_BASS, 0.0f);
        set(&E, DP_VCF_CUT, 0.35f);
        set(&E, DP_DRIFT_Y, ys[t]);
        db_engine_restart(&E);
        run_into(&E, 1.0f, t == 0 ? cap : cap2);
        double lo;
        bands(t == 0 ? cap : cap2, 8000, 16384, SR, 200, 1500, &lo, &hi[t]);
    }
    CHECK(hi[0] > 2.0 * hi[1] && hi[2] > 2.0 * hi[1], "DRIFT Y either way tears the filter open (%g %g %g)",
          hi[0], hi[1], hi[2]);
    CHECK(fabs(hi[0] - hi[2]) > 0.05 * fmax(hi[0], hi[2]), "-Y and +Y sound different (%g vs %g)", hi[0], hi[2]);
    /* DRIFT Y is not CUTOFF: the subs (after the VCF) are untouched */
    init_still(&E, SR);
    set(&E, DP_DRIFT_Y, 1.0f);
    run(&E, 0.3f);
    CHECK(fabsf(E.sub_oct - log2f(db_sub_hz(E.param[DP_VCO1_FREQ]))) < 1e-3f, "DRIFT Y leaves the subs alone");
}

/* ---- BASS ENGINE --------------------------------------------------------------- */
/* The sub path alone: render with BASS b and BASS 0 and subtract. Nothing on the
 * main path depends on BASS and the limiter stays out at VOLUME 0.5. */
static void sub_alone(float bass, float dirty, float fm, float motion) {
    for (int r = 0; r < 2; ++r) {
        db_engine *e = r ? &E2 : &E;
        init_still(e, SR);
        set(e, DP_VOLUME, 0.5f);
        set(e, DP_VCF_CUT, 0.0f);             /* main path quiet: no clipping */
        set(e, DP_FM_DRIVE, fm);
        set(e, DP_MOTION, motion);
        set(e, DP_DIRTY_SUB, dirty);
        set(e, DP_BASS, r ? 0.0f : bass);
        db_engine_restart(e);
        run_into(e, 3.0f, r ? cap2 : cap);
    }
    for (int i = 0; i < 3 * SR; ++i) cap[i] -= cap2[i];
    CHECK(E.lim_hits == 0 && E2.lim_hits == 0, "sub isolation: limiter stayed out");
}

static void test_bass(void) {
    const float sub = db_sub_hz(db_params[DP_VCO1_FREQ].def);
    CHECK(fabsf(sub - 55.0f) < 0.1f, "default subs at 55 Hz (%f)", sub);
    /* CLEAN SUB alone (DIRTY 0): a sine at the sub pitch, the same with FM 0
     * and FM 1 and full MOTION -- the main VCOs' chaos never reaches it */
    sub_alone(1.0f, 0.0f, 0.0f, 0.0f);
    double a0 = tone(cap, SR, SR, SR, sub), w0 = window_rms(cap, SR, SR);
    sub_alone(1.0f, 0.0f, 1.0f, 1.0f);
    double a1 = tone(cap, SR, SR, SR, sub), w1 = window_rms(cap, SR, SR);
    CHECK(a0 > 0.05 && fabs(a1 / a0 - 1.0) < 0.05, "CLEAN SUB the same under FM 1 + MOTION 1 (%g vs %g)", a0, a1);
    CHECK(a0 / sqrt(2.0) > 0.97 * w0 && a1 / sqrt(2.0) > 0.97 * w1, "CLEAN SUB is a pure sine (%g/%g)",
          a0 / sqrt(2.0), w0);
    /* DIRTY SUB: harmonics appear, the fundamental stays (and in phase:
     * the two subs add, never cancel) */
    sub_alone(1.0f, 1.0f, 0.3f, 0.0f);
    double d1 = tone(cap, SR, SR, SR, sub), dw = window_rms(cap, SR, SR);
    /* what is not the fundamental, against the clean sub's own (the diff holds
     * both subs, half of it clean) */
    double thd_d = sqrt(fmax(0.0, dw * dw - d1 * d1 / 2.0)) / (d1 / sqrt(2.0));
    double thd_c = sqrt(fmax(0.0, w0 * w0 - a0 * a0 / 2.0)) / (a0 / sqrt(2.0));
    CHECK(d1 > 0.9 * a0, "DIRTY SUB keeps the fundamental (%g vs clean %g)", d1, a0);
    CHECK(thd_d > 0.12 && thd_d > 4.0 * thd_c, "DIRTY SUB adds overtones (%.3f vs clean %.3f)", thd_d, thd_c);
    double hl, hh;
    bands(cap, SR, SR, SR, 150, 2500, &hl, &hh);
    CHECK(hh < 0.02, "DIRTY SUB stays low end, no fizz (%g above 2.5 kHz)", hh);
    /* and it moves: the harmonic content wanders over seconds */
    double ha = window_rms(cap, SR / 2, SR / 4), hb = window_rms(cap, 2 * SR, SR / 4);
    (void)ha; (void)hb;
    /* BASS adds low end, monotonically */
    double prev = -1.0;
    int more = 1;
    for (int t = 0; t <= 4; ++t) {
        init_still(&E, SR);
        set(&E, DP_BASS, 0.25f * t);
        db_engine_restart(&E);
        run(&E, 2.0f);
        double a = tone(cap, SR / 2, SR, SR, sub);
        if (!(a > prev)) more = 0;
        prev = a;
    }
    CHECK(more, "BASS up = more low end at every step");
    /* headroom: default patch with BASS 1 + DIRTY 1 at VOLUME 1 stays under the
     * limiter nearly all the time */
    init_still(&E, SR);
    set(&E, DP_BASS, 1.0f);
    set(&E, DP_DIRTY_SUB, 1.0f);
    set(&E, DP_VOLUME, 1.0f);
    stats s = run(&E, 3.0f);
    CHECK(s.peak < 0.99 && E.lim_hits < 200, "BASS 1 + DIRTY 1: peak %g, limiter %u samples", s.peak, E.lim_hits);
    CHECK(fabs(s.dc) < 0.003, "no DC (%g)", s.dc);
}

/* ---- MOTION ------------------------------------------------------------------- */
static void test_motion(void) {
    db_motion m;
    db_engine_init(&E, SR);
    memcpy(&m, &E.mo, sizeof(m));
    for (int b = 0; b < 2000; ++b) db_motion_step(&m, 0.0f, BLOCK / (float)SR);
    int zero = 1;
    for (int i = 0; i < DB_MOTION_N; ++i) if (m.out[i] != 0.0f) zero = 0;
    CHECK(zero, "MOTION 0: every modulation exactly 0");
    /* continuous: no block-to-block step; covers a wide range; not periodic */
    float prev[DB_MOTION_N], lo[DB_MOTION_N], hi[DB_MOTION_N], maxstep = 0.0f;
    db_motion_step(&m, 1.0f, BLOCK / (float)SR);
    for (int i = 0; i < DB_MOTION_N; ++i) prev[i] = lo[i] = hi[i] = m.out[i];
    const int blocks = (int)(60.0f * SR / BLOCK);
    for (int b = 0; b < blocks; ++b) {
        db_motion_step(&m, 1.0f, BLOCK / (float)SR);
        for (int i = 0; i < DB_MOTION_N; ++i) {
            maxstep = fmaxf(maxstep, fabsf(m.out[i] - prev[i]));
            prev[i] = m.out[i];
            lo[i] = fminf(lo[i], m.out[i]);
            hi[i] = fmaxf(hi[i], m.out[i]);
        }
    }
    CHECK(maxstep < 0.05f, "MOTION 1 continuous: max step per block %g", maxstep);
    int wide = 1;
    for (int i = 0; i < DB_MOTION_N; ++i) if (hi[i] - lo[i] < 0.8f || hi[i] > 1.0f || lo[i] < -1.0f) wide = 0;
    CHECK(wide, "MOTION 1 spans a wide, bounded range on every channel");
    /* faster with more MOTION */
    db_motion a, b;
    memcpy(&a, &E.mo, sizeof(a)); memcpy(&b, &E.mo, sizeof(b));
    double tra = 0, trb = 0;
    float pa = 0, pb = 0;
    for (int k = 0; k < blocks; ++k) {
        db_motion_step(&a, 0.3f, BLOCK / (float)SR);
        db_motion_step(&b, 1.0f, BLOCK / (float)SR);
        if (k) { tra += fabsf(a.out[0] / 0.3f - pa); trb += fabsf(b.out[0] - pb); }
        pa = a.out[0] / 0.3f; pb = b.out[0];
    }
    CHECK(trb > 3.0 * tra, "MOTION 1 moves faster than 0.3 (%g vs %g)", trb, tra);

    /* in the engine: MOTION 0 holds every control still, MOTION 1 moves them
     * (the cutoff here) -- and never the subs */
    for (int t = 0; t < 2; ++t) {
        init_still(&E, SR);
        set(&E, DP_MOTION, (float)t);
        db_engine_restart(&E);
        run_into(&E, 1.0f, NULL);
        float cmin = 1e9f, cmax = -1e9f, fmn = 1e9f, fmx = -1e9f, smin = 1e9f, smax = -1e9f;
        float buf[BLOCK];
        for (int k = 0; k < (int)(20.0f * SR / BLOCK); ++k) {
            db_engine_render(&E, buf, BLOCK);
            float c = E.cut_oct + 1.2f * E.mo.out[DB_MO_CUT];
            cmin = fminf(cmin, c); cmax = fmaxf(cmax, c);
            fmn = fminf(fmn, E.fm_s[0]); fmx = fmaxf(fmx, E.fm_s[0]);
            smin = fminf(smin, E.sub_oct); smax = fmaxf(smax, E.sub_oct);
        }
        if (t == 0)
            CHECK(cmax - cmin < 1e-4f && fmx - fmn < 1e-4f, "MOTION 0: cutoff and FM hold still (%g, %g)",
                  cmax - cmin, fmx - fmn);
        else
            CHECK(cmax - cmin > 1.0f && fmx - fmn > 0.5f, "MOTION 1: cutoff and FM wander (%g oct, %g oct)",
                  cmax - cmin, fmx - fmn);
        CHECK(smax - smin < 1e-4f, "MOTION %d: the subs stay put", t);
    }
}

/* ---- no knob makes a dangerous level change ------------------------------------- */
static void test_knob_sweeps(void) {
    int bad = 0;
    for (int k = 0; k < DP_COUNT; ++k) {
        if (k == DP_RANDOM_STYLE) continue;
        init_still(&E, SR);
        set(&E, DP_MOTION, 0.5f);
        set(&E, DP_VOLUME, 1.0f);
        set(&E, k, db_params[k].min);
        db_engine_restart(&E);
        run(&E, 0.5f);
        double prev = -1.0, worst_jump = 0.0, worst_peak = 0.0;
        const int steps = 40;
        for (int s = 0; s <= steps; ++s) {
            float v = db_params[k].min + (db_params[k].max - db_params[k].min) * s / steps;
            set(&E, k, v);
            stats st = run(&E, 0.05f);
            double r = st.rms;
            worst_peak = fmax(worst_peak, st.peak);
            if (k != DP_VOLUME && prev > 1e-3 && r > 1e-3) worst_jump = fmax(worst_jump, db(r) - db(prev));
            if (st.nonfinite) bad = 1;
            prev = r;
        }
        CHECK(worst_jump < 9.0, "%s sweep: no sudden level rise (%.1f dB per 50 ms)", db_params[k].key, worst_jump);
        CHECK(worst_peak <= 0.99, "%s sweep: peak %g", db_params[k].key, worst_peak);
    }
    CHECK(!bad, "knob sweeps finite");
}

/* ---- rates, random abuse, all-max endurance ----------------------------------- */
static void all_max(db_engine *e) {
    for (int k = 0; k < DP_COUNT; ++k) set(e, k, db_params[k].max);
}

static void test_stress(float seconds) {
    const float rates[4] = { 22050, 44100, 48000, 96000 };
    for (int r = 0; r < 4; ++r) {
        for (int combo = 0; combo < 8; ++combo) {
            init_still(&E, rates[r]);
            set(&E, DP_FM_DRIVE, combo & 1 ? 1.0f : 0.0f);
            set(&E, DP_SYNC_SWEEP, combo & 2 ? 1.0f : 0.0f);
            set(&E, DP_VCF_RESO, combo & 4 ? 1.0f : 0.0f);
            set(&E, DP_VCO1_FREQ, combo & 1 ? 1.0f : 0.0f);
            set(&E, DP_DRIFT_X, combo & 2 ? 1.0f : -1.0f);
            set(&E, DP_DRIFT_Y, combo & 4 ? -1.0f : 1.0f);
            set(&E, DP_MOTION, 1.0f);
            set(&E, DP_DELAY, 1.0f);
            set(&E, DP_VOLUME, 1.0f);
            stats s = run_into(&E, 1.0f, NULL);
            CHECK(s.nonfinite == 0 && s.peak <= 1.0 && E.resets == 0,
                  "%g Hz combo %d: finite, bounded (peak %g, resets %u)", rates[r], combo, s.peak, E.resets);
            CHECK(s.rms > 0.01, "%g Hz combo %d: not silent (%g)", rates[r], combo, s.rms);
        }
    }
    /* every knob at its top at once, for a long time */
    init_still(&E, SR);
    all_max(&E);
    stats s = run_into(&E, seconds, NULL);
    CHECK(s.nonfinite == 0 && s.peak <= 1.0 && E.resets == 0 && s.rms > 0.05,
          "%.0f s all-max: rms %g peak %g resets %u", seconds, s.rms, s.peak, E.resets);
    /* random parameter abuse, transport toggled */
    init_still(&E, SR);
    uint32_t rng = 12345;
    float buf[BLOCK];
    int ok = 1;
    for (int b = 0; b < 20000; ++b) {
        rng = rng * 1664525u + 1013904223u;
        int k = (int)(rng >> 8) % DP_COUNT;
        rng = rng * 1664525u + 1013904223u;
        float v = db_params[k].min + (db_params[k].max - db_params[k].min) * (float)(rng >> 8) / 16777216.0f;
        set(&E, k, v);
        if (b % 997 == 0) db_engine_set_transport(&E, (b / 997) % 3 != 0);
        db_engine_render(&E, buf, BLOCK);
        for (int i = 0; i < BLOCK; ++i) if (!isfinite(buf[i]) || fabsf(buf[i]) > 1.0f) ok = 0;
    }
    CHECK(ok && E.resets == 0, "20000 random blocks: finite and bounded (resets %u)", E.resets);
}

/* ---- bench / demo ---------------------------------------------------------------- */
static void bench(void) {
    init_still(&E, SR);
    all_max(&E);
    float buf[BLOCK];
    const int blocks = (int)(60.0f * SR / BLOCK);
    clock_t t0 = clock();
    for (int b = 0; b < blocks; ++b) db_engine_render(&E, buf, BLOCK);
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: 60 s rendered in %.3f s (%.1fx realtime, %.2f%% of a core, %.1f us per block)\n",
           sec, 60.0 / sec, 100.0 * sec / 60.0, 1e6 * sec / blocks);
}

static void put32(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

/* 24 s: the default drone, then FM DRIVE, SYNC SWEEP, DRIFT X/Y and MOTION
 * moved in turn, then Stop. */
static void demo(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    const int secs = 24, n = SR * secs;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, SR); put32(f, SR * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, n * 2);
    db_engine_init(&E, SR);
    db_engine_set_transport(&E, 1);
    float buf[BLOCK];
    for (int b = 0; b < n / BLOCK; ++b) {
        float t = (float)b * BLOCK / SR;
        if (t > 4 && t < 8) set(&E, DP_FM_DRIVE, 0.3f + 0.6f * (t - 4) / 4);
        if (t > 8 && t < 12) set(&E, DP_SYNC_SWEEP, 0.35f + 0.5f * sinf((t - 8) * 0.8f));
        if (t > 12 && t < 16) { set(&E, DP_DRIFT_X, sinf((t - 12) * 1.2f)); set(&E, DP_DRIFT_Y, -sinf((t - 12) * 0.9f)); }
        if (t > 16) set(&E, DP_MOTION, 0.9f);
        if (t > 22) db_engine_set_transport(&E, 0);
        db_engine_render(&E, buf, BLOCK);
        for (int i = 0; i < BLOCK; ++i) put16(f, (unsigned)(int16_t)lrintf(buf[i] * 32767.0f) & 0xFFFF);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) { bench(); return 0; }
    if (argc > 2 && !strcmp(argv[1], "--wav")) { demo(argv[2]); return 0; }
    int quick = argc > 1 && !strcmp(argv[1], "--quick");
    test_params();
    test_transport();
    test_vco_drift_x();
    test_fm();
    test_fm_centre();
    test_sync();
    test_character_switch();
    test_drift_y();
    test_bass();
    test_motion();
    test_knob_sweeps();
    test_stress(quick ? 5.0f : 60.0f);
    printf("%s: %d checks\n", g_fail ? "FAILED" : "PASS: engine", g_checks);
    return g_fail;
}
