/* DRIFTER engine tests. Built with ASan/UBSan by scripts/test.sh.
 *   test-engine            all tests (includes a 60 s stress render)
 *   test-engine --quick    skips the 60 s stress render
 *   bench --bench          realtime factor
 *   bench --wav out.wav    a short demo */
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

static db_engine E;
static float cap[CAP];

typedef struct {
    double rms, peak, dc, diff_rms;
    int nonfinite;
    int zc;             /* rising zero crossings, with hysteresis */
    long n;
} stats;

/* Render `seconds` of e; captures up to CAP samples into cap[]. */
static stats run(db_engine *e, float seconds) {
    stats s;
    memset(&s, 0, sizeof(s));
    float buf[BLOCK];
    long total = (long)(seconds * e->sr);
    float prev_s = 0.0f, last = 0.0f;
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
            if (prev_s < 0.0f && x > 0.01f) { s.zc++; prev_s = 1.0f; }
            else if (x < -0.01f) prev_s = -1.0f;
            if (s.n < CAP) cap[s.n] = x;
        }
    }
    s.rms = sqrt(s.rms / s.n);
    s.diff_rms = sqrt(s.diff_rms / s.n);
    s.dc /= s.n;
    return s;
}

static void set(db_engine *e, int k, float v) { db_engine_set_param(e, k, v); }

/* A plain, measurable voice: VCO1 saw only, VCF wide open, no delay. */
static void init_plain(db_engine *e, float sr) {
    db_engine_init(e, sr);
    set(e, DP_VCO2_VOL, 0.0f);
    set(e, DP_VCF_CUT, 1.0f);
    set(e, DP_VCF_RESO, 0.0f);
    set(e, DP_DELAY_FB, 0.0f);
    set(e, DP_VOLUME, 1.0f);
    set(e, DP_GATE_HOLD, 0.0f);    /* default Play (0.3.0): tests gate by note */
    set(e, DP_SUB, 0.0f);          /* default 0.5 (0.6.0): VCO pitch tests hear VCO1 alone */
    db_engine_restart(e);          /* smoothed values start AT these settings */
}

/* Fundamental from the autocorrelation peak of cap[from .. from+len). */
static float pitch_hz(int from, int len, float sr, float lo_hz, float hi_hz) {
    static double r[8192];
    int lag_lo = (int)(sr / hi_hz), lag_hi = (int)(sr / lo_hz) + 1;
    if (lag_hi > 8190) lag_hi = 8190;
    double best = -1e30;
    for (int lag = lag_lo - 1; lag <= lag_hi + 1; ++lag) {
        double acc = 0.0;
        for (int i = 0; i < len; ++i) acc += (double)cap[from + i] * cap[from + i + lag];
        r[lag] = acc;
        if (lag >= lag_lo && lag <= lag_hi && acc > best) best = acc;
    }
    /* the SHORTEST local peak within 10% of the best: the fundamental,
     * not a multiple of its period */
    int best_lag = lag_lo;
    for (int lag = lag_lo; lag <= lag_hi; ++lag)
        if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { best_lag = lag; break; }
    /* parabolic refinement */
    double a = r[best_lag - 1], b = r[best_lag], c = r[best_lag + 1];
    double den = a - 2 * b + c;
    double off = fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
    return (float)(sr / (best_lag + off));
}

static double window_rms(int from, int len) {
    double r = 0.0;
    for (int i = 0; i < len; ++i) r += (double)cap[from + i] * cap[from + i];
    return sqrt(r / len);
}

