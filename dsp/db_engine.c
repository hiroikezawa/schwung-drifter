/* Driftbox SIII - engine. MIT. See db_engine.h. */
#include "db_engine.h"
#include <math.h>
#include <string.h>

/* key, min, max, default, is_int -- the order of enum DP_* and of
 * module.json's chain_params (checked by scripts/metadata.py). */
const db_param_info db_params[DP_COUNT] = {
    {"vco1_freq", 0.0f, 1.0f, 0.63f, 0},
    {"vco1_mod", 0.0f, 1.0f, 0.0f, 0},
    {"vco1_wave", 0.0f, 4.0f, 0.0f, 1},
    {"vco1_mode", 0.0f, 1.0f, 0.0f, 1},
    {"vco1_lfo", 0.0f, 1.0f, 0.0f, 1},
    {"vco1_vol", 0.0f, 1.0f, 0.8f, 0},
    {"vco2_freq", 0.0f, 1.0f, 0.5655f, 0},
    {"vco2_mod", 0.0f, 1.0f, 0.0f, 0},
    {"vco2_wave", 0.0f, 4.0f, 2.0f, 1},
    {"vco2_mode", 0.0f, 1.0f, 0.0f, 1},
    {"vco2_lfo", 0.0f, 1.0f, 0.0f, 1},
    {"vco2_vol", 0.0f, 1.0f, 0.5f, 0},
    {"vcf_cut", 0.0f, 1.0f, 0.7f, 0},
    {"vcf_reso", 0.0f, 1.0f, 0.3f, 0},
    {"vcf_mod", 0.0f, 1.0f, 0.0f, 0},
    {"vcf_vco1", 0.0f, 1.0f, 0.0f, 1},
    {"vcf_vco2", 0.0f, 1.0f, 0.0f, 1},
    {"vcf_lfo", 0.0f, 1.0f, 0.0f, 1},
    {"lfo_rate", 0.0f, 1.0f, 0.4f, 0},
    {"lfo_wave", 0.0f, 1.0f, 0.0f, 1},
    {"delay_time", 0.0f, 1.0f, 0.5f, 0},
    {"delay_feedback", 0.0f, 1.0f, 0.25f, 0},
    {"drift_x", -1.0f, 1.0f, 0.0f, 0},
    {"drift_x_target", 0.0f, 1.0f, 0.0f, 1},
    {"drift_y", -1.0f, 1.0f, 0.0f, 0},
    {"drift_y_target", 0.0f, 1.0f, 0.0f, 1},
    {"gate_hold", 0.0f, 2.0f, 1.0f, 1},
    {"trigger", 0.0f, 1.0f, 0.0f, 1},
    {"volume", 0.0f, 1.0f, 0.7f, 0},
};

/* ---- knob maps --------------------------------------------------------- */
#define VCO_SPAN_OCT 15.510175f      /* log2(14000 / 0.3) */
#define VCO_LO_OCT (-1.7369656f)     /* log2(0.3) */

static float unit(float v) { return db_clampf(v, 0.0f, 1.0f); }

static float vco_oct(float v) { return VCO_LO_OCT + unit(v) * VCO_SPAN_OCT; }
static float cut_oct(float v) { return 4.0f /* log2 16 */ + unit(v) * DB_VCF_OCT; }

float db_vco_hz(float v) { return exp2f(vco_oct(v)); }
float db_cut_hz(float v) { return exp2f(cut_oct(v)); }
float db_lfo_hz(float v) { return DB_LFO_LO_HZ * powf(10.0f, unit(v) * DB_LFO_DECADES); }
float db_delay_s(float v) { return DB_DELAY_LO_S * powf(10.0f, unit(v) * DB_DELAY_DECADES); }
float db_mod_depth(float v) { v = unit(v); return v * v; }