/* ---- 3/4/16: parameters -------------------------------------------------- */
static void test_params(void) {
    db_engine_init(&E, SR);
    for (int i = 0; i < DP_COUNT; ++i)
        CHECK(E.param[i] == db_params[i].def || i == DP_TRIGGER, "default %s", db_params[i].key);
    set(&E, DP_VCF_CUT, 5.0f);
    CHECK(E.param[DP_VCF_CUT] == 1.0f, "clamped high");
    set(&E, DP_DRIFT_X, -7.0f);
    CHECK(E.param[DP_DRIFT_X] == -1.0f, "drift clamped to -1");
    set(&E, DP_VCO1_WAVE, 2.6f);
    CHECK(E.param[DP_VCO1_WAVE] == 3.0f, "enum rounds");
    set(&E, DP_VCF_CUT, NAN);
    CHECK(E.param[DP_VCF_CUT] == 1.0f, "NaN ignored");
    set(&E, DP_TRIGGER, 1.0f);
    CHECK(E.param[DP_TRIGGER] == 0.0f && E.trig_left > 0, "trigger fires, keeps no value");
    /* maps */
    CHECK(fabsf(db_vco_hz(0.0f) - 0.3f) < 0.001f && fabsf(db_vco_hz(1.0f) / 14000.0f - 1.0f) < 1e-3f,
          "VCO FREQ 0.3 Hz .. 14 kHz: %f %f", db_vco_hz(0.0f), db_vco_hz(1.0f));
    CHECK(fabsf(db_vco_hz(0.63f) - 261.6f) < 2.0f, "default FREQ ~ C4: %f", db_vco_hz(0.63f));
    CHECK(fabsf(db_delay_s(0.0f) - 0.01f) < 1e-4f && fabsf(db_delay_s(1.0f) - 1.0f) < 1e-3f, "delay map");
    CHECK(db_lfo_hz(0.0f) < 0.03f && db_lfo_hz(1.0f) > 150.0f, "LFO range");
}

/* ---- 5/15: MIDI gate, note off, gate hold, trigger ---------------------------- */
static void test_gate(void) {
    init_plain(&E, SR);
    stats s = run(&E, 0.3f);
    CHECK(s.peak == 0.0, "no gate: silent (%g)", s.peak);
    db_engine_set_gate(&E, 1, 60);
    s = run(&E, 0.5f);
    CHECK(s.rms > 0.05 && s.nonfinite == 0, "note on: audio (rms %g)", s.rms);
    CHECK(fabs(s.dc) < 0.01, "no DC (%g)", s.dc);
    /* VCA smoothing: the first ms ramps, no step */
    init_plain(&E, SR);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.01f);
    CHECK(fabsf(cap[0]) < 0.05f, "gate opens without a click (%g)", cap[0]);
    db_engine_set_gate(&E, 0, -1);
    run(&E, 0.05f);
    s = run(&E, 0.2f);
    CHECK(s.peak < 1e-4, "note off stops (no delay): %g", s.peak);
    /* gate hold Play: open only while the transport runs */
    set(&E, DP_GATE_HOLD, (float)DB_HOLD_PLAY);
    s = run(&E, 0.3f);
    CHECK(s.peak < 1e-3, "hold Play, stopped: silent (%g)", s.peak);
    db_engine_set_transport(&E, 1);
    s = run(&E, 0.3f);
    CHECK(s.rms > 0.05, "hold Play, playing: sounds without a note (%g)", s.rms);
    db_engine_set_transport(&E, 0);
    run(&E, 0.3f);
    s = run(&E, 0.2f);
    CHECK(s.peak < 1e-3, "hold Play: Stop closes the gate (%g)", s.peak);
    db_engine_set_gate(&E, 1, 60);
    s = run(&E, 0.2f);
    CHECK(s.rms > 0.05, "hold Play, stopped: a note still plays");
    db_engine_set_gate(&E, 0, -1);
    /* gate hold Always: sounds with no note and no transport, and after a release */
    set(&E, DP_GATE_HOLD, (float)DB_HOLD_ALWAYS);
    s = run(&E, 0.3f);
    CHECK(s.rms > 0.05, "hold Always: sounds without a note (%g)", s.rms);
    db_engine_set_gate(&E, 1, 64);
    db_engine_set_gate(&E, 0, -1);
    s = run(&E, 0.3f);
    CHECK(s.rms > 0.05, "gate hold: stays on after note off");
    CHECK(E.note == 64, "release keeps the last pitch");
    set(&E, DP_GATE_HOLD, 0.0f);
    run(&E, 0.3f);                 /* VCA 3 ms, then only the 8 Hz DC blocker's tail */
    s = run(&E, 0.2f);
    CHECK(s.peak < 1e-3, "hold off: closes (%g)", s.peak);
    /* trigger: a ~200 ms gate, then silence */
    set(&E, DP_TRIGGER, 1.0f);
    s = run(&E, 0.15f);
    CHECK(s.rms > 0.05, "trigger opens the gate");
    run(&E, 0.3f);
    s = run(&E, 0.2f);
    CHECK(s.peak < 1e-3, "trigger closes by itself (%g)", s.peak);
}

/* ---- 6: VCO pitch, key tracking, waveforms ------------------------------------ */
static void test_vco(void) {
    const float want[] = { 261.63f, 523.25f, 130.81f };
    const int notes[] = { 60, 72, 48 };
    for (int w = 0; w < 4; ++w) {
        for (int n = 0; n < 3; ++n) {
            init_plain(&E, SR);
            set(&E, DP_VCO1_FREQ, 0.63f);
            set(&E, DP_VCO1_WAVE, (float)w);
            db_engine_set_gate(&E, 1, notes[n]);
            run(&E, 0.3f);
            float hz = pitch_hz(4410, 4096, SR, 60.0f, 1200.0f);
            CHECK(fabsf(hz / want[n] - 1.0f) < 0.01f, "wave %d note %d: %f Hz (want %f)",
                  w, notes[n], hz, want[n]);
        }
    }
    /* noise: no pitch, broadband */
    init_plain(&E, SR);
    set(&E, DP_VCO1_WAVE, 4.0f);
    db_engine_set_gate(&E, 1, 60);
    stats s = run(&E, 0.5f);
    CHECK(s.rms > 0.05 && s.diff_rms > 0.5 * s.rms, "noise is broadband (%g/%g)", s.diff_rms, s.rms);
    /* sub-audio end: FREQ 0 is a 0.3 Hz ramp -- slow, not silent */
    init_plain(&E, SR);
    set(&E, DP_VCO1_FREQ, 0.0f);
    db_engine_set_gate(&E, 1, 60);
    s = run(&E, 3.5f);
    CHECK(s.zc <= 2 && s.nonfinite == 0, "0.3 Hz: at most one cycle in 3.5 s (%d)", s.zc);
    /* VCO2 alone tracks too */
    init_plain(&E, SR);
    set(&E, DP_VCO1_VOL, 0.0f);
    set(&E, DP_VCO2_VOL, 1.0f);
    set(&E, DP_VCO2_WAVE, 0.0f);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.3f);
    float hz = pitch_hz(4410, 4096, SR, 60.0f, 1200.0f);
    CHECK(fabsf(hz / 130.8f - 1.0f) < 0.02f, "VCO2 default an octave below: %f", hz);
}

/* ---- 7: audio-rate cross FM ------------------------------------------------------ */
static void test_fm(void) {
    init_plain(&E, SR);
    set(&E, DP_VCO1_WAVE, 1.0f);             /* triangle carrier: FM shows as brightness */
    db_engine_set_gate(&E, 1, 60);
    stats dry = run(&E, 0.5f);
    init_plain(&E, SR);
    set(&E, DP_VCO1_WAVE, 1.0f);
    set(&E, DP_VCO1_MOD, 0.5f);              /* VCO2 (131 Hz, square) -> VCO1 */
    db_engine_set_gate(&E, 1, 60);
    stats fm = run(&E, 0.5f);
    CHECK(fm.nonfinite == 0 && fm.rms > 0.05, "FM finite");
    CHECK(fm.zc != dry.zc, "VCO2 -> VCO1 FM changes the waveform (%d vs %d zc)", fm.zc, dry.zc);
    /* audio-rate FM spreads energy upward (sidebands): the waveform gets brighter */
    CHECK(fm.diff_rms / fm.rms > 1.3 * dry.diff_rms / dry.rms, "FM adds sidebands (%g vs %g)",
          fm.diff_rms / fm.rms, dry.diff_rms / dry.rms);
    /* both ways at max depth: chaotic but bounded */
    init_plain(&E, SR);
    set(&E, DP_VCO1_MOD, 1.0f);
    set(&E, DP_VCO2_MOD, 1.0f);
    set(&E, DP_VCO2_VOL, 1.0f);
    db_engine_set_gate(&E, 1, 60);
    stats x = run(&E, 2.0f);
    CHECK(x.nonfinite == 0 && x.peak <= 1.0 && x.rms > 0.05, "max cross FM bounded (%g)", x.peak);
    CHECK(x.diff_rms > dry.diff_rms, "max cross FM is brighter/noisier");
}