/* ---- halfband decimator: windowed-sinc (Blackman), 31 taps, 2:1 -------- */
static void hb_design(float *h) {
    const int c = DB_HB_TAPS / 2;
    for (int n = 0; n < DB_HB_TAPS; ++n) {
        int k = n - c;
        float w = 0.42f - 0.5f * cosf(2.0f * DB_PI * n / (DB_HB_TAPS - 1))
                        + 0.08f * cosf(4.0f * DB_PI * n / (DB_HB_TAPS - 1));
        float s = k == 0 ? 0.5f : (k % 2 == 0 ? 0.0f : sinf(0.5f * DB_PI * k) / (DB_PI * k));
        h[n] = s * w;
    }
    float sum = 0.0f;
    for (int n = 0; n < DB_HB_TAPS; ++n) sum += h[n];
    for (int n = 0; n < DB_HB_TAPS; ++n) h[n] /= sum;
}

/* The history is mirrored (2 x length) so the dot product never wraps. */
static inline void hb_push(db_engine *e, float x) {
    int p = e->hpos;
    e->hist[p] = e->hist[p + DB_HB_TAPS] = x;
    e->hpos = p + 1 == DB_HB_TAPS ? 0 : p + 1;
}

static inline float hb_out(const db_engine *e) {
    const float *x = &e->hist[e->hpos];      /* oldest .. newest */
    const int c = DB_HB_TAPS / 2;
    float acc = e->hb[c] * x[c];
    for (int n = (c & 1) ? 0 : 1; n < c; n += 2)
        acc += e->hb[n] * (x[n] + x[DB_HB_TAPS - 1 - n]);
    return acc;
}

/* ---- lifecycle ---------------------------------------------------------- */
static float delay_samples(const db_engine *e, float v, float x_mul) {
    return db_delay_s(v) * x_mul * e->sr;
}

void db_engine_restart(db_engine *e) {
    osc_reset(&e->osc[0], 0.0f, 0x1234567u);
    osc_reset(&e->osc[1], 0.37f, 0x89ABCDEu);     /* free-running analog VCOs */
    e->last[0] = e->last[1] = 0.0f;
    e->lfo_phase = 0.0f;
    e->lfo_sq = 1.0f;
    ladder_reset(&e->vcf);
    e->rng = 0x2545F491u;
    e->p_oct[0] = vco_oct(e->param[DP_VCO1_FREQ]);
    e->p_oct[1] = vco_oct(e->param[DP_VCO2_FREQ]);
    e->cut_oct = cut_oct(e->param[DP_VCF_CUT]);
    e->vol_mix[0] = e->param[DP_VCO1_VOL];
    e->vol_mix[1] = e->param[DP_VCO2_VOL];
    e->vca = 0.0f;
    e->trig_left = 0;
    delay_reset(&e->dly, delay_samples(e, e->param[DP_DELAY_TIME], 1.0f));
    e->fb_s = e->param[DP_DELAY_FB];
    e->wet_s = 0.0f;
    e->dc_x = e->dc_y = 0.0f;
    e->lim_gain = 1.0f;
    e->vol_s = e->param[DP_VOLUME];
    memset(e->hist, 0, sizeof(e->hist));
    e->hpos = 0;
}

void db_engine_init(db_engine *e, float sample_rate) {
    if (!(sample_rate >= 8000.0f && sample_rate <= 192000.0f)) sample_rate = 44100.0f;
    e->sr = sample_rate;
    e->osr = sample_rate * DB_OS;
    for (int i = 0; i < DP_COUNT; ++i) e->param[i] = db_params[i].def;
    e->mod_source[0] = 1;       /* VCO2 modulates / syncs VCO1 */
    e->mod_source[1] = 0;       /* VCO1 modulates / syncs VCO2 */
    e->gate = 0;
    e->transport = 0;
    e->note = DB_VCO_REF_NOTE;
    e->resets = e->lim_hits = 0;
    e->sm_coef = db_coef(0.003f, e->osr);          /* knob / note de-zipper */
    e->sq_coef = db_coef(0.0003f, e->osr);         /* LFO square edge */
    e->vca_coef = db_coef(0.0015f, e->sr);         /* ~3 ms to settle: click guard */
    e->fb_coef = db_coef(0.02f, e->sr);
    e->t_coef = db_coef(0.08f, e->sr);             /* delay-time glide */
    e->dc_r = 1.0f - 2.0f * DB_PI * 8.0f / e->sr;  /* 8 Hz DC blocker */
    e->lim_rel = db_coef(0.25f, e->sr);
    e->vol_coef = db_coef(0.015f, e->sr);
    hb_design(e->hb);
    db_engine_restart(e);
}