/* ---- 8: hard sync ------------------------------------------------------------------ */
static void test_sync(void) {
    /* VCO2 synced to VCO1: VCO2 alone, tuned 1.7x up, must repeat at VCO1's pitch */
    init_plain(&E, SR);
    set(&E, DP_VCO1_VOL, 0.0f);
    set(&E, DP_VCO2_VOL, 1.0f);
    set(&E, DP_VCO2_WAVE, 0.0f);
    set(&E, DP_VCO2_FREQ, 0.63f + log2f(1.7f) / 15.510175f);
    set(&E, DP_VCO2_MODE, 1.0f);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.4f);
    float hz = pitch_hz(8820, 4096, SR, 100.0f, 1200.0f);
    CHECK(fabsf(hz / 261.6f - 1.0f) < 0.01f, "synced VCO2 repeats at VCO1: %f", hz);
    set(&E, DP_VCO2_MODE, 0.0f);
    run(&E, 0.4f);
    hz = pitch_hz(8820, 4096, SR, 100.0f, 1200.0f);
    CHECK(fabsf(hz / (261.6f * 1.7f) - 1.0f) < 0.02f, "FM mode (depth 0): VCO2 free: %f", hz);
    /* mutual sync: stable and sounding */
    init_plain(&E, SR);
    set(&E, DP_VCO1_MODE, 1.0f);
    set(&E, DP_VCO2_MODE, 1.0f);
    set(&E, DP_VCO2_VOL, 1.0f);
    set(&E, DP_VCO1_MOD, 0.7f);
    db_engine_set_gate(&E, 1, 60);
    stats s = run(&E, 1.0f);
    CHECK(s.nonfinite == 0 && s.rms > 0.05, "mutual sync sounds (%g)", s.rms);
    /* the routing is data, not code */
    CHECK(E.mod_source[0] == 1 && E.mod_source[1] == 0, "default routing VCO2<->VCO1");
}

/* ---- 10/11: VCF, self-oscillation, audio-rate modulation ----------------------- */
static void test_vcf(void) {
    init_plain(&E, SR);
    db_engine_set_gate(&E, 1, 60);
    stats open = run(&E, 0.5f);
    init_plain(&E, SR);
    set(&E, DP_VCF_CUT, 0.3f);                /* ~130 Hz */
    db_engine_set_gate(&E, 1, 60);
    stats shut = run(&E, 0.5f);
    CHECK(shut.diff_rms / shut.rms < 0.5 * open.diff_rms / open.rms,
          "low cutoff removes highs (%g vs %g)", shut.diff_rms / shut.rms, open.diff_rms / open.rms);
    /* self-oscillation with both VCOs off */
    init_plain(&E, SR);
    set(&E, DP_VCO1_VOL, 0.0f);
    set(&E, DP_VCF_RESO, 1.0f);
    set(&E, DP_VCF_CUT, (log2f(880.0f) - 4.0f) / 10.1f);
    db_engine_set_gate(&E, 1, 60);
    stats so = run(&E, 1.5f);
    CHECK(so.rms > 0.1 && so.nonfinite == 0, "VCF self-oscillates alone (rms %g)", so.rms);
    float hz = pitch_hz(44100, 4096, SR, 200.0f, 3000.0f);
    CHECK(fabsf(hz / 880.0f - 1.0f) < 0.08f, "self-oscillation near cutoff: %f", hz);
    set(&E, DP_VCF_RESO, 0.5f);
    run(&E, 0.5f);
    stats quiet = run(&E, 0.3f);
    CHECK(quiet.rms < 0.01, "below k=4 it dies away (%g)", quiet.rms);
    /* audio-rate cutoff FM from VCO2 (a sub-oscillator for VCO1) */
    init_plain(&E, SR);
    set(&E, DP_VCF_CUT, 0.5f);
    db_engine_set_gate(&E, 1, 60);
    stats base = run(&E, 0.5f);
    set(&E, DP_VCF_VCO2, 1.0f);
    set(&E, DP_VCF_MOD, 0.8f);
    stats mod = run(&E, 0.5f);
    CHECK(fabs(mod.diff_rms - base.diff_rms) > 0.1 * base.diff_rms, "VCO2 -> VCF changes the tone");
    float mhz = pitch_hz(8820, 4096, SR, 60.0f, 1200.0f);
    CHECK(fabsf(mhz / 130.8f - 1.0f) < 0.03f, "audio-rate VCF mod makes VCO2's period: %f", mhz);
    /* every source at max with max resonance: bounded */
    set(&E, DP_VCF_VCO1, 1.0f);
    set(&E, DP_VCF_LFO, 1.0f);
    set(&E, DP_VCF_MOD, 1.0f);
    set(&E, DP_VCF_RESO, 1.0f);
    stats wild = run(&E, 2.0f);
    CHECK(wild.nonfinite == 0 && wild.peak <= 1.0, "VCF MOD max + RESO max bounded");
}

/* ---- 12: LFO -------------------------------------------------------------------- */
static void test_lfo(void) {
    for (int w = 0; w < 2; ++w) {
        init_plain(&E, SR);
        set(&E, DP_VCO1_LFO, 1.0f);
        set(&E, DP_VCO1_MOD, 0.4f);            /* VCO2 must not FM for this one */
        set(&E, DP_VCO2_WAVE, 1.0f);
        set(&E, DP_VCO2_FREQ, 1.0f);           /* 14 kHz tri: FM averages to a fixed offset */
        set(&E, DP_LFO_RATE, (log10f(2.0f / 0.02f)) / 4.0f);   /* 2 Hz */
        set(&E, DP_LFO_WAVE, (float)w);
        db_engine_set_gate(&E, 1, 60);
        run(&E, 1.2f);
        float lo = 1e9f, hi = 0.0f;
        for (int k = 0; k < 8; ++k) {
            float hz = pitch_hz(4410 + k * 5512, 1024, SR, 50.0f, 2000.0f);
            lo = fminf(lo, hz); hi = fmaxf(hi, hz);
        }
        CHECK(hi / lo > 1.3f, "LFO %d -> VCO1 sweeps pitch (%f..%f)", w, lo, hi);
    }
    /* switch off: steady */
    init_plain(&E, SR);
    set(&E, DP_VCO1_MOD, 0.4f);
    set(&E, DP_VCO2_WAVE, 1.0f);
    set(&E, DP_VCO2_FREQ, 1.0f);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 1.2f);
    float a = pitch_hz(4410, 1024, SR, 50.0f, 2000.0f), b = pitch_hz(30000, 1024, SR, 50.0f, 2000.0f);
    CHECK(fabsf(a / b - 1.0f) < 0.1f, "LFO off: no sweep (%f %f)", a, b);
    /* LFO -> VCF: brightness moves */
    init_plain(&E, SR);
    set(&E, DP_VCF_CUT, 0.45f);
    set(&E, DP_VCF_LFO, 1.0f);
    set(&E, DP_VCF_MOD, 0.7f);
    set(&E, DP_LFO_RATE, (log10f(1.0f / 0.02f)) / 4.0f);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 1.5f);
    double w0 = 1e9, w1 = 0;
    for (int k = 0; k < 12; ++k) {
        double r = window_rms(k * 5000, 2048);
        w0 = fmin(w0, r); w1 = fmax(w1, r);
    }
    CHECK(w1 > 1.5 * w0, "LFO -> VCF moves the level (%g..%g)", w0, w1);
}

/* ---- 13: delay ----------------------------------------------------------------------- */
static void test_delay(void) {
    /* a 20 ms burst, then listen for its echo 300 ms later */
    init_plain(&E, SR);
    set(&E, DP_DELAY_TIME, log10f(0.3f / 0.01f) / 2.0f);
    set(&E, DP_DELAY_FB, 0.5f);
    run(&E, 0.1f);                              /* let fb/wet smoothing settle */
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.02f);
    db_engine_set_gate(&E, 0, -1);
    stats s = run(&E, 1.5f);
    double gap = window_rms((int)(0.15f * SR), 2048);
    double echo1 = window_rms((int)(0.28f * SR), (int)(0.04f * SR));
    double echo2 = window_rms((int)(0.58f * SR), (int)(0.04f * SR));
    CHECK(gap < 1e-3 && echo1 > 0.02, "echo at 300 ms (gap %g, echo %g)", gap, echo1);
    CHECK(echo2 > 0.2 * echo1 && echo2 < echo1, "feedback repeats and decays (%g %g)", echo1, echo2);
    CHECK(s.nonfinite == 0, "finite");
    /* feedback 0: no delay at all */
    init_plain(&E, SR);
    set(&E, DP_DELAY_TIME, log10f(0.3f / 0.01f) / 2.0f);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.02f);
    db_engine_set_gate(&E, 0, -1);
    run(&E, 1.0f);
    CHECK(window_rms((int)(0.28f * SR), (int)(0.04f * SR)) < 1e-4, "FEEDBACK 0 = dry");
    /* max feedback: bounded, still going 20 s later */
    init_plain(&E, SR);
    set(&E, DP_DELAY_FB, 1.0f);
    set(&E, DP_DELAY_TIME, 0.3f);
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.5f);
    db_engine_set_gate(&E, 0, -1);
    stats m = run(&E, 20.0f);
    CHECK(m.nonfinite == 0 && m.peak <= 1.0, "max feedback bounded (%g)", m.peak);
    double tail = window_rms(CAP - 8192, 8192);
    CHECK(tail > 1e-3, "max feedback self-sustains (%g)", tail);
}