void db_engine_set_param(db_engine *e, int idx, float v) {
    if (idx < 0 || idx >= DP_COUNT || !(v == v)) return;
    const db_param_info *pi = &db_params[idx];
    v = db_clampf(v, pi->min, pi->max);
    if (pi->is_int) v = floorf(v + 0.5f);
    if (idx == DP_TRIGGER) {             /* momentary: fires, holds no value */
        if (v >= 1.0f) db_engine_trigger(e);
        return;
    }
    e->param[idx] = v;
}

void db_engine_set_gate(db_engine *e, int gate, int note) {
    e->gate = gate ? 1 : 0;
    if (note >= 0 && note <= 127) e->note = note;
}

void db_engine_set_transport(db_engine *e, int running) {
    e->transport = running ? 1 : 0;
}

void db_engine_trigger(db_engine *e) {
    e->trig_left = (int32_t)(DB_TRIG_S * e->sr);
}

/* ---- output safety ------------------------------------------------------ */
/* Above 0.7 the output bends smoothly into a 0.95 ceiling. */
static inline float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.7f) return x;
    float y = 0.7f + 0.25f * db_tanh((a - 0.7f) * (1.0f / 0.25f));
    return x < 0.0f ? -y : y;
}

#define DB_LIM_THRESH 1.5f

static int engine_finite(const db_engine *e) {
    float acc = e->osc[0].phase + e->osc[1].phase + e->last[0] + e->last[1]
              + e->p_oct[0] + e->p_oct[1] + e->cut_oct + e->lfo_phase + e->lfo_sq
              + e->vcf.s[0] + e->vcf.s[1] + e->vcf.s[2] + e->vcf.s[3]
              + e->vca + e->dly.t_s + e->dly.lp + e->dly.hp_x + e->dly.hp_y
              + e->dc_x + e->dc_y + e->lim_gain + e->vol_s;
    for (int i = 0; i < 2 * DB_HB_TAPS; ++i) acc += e->hist[i];
    return isfinite(acc);
}