/* ---- 14: DRIFT ------------------------------------------------------------------------- */
static void test_drift(void) {
    /* X -> VCO: +-3 octaves on both VCOs */
    init_plain(&E, SR);
    set(&E, DP_DRIFT_X, 1.0f);
    db_engine_set_gate(&E, 1, 48);
    run(&E, 0.3f);
    float hz = pitch_hz(4410, 4096, SR, 200.0f, 2000.0f);
    CHECK(fabsf(hz / (130.81f * 8.0f) - 1.0f) < 0.02f, "DRIFT X +1 on VCO: 3 octaves up (%f)", hz);
    set(&E, DP_DRIFT_X, -0.5f);
    run(&E, 0.3f);
    hz = pitch_hz(4410, 4096, SR, 20.0f, 200.0f);
    CHECK(fabsf(hz / (130.81f / 2.828f) - 1.0f) < 0.02f, "DRIFT X -0.5: 1.5 octaves down (%f)", hz);
    /* X -> D.TIME: no pitch change, the echo moves */
    set(&E, DP_DRIFT_X_TGT, 1.0f);
    run(&E, 0.3f);
    hz = pitch_hz(4410, 4096, SR, 60.0f, 400.0f);
    CHECK(fabsf(hz / 130.81f - 1.0f) < 0.02f, "X on D.TIME leaves pitch alone (%f)", hz);
    init_plain(&E, SR);
    set(&E, DP_DELAY_TIME, log10f(0.2f / 0.01f) / 2.0f);     /* 200 ms */
    set(&E, DP_DELAY_FB, 0.4f);
    set(&E, DP_DRIFT_X_TGT, 1.0f);
    set(&E, DP_DRIFT_X, 0.5f);                                /* x2 -> 400 ms */
    run(&E, 1.0f);                                            /* glide settles */
    db_engine_set_gate(&E, 1, 60);
    run(&E, 0.02f);
    db_engine_set_gate(&E, 0, -1);
    run(&E, 1.0f);
    double at200 = window_rms((int)(0.19f * SR), (int)(0.03f * SR));
    double at400 = window_rms((int)(0.39f * SR), (int)(0.03f * SR));
    CHECK(at400 > 0.02 && at200 < 0.2 * at400, "X on D.TIME doubles the delay (%g @200, %g @400)", at200, at400);
    /* Y -> VCF */
    init_plain(&E, SR);
    set(&E, DP_VCF_CUT, 0.5f);
    db_engine_set_gate(&E, 1, 60);
    stats mid = run(&E, 0.4f);
    set(&E, DP_DRIFT_Y, -1.0f);
    run(&E, 0.1f);
    stats dark = run(&E, 0.4f);
    CHECK(dark.rms < 0.5 * mid.rms, "DRIFT Y -1 on VCF closes it (%g vs %g)", dark.rms, mid.rms);
    /* Y -> F.B: more tail */
    double tails[2];
    for (int k = 0; k < 2; ++k) {
        init_plain(&E, SR);
        set(&E, DP_DELAY_FB, 0.3f);
        set(&E, DP_DELAY_TIME, 0.3f);
        set(&E, DP_DRIFT_Y_TGT, 1.0f);
        set(&E, DP_DRIFT_Y, k ? 1.0f : 0.0f);
        run(&E, 0.2f);
        db_engine_set_gate(&E, 1, 60);
        run(&E, 0.05f);
        db_engine_set_gate(&E, 0, -1);
        run(&E, 2.0f);
        tails[k] = window_rms((int)(1.5f * SR), (int)(0.5f * SR));
    }
    CHECK(tails[1] > 4.0 * tails[0], "DRIFT Y on F.B lengthens the tail (%g vs %g)", tails[1], tails[0]);
}

/* ---- SUB: a sine an octave below VCO1, after the VCF -------------------------- */
static double goertzel(int from, int len, float sr, float hz) {
    double w = 2.0 * M_PI * hz / sr, c = 2.0 * cos(w), s0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i) { s0 = cap[from + i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / len;
}

static void test_sub(void) {
    double lvl[2];
    for (int k = 0; k < 2; ++k) {
        init_plain(&E, SR);
        set(&E, DP_VCF_CUT, 0.45f);            /* ~370 Hz: the VCF thins the VCO's bass */
        set(&E, DP_VCF_RESO, 0.9f);            /* and resonance thins it more */
        set(&E, DP_SUB, k ? 1.0f : 0.0f);
        db_engine_set_gate(&E, 1, 48);         /* VCO1 130.8 Hz -> SUB 65.4 Hz */
        run(&E, 0.5f);
        lvl[k] = goertzel(8820, 8192, SR, 65.41f);
    }
    CHECK(lvl[1] > 0.1 && lvl[1] > 20.0 * lvl[0], "SUB adds the octave below (%g vs %g)", lvl[1], lvl[0]);
    /* it follows VCO1's pitch BEFORE FM: heavy FM leaves it in place */
    init_plain(&E, SR);
    set(&E, DP_SUB, 1.0f);
    set(&E, DP_VCO1_VOL, 0.0f);
    set(&E, DP_VCO1_MOD, 1.0f);
    db_engine_set_gate(&E, 1, 48);
    run(&E, 0.5f);
    CHECK(goertzel(8820, 8192, SR, 65.41f) > 0.1, "SUB steady under max FM");
    /* and the default patch keeps peak below full scale */
    db_engine_init(&E, SR);
    set(&E, DP_VOLUME, 1.0f);
    db_engine_set_gate(&E, 1, 36);
    stats s = run(&E, 1.0f);
    CHECK(s.peak <= 1.0 && s.nonfinite == 0 && E.resets == 0, "default + SUB bounded (%g)", s.peak);
}

/* ---- 18: stress --------------------------------------------------------------------- */
static void all_max(db_engine *e) {
    set(e, DP_VCO1_MOD, 1.0f); set(e, DP_VCO2_MOD, 1.0f);
    set(e, DP_VCO1_MODE, 1.0f); set(e, DP_VCO2_MODE, 0.0f);
    set(e, DP_VCO1_LFO, 1.0f); set(e, DP_VCO2_LFO, 1.0f);
    set(e, DP_VCO1_VOL, 1.0f); set(e, DP_VCO2_VOL, 1.0f);
    set(e, DP_VCF_RESO, 1.0f); set(e, DP_VCF_MOD, 1.0f);
    set(e, DP_VCF_VCO1, 1.0f); set(e, DP_VCF_VCO2, 1.0f); set(e, DP_VCF_LFO, 1.0f);
    set(e, DP_LFO_RATE, 1.0f); set(e, DP_LFO_WAVE, 1.0f);
    set(e, DP_DELAY_FB, 1.0f); set(e, DP_DRIFT_X, 1.0f); set(e, DP_DRIFT_Y, 1.0f);
    set(e, DP_GATE_HOLD, (float)DB_HOLD_ALWAYS); set(e, DP_VOLUME, 1.0f); set(e, DP_SUB, 1.0f);
}

static void test_stress(float seconds) {
    const float rates[] = { 22050.0f, 44100.0f, 48000.0f, 96000.0f };
    for (int r = 0; r < 4; ++r) {
        db_engine_init(&E, rates[r]);
        all_max(&E);
        for (int corner = 0; corner < 16; ++corner) {
            set(&E, DP_VCO1_FREQ, (corner & 1) ? 1.0f : 0.0f);
            set(&E, DP_VCO2_FREQ, (corner & 2) ? 1.0f : 0.0f);
            set(&E, DP_VCF_CUT, (corner & 4) ? 1.0f : 0.0f);
            set(&E, DP_DRIFT_X, (corner & 8) ? -1.0f : 1.0f);
            set(&E, DP_DRIFT_X_TGT, (float)(corner & 1));
            set(&E, DP_DRIFT_Y_TGT, (float)((corner >> 1) & 1));
            set(&E, DP_VCO1_WAVE, (float)(corner % 5));
            set(&E, DP_VCO2_WAVE, (float)((corner * 3) % 5));
            db_engine_set_gate(&E, 1, corner * 8);
            stats s = run(&E, 0.5f);
            CHECK(s.nonfinite == 0 && s.peak <= 1.0, "corner %d @%g finite+bounded", corner, rates[r]);
        }
        CHECK(E.resets == 0, "no NaN recoveries @%g (%u)", rates[r], E.resets);
    }
    /* long run, every modulation at once */
    db_engine_init(&E, SR);
    all_max(&E);
    db_engine_set_gate(&E, 1, 60);
    stats s = run(&E, seconds);
    CHECK(s.nonfinite == 0 && s.peak <= 1.0 && E.resets == 0,
          "%.0f s all-max render: finite, bounded (peak %g, resets %u)", seconds, s.peak, E.resets);
    CHECK(s.rms > 0.01, "all-max still sounds (%g)", s.rms);
    /* random parameter fuzz, changing every block */
    db_engine_init(&E, SR);
    srand(1);
    float buf[BLOCK];
    int bad = 0;
    for (int b = 0; b < 20000; ++b) {
        int k = rand() % DP_COUNT;
        float v = db_params[k].min + (db_params[k].max - db_params[k].min) * (float)rand() / RAND_MAX;
        set(&E, k, v);
        if (b % 97 == 0) db_engine_set_gate(&E, rand() & 1, rand() % 128);
        db_engine_render(&E, buf, BLOCK);
        for (int i = 0; i < BLOCK; ++i) if (!isfinite(buf[i]) || fabsf(buf[i]) > 1.0f) bad++;
    }
    CHECK(bad == 0 && E.resets == 0, "parameter fuzz: finite (%d bad, %u resets)", bad, E.resets);
}

/* ---- bench / demo ---------------------------------------------------------------- */
static void bench(void) {
    db_engine_init(&E, SR);
    all_max(&E);
    db_engine_set_gate(&E, 1, 60);
    float buf[BLOCK];
    const int blocks = (int)(60.0f * SR / BLOCK);
    clock_t t0 = clock();
    for (int b = 0; b < blocks; ++b) db_engine_render(&E, buf, BLOCK);
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: 60 s rendered in %.3f s (%.1fx realtime, %.2f%% of a core)\n",
           sec, 60.0 / sec, 100.0 * sec / 60.0);
}