/* ---- render ------------------------------------------------------------- */
void db_engine_render(db_engine *e, float *out, int frames) {
    if (frames <= 0) return;
    if (frames > DB_MAX_FRAMES) frames = DB_MAX_FRAMES;
    const float *P = e->param;
    const float osr = e->osr;

    /* block-rate targets */
    const float dx = P[DP_DRIFT_X], dy = P[DP_DRIFT_Y];
    const int x_vco = (int)P[DP_DRIFT_X_TGT] == DB_X_VCO;
    const int y_vcf = (int)P[DP_DRIFT_Y_TGT] == DB_Y_VCF;
    const float note_oct = (float)(e->note - DB_VCO_REF_NOTE) * (1.0f / 12.0f);
    const float drift_p = x_vco ? dx * DB_DRIFT_X_OCT : 0.0f;
    const float p_t[2] = { vco_oct(P[DP_VCO1_FREQ]) + note_oct + drift_p,
                           vco_oct(P[DP_VCO2_FREQ]) + note_oct + drift_p };
    const float cut_t = cut_oct(P[DP_VCF_CUT]) + (y_vcf ? dy * DB_DRIFT_Y_OCT : 0.0f);
    const float fm[2] = { db_mod_depth(P[DP_VCO1_MOD]) * DB_FM_OCT,
                          db_mod_depth(P[DP_VCO2_MOD]) * DB_FM_OCT };
    const int sync[2] = { (int)P[DP_VCO1_MODE] == DB_MODE_SYNC, (int)P[DP_VCO2_MODE] == DB_MODE_SYNC };
    const float lfo_on[2] = { P[DP_VCO1_LFO] >= 0.5f ? 1.0f : 0.0f, P[DP_VCO2_LFO] >= 0.5f ? 1.0f : 0.0f };
    const int wave[2] = { (int)P[DP_VCO1_WAVE], (int)P[DP_VCO2_WAVE] };
    const float vol_t[2] = { P[DP_VCO1_VOL], P[DP_VCO2_VOL] };
    const float vf1 = P[DP_VCF_VCO1] >= 0.5f ? 1.0f : 0.0f;
    const float vf2 = P[DP_VCF_VCO2] >= 0.5f ? 1.0f : 0.0f;
    const float vfl = P[DP_VCF_LFO] >= 0.5f ? 1.0f : 0.0f;
    const float vcf_depth = db_mod_depth(P[DP_VCF_MOD]) * DB_VCF_MOD_OCT;
    const float k = unit(P[DP_VCF_RESO]) * DB_RESO_K;
    const float in_gain = 0.6f * (1.0f + 0.5f * k);   /* part of the ladder's bass loss back */
    const float lfo_inc = db_lfo_hz(P[DP_LFO_RATE]) / osr;
    const int lfo_sqr = (int)P[DP_LFO_WAVE] == DB_LFO_SQR;
    const float hz_max = 0.45f * osr;
    const float oct_max = log2f(hz_max), oct_min = -6.6f;   /* 0.01 Hz */
    const float fc_max = fminf(20000.0f, 0.22f * osr);      /* keeps db_tan accurate */
    const float cut_max = log2f(fc_max), cut_min = 3.0f;    /* 8 Hz */
    const float pi_osr = DB_PI / osr;
    const int s1 = e->mod_source[0], s2 = e->mod_source[1];

    const float x_dt = x_vco ? 1.0f : exp2f(dx * DB_DRIFT_X_DT_OCT);
    const float dly_t = delay_samples(e, P[DP_DELAY_TIME], x_dt);
    float fb_t = P[DP_DELAY_FB] + (y_vcf ? 0.0f : dy * DB_DRIFT_Y_FB);
    fb_t = unit(fb_t);
    /* repeats darken with length, as a bucket brigade's clock slows */
    const float dly_s = dly_t / e->sr;
    const float lp_hz = fminf(9000.0f * powf(fmaxf(dly_s, 0.01f) / 0.01f, -0.25f), 0.4f * e->sr);
    const float lp_coef = 1.0f - expf(-2.0f * DB_PI * lp_hz / e->sr);
    const float hp_r = 1.0f - 2.0f * DB_PI * 20.0f / e->sr;
    /* GATE HOLD: Play holds the gate open while Move's transport runs,
     * Always holds it open regardless (a drone). */
    const int hold_mode = (int)P[DP_GATE_HOLD];
    const int hold = hold_mode == DB_HOLD_ALWAYS ||
                     (hold_mode == DB_HOLD_PLAY && e->transport);
    const float vol = P[DP_VOLUME];

    for (int i = 0; i < frames; ++i) {
        for (int j = 0; j < DB_OS; ++j) {
            /* LFO */
            float lp = e->lfo_phase + lfo_inc;
            if (lp >= 1.0f) { lp -= 1.0f; if (lp >= 1.0f) lp = 0.0f; }
            e->lfo_phase = lp;
            float lfo;
            if (lfo_sqr) {
                e->lfo_sq += ((lp < 0.5f ? 1.0f : -1.0f) - e->lfo_sq) * e->sq_coef;
                lfo = e->lfo_sq;
            } else {
                lfo = 1.0f - 4.0f * fabsf(lp - 0.5f);
                e->lfo_sq = lp < 0.5f ? 1.0f : -1.0f;     /* ready if switched */
            }

            /* de-zippered pitch, cutoff, mix */
            for (int v = 0; v < 2; ++v) {
                e->p_oct[v] += (p_t[v] - e->p_oct[v]) * e->sm_coef;
                e->vol_mix[v] += (vol_t[v] - e->vol_mix[v]) * e->sm_coef;
            }
            e->cut_oct += (cut_t - e->cut_oct) * e->sm_coef;

            /* cross FM from the other VCO's PREVIOUS sample; sync from its
             * previous-sample wrap -- both order independent */
            int wr1 = e->osc[s1].wrapped, wr2 = e->osc[s2].wrapped;
            float ag1 = e->osc[s1].age, ag2 = e->osc[s2].age;
            float m1 = fm[0] * (e->last[s1] + lfo_on[0] * lfo);
            float m2 = fm[1] * (e->last[s2] + lfo_on[1] * lfo);
            float inc1 = db_exp2(db_clampf(e->p_oct[0] + m1, oct_min, oct_max)) / osr;
            float inc2 = db_exp2(db_clampf(e->p_oct[1] + m2, oct_min, oct_max)) / osr;
            if (inc1 > 0.45f) inc1 = 0.45f;
            if (inc2 > 0.45f) inc2 = 0.45f;
            float o1 = osc_render(&e->osc[0], wave[0], inc1, sync[0] && wr1, ag1);
            float o2 = osc_render(&e->osc[1], wave[1], inc2, sync[1] && wr2, ag2);
            e->last[0] = o1;
            e->last[1] = o2;

            /* MIX -> VCF, cutoff modulated at audio rate */
            float mix = e->vol_mix[0] * o1 + e->vol_mix[1] * o2;
            float cm = (vf1 * o1 + vf2 * o2 + vfl * lfo) * vcf_depth;
            float c = db_clampf(e->cut_oct + cm, cut_min, cut_max);
            float g = db_tan(pi_osr * db_exp2(c));
            /* a whisper of noise lets a self-oscillating VCF start on its own */
            float x = mix * in_gain + 1e-5f * db_noise(&e->rng);
            hb_push(e, ladder_process(&e->vcf, x, g, k));
        }
        float y = hb_out(e);

        /* VCA: the gate, smoothed */
        int open = e->gate || hold || e->trig_left > 0;
        if (e->trig_left > 0) e->trig_left--;
        e->vca += ((open ? 1.0f : 0.0f) - e->vca) * e->vca_coef;
        if (!open && e->vca < 1e-6f) e->vca = 0.0f;
        float s = y * e->vca;

        /* DELAY: wet level follows FEEDBACK (0 = no delay at all) */
        e->fb_s += (fb_t - e->fb_s) * e->fb_coef;
        float fbg = 1.08f * e->fb_s;
        float wet_t = fminf(1.0f, 2.5f * e->fb_s) * 0.8f;
        e->wet_s += (wet_t - e->wet_s) * e->fb_coef;
        float d = delay_process(&e->dly, s, dly_t, fbg, e->t_coef, lp_coef, hp_r);
        float m = 1.6f * (s + e->wet_s * d);   /* output makeup (0.2.0: +6 dB) */

        /* DC blocker, emergency limiter, soft clip */
        float yo = m - e->dc_x + e->dc_r * e->dc_y;
        e->dc_x = m;
        e->dc_y = db_flush(yo);
        float pk = fabsf(yo) * e->lim_gain;
        if (pk > DB_LIM_THRESH) { e->lim_gain *= DB_LIM_THRESH / pk; e->lim_hits++; }
        else e->lim_gain += (1.0f - e->lim_gain) * e->lim_rel;
        yo = soft_clip(yo * e->lim_gain);

        /* VOLUME: glided, x^1.5 taper, exactly 0 at 0 */
        e->vol_s += (vol - e->vol_s) * e->vol_coef;
        if (vol == 0.0f && e->vol_s < 2e-3f) e->vol_s = 0.0f;
        out[i] = db_clampf(yo * e->vol_s * sqrtf(e->vol_s), -1.0f, 1.0f);
    }

    if (!engine_finite(e)) {
        /* Should never happen; if it does, recover in one block, silently. */
        e->resets++;
        db_engine_restart(e);
        memset(out, 0, (size_t)frames * sizeof(float));
    }
}