static void put32(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

static void demo(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    const int secs = 12, n = SR * secs;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, SR); put32(f, SR * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, n * 2);
    db_engine_init(&E, SR);
    set(&E, DP_DELAY_FB, 0.45f);
    set(&E, DP_VCF_RESO, 0.6f);
    float buf[BLOCK];
    const int seq[] = { 48, 55, 60, 51 };
    for (int b = 0; b < n / BLOCK; ++b) {
        float t = (float)b * BLOCK / SR;
        int step = (int)(t * 4.0f);
        db_engine_set_gate(&E, (fmodf(t * 4.0f, 1.0f) < 0.5f), seq[step % 4]);
        set(&E, DP_VCO1_MOD, t < 4 ? 0.0f : 0.25f + 0.1f * sinf(t));
        set(&E, DP_VCO2_MODE, t > 8 ? 1.0f : 0.0f);
        set(&E, DP_DRIFT_Y, sinf(t * 0.7f));
        set(&E, DP_VCF_VCO2, t > 6 ? 1.0f : 0.0f);
        set(&E, DP_VCF_MOD, 0.4f);
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
    test_gate();
    test_vco();
    test_fm();
    test_sync();
    test_vcf();
    test_lfo();
    test_delay();
    test_drift();
    test_sub();
    test_stress(quick ? 5.0f : 60.0f);
    printf("%s: %d checks\n", g_fail ? "FAILED" : "PASS: engine", g_checks);
    return g_fail;
}
