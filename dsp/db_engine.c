/* Drifter - engine. MIT. See db_engine.h. */
#include "db_engine.h"
#include <math.h>
#include <string.h>

/* key, min, max, default, is_int -- the order of enum DP_* and of
 * module.json's chain_params (checked by scripts/metadata.py). */
const db_param_info db_params[DP_COUNT] = {
    {"volume", 0.0f, 1.0f, 0.7f, 0},
    {"drift_x", -1.0f, 1.0f, 0.0f, 0},
    {"drift_y", -1.0f, 1.0f, 0.0f, 0},
    {"fm_drive", 0.0f, 1.0f, 0.3f, 0},
    {"sync_sweep", 0.0f, 1.0f, 0.35f, 0},
    {"vcf_cut", 0.0f, 1.0f, 0.55f, 0},
    {"vcf_reso", 0.0f, 1.0f, 0.35f, 0},
    {"motion", 0.0f, 1.0f, 0.35f, 0},
    {"vco1_freq", 0.0f, 1.0f, 0.25f, 0},
    {"vco2_freq", 0.0f, 1.0f, 0.3975f, 0},
    {"vco_mix", 0.0f, 1.0f, 0.45f, 0},
    {"bass", 0.0f, 1.0f, 0.6f, 0},
    {"dirty_sub", 0.0f, 1.0f, 0.35f, 0},
    {"delay", 0.0f, 1.0f, 0.3f, 0},
    {"character", 0.0f, 2.0f, 2.0f, 1},
    {"random_style", 0.0f, 2.0f, 0.0f, 1},
};

/* ---- knob maps --------------------------------------------------------- */
#define VCO_LO_OCT 4.7813597f        /* log2(27.5) */
#define SUB_LO_OCT 5.3219281f        /* log2(40) */
#define CUT_LO_OCT 5.3219281f        /* log2(40) */

static float unit(float v) { return db_clampf(v, 0.0f, 1.0f); }

static float vco_oct(float v) { return VCO_LO_OCT + unit(v) * DB_VCO_OCT; }
static float cut_oct(float v) { return CUT_LO_OCT + unit(v) * DB_VCF_OCT; }
/* VCO1's pitch, moved by whole octaves into [40, 80) Hz: the subs always sit
 * where a speaker and a kick drum leave room, and always on VCO1's note. */
static float sub_oct(float vco1) {
    float o = vco_oct(vco1);
    while (o >= SUB_LO_OCT + 1.0f) o -= 1.0f;
    while (o < SUB_LO_OCT) o += 1.0f;
    return o;
}

float db_vco_hz(float v) { return exp2f(vco_oct(v)); }
float db_sub_hz(float vco1) { return exp2f(sub_oct(vco1)); }
float db_cut_hz(float v) { return exp2f(cut_oct(v)); }
/* x^1.3: a third of the way up is already metallic, the top is chaos */
float db_fm_oct(float v) { return DB_FM_OCT * powf(unit(v), 1.3f); }
float db_delay_s(float v) { return 0.06f * powf(15.0f, unit(v)); }        /* 60 ms .. 0.9 s */
float db_delay_fb(float v) { return 0.12f + 0.63f * powf(unit(v), 1.2f); } /* never past 0.75 */
float db_delay_wet(float v) { return 0.5f * powf(unit(v), 0.8f); }         /* 0 = no delay */

/* sin(2 pi ph) for any ph: odd 7th-order series on the folded quarter. */
static inline float sin2pi(float ph) {
    float x = ph - floorf(ph) - 0.5f;          /* -0.5 .. 0.5; sin(2pi ph) = -sin(2pi x) */
    if (x > 0.25f) x = 0.5f - x;
    else if (x < -0.25f) x = -0.5f - x;
    float w = 2.0f * DB_PI * x, w2 = w * w;
    return -w * (1.0f - w2 * (0.16666667f - w2 * (0.0083333f - w2 * 0.00019841f)));
}

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

/* ---- MOTION ------------------------------------------------------------- */
/* Five channels, each a sine at its own (mutually irrational) rate plus a
 * random walk that glides to a new target every 1.5..4.5 s with a smoothstep:
 * never periodic, never a step, never a rhythm. MOTION sets the depth
 * (linearly, so 0 is exactly none) and the speed (0.35x .. 3.35x); above 0.6
 * a faster, smaller walk joins in for the chaotic end. */
static const float mo_rate_hz[DB_MOTION_N] = { 0.051f, 0.083f, 0.037f, 0.067f, 0.029f };

static float mo_rand(db_motion *m) { return db_noise(&m->rng); }
static float smooth01(float t) { t = unit(t); return t * t * (3.0f - 2.0f * t); }

static void motion_reset(db_motion *m) {
    memset(m, 0, sizeof(*m));
    m->rng = 0x6D2B79F5u;
    for (int i = 0; i < DB_MOTION_N; ++i) {
        m->sin_ph[i] = 0.17f * (float)i;
        m->rw_to[i] = mo_rand(m);
        m->rw_len[i] = 2.0f + 0.5f * (float)i;
        m->jit_to[i] = mo_rand(m);
    }
}

void db_motion_step(db_motion *m, float motion, float seconds) {
    motion = unit(motion);
    if (motion <= 0.0f) {                       /* stopped: no walk, no output */
        for (int i = 0; i < DB_MOTION_N; ++i) m->out[i] = 0.0f;
        return;
    }
    const float rate = 0.35f + 3.0f * motion * motion;
    const float dt = seconds * rate;
    const float jw = 0.5f * unit((motion - 0.6f) / 0.4f);
    m->jit_t += dt * 1.6f;
    int jnew = m->jit_t >= 1.0f;
    if (jnew) m->jit_t -= floorf(m->jit_t);
    for (int i = 0; i < DB_MOTION_N; ++i) {
        float ph = m->sin_ph[i] + mo_rate_hz[i] * dt;
        m->sin_ph[i] = ph - floorf(ph);
        m->rw_t[i] += dt / m->rw_len[i];
        if (m->rw_t[i] >= 1.0f) {
            m->rw_t[i] = 0.0f;
            m->rw_from[i] = m->rw_to[i];
            m->rw_to[i] = mo_rand(m);
            m->rw_len[i] = 3.0f + 1.5f * mo_rand(m);
        }
        if (jnew) { m->jit_from[i] = m->jit_to[i]; m->jit_to[i] = mo_rand(m); }
        float rw = m->rw_from[i] + (m->rw_to[i] - m->rw_from[i]) * smooth01(m->rw_t[i]);
        float jt = m->jit_from[i] + (m->jit_to[i] - m->jit_from[i]) * smooth01(m->jit_t);
        float s = 0.45f * sin2pi(m->sin_ph[i]) + 0.55f * rw + jw * jt;
        m->out[i] = motion * db_clampf(s, -1.0f, 1.0f);
    }
}

/* ---- lifecycle ---------------------------------------------------------- */
void db_engine_restart(db_engine *e) {
    const float *P = e->param;
    osc_reset(&e->osc[0], 0.0f);
    osc_reset(&e->osc[1], 0.37f);
    e->tri[0] = osc_tri(0.0f);
    e->tri[1] = osc_tri(0.37f);
    e->tri2_dc = 0.0f;
    ladder_reset(&e->vcf);
    e->rng = 0x2545F491u;
    e->p_oct[0] = vco_oct(P[DP_VCO1_FREQ]);
    e->p_oct[1] = vco_oct(P[DP_VCO2_FREQ]);
    e->sub_oct = sub_oct(P[DP_VCO1_FREQ]);
    e->cut_oct = cut_oct(P[DP_VCF_CUT]);
    e->fm_s[0] = e->fm_s[1] = 0.0f;
    e->vcfm_s = 0.0f;
    e->mix_s = P[DP_VCO_MIX];
    e->clean_s = e->dirty_s = 0.0f;
    e->dirty_amt_s = P[DP_DIRTY_SUB];
    for (int i = 0; i < DB_MOTION_N; ++i) e->mo_prev[i] = e->mo.out[i];
    e->sub_phase = 0.0f;
    e->dsub_lp = 0.0f;
    e->wander_ph = 0.0f;
    e->character = (int)P[DP_CHARACTER];
    e->duck = 1.0f;
    e->fade = 0.0f;
    delay_reset(&e->dly, db_delay_s(P[DP_DELAY]) * e->sr);
    e->fb_s = db_delay_fb(P[DP_DELAY]);
    e->wet_s = db_delay_wet(P[DP_DELAY]);
    e->hp_z1 = e->hp_z2 = 0.0f;
    e->lim_gain = 1.0f;
    e->vol_s = P[DP_VOLUME];
    memset(e->hist, 0, sizeof(e->hist));
    e->hpos = 0;
}

void db_engine_init(db_engine *e, float sample_rate) {
    if (!(sample_rate >= 8000.0f && sample_rate <= 192000.0f)) sample_rate = 44100.0f;
    e->sr = sample_rate;
    e->osr = sample_rate * DB_OS;
    for (int i = 0; i < DP_COUNT; ++i) e->param[i] = db_params[i].def;
    e->transport = 0;
    e->idle = 1;
    e->resets = e->lim_hits = 0;
    e->sm_coef = db_coef(0.010f, e->osr);          /* knob de-zipper */
    e->slow_coef = db_coef(0.040f, e->osr);        /* sub octave fold: a glide */
    e->dc_coef = db_coef(0.050f, e->osr);          /* VCO2 triangle's bias */
    e->duck_coef = db_coef(0.0015f, e->sr);
    e->fade_in = 1.0f / (DB_FADE_IN_S * e->sr);
    e->fade_out = 1.0f / (DB_FADE_OUT_S * e->sr);
    e->fb_coef = db_coef(0.05f, e->sr);
    e->t_coef = db_coef(0.12f, e->sr);             /* delay-time glide */
    e->lim_rel = db_coef(0.25f, e->sr);
    e->vol_coef = db_coef(0.015f, e->sr);
    {   /* 25 Hz Butterworth high pass (RBJ): nothing below the subs */
        float w = 2.0f * DB_PI * 25.0f / e->sr, cw = cosf(w);
        float al = sinf(w) / (2.0f * 0.70710678f), a0 = 1.0f + al;
        e->hp_b0 = 0.5f * (1.0f + cw) / a0;
        e->hp_b1 = -(1.0f + cw) / a0;
        e->hp_b2 = e->hp_b0;
        e->hp_a1 = -2.0f * cw / a0;
        e->hp_a2 = (1.0f - al) / a0;
    }
    hb_design(e->hb);
    motion_reset(&e->mo);
    db_engine_restart(e);
}

void db_engine_set_param(db_engine *e, int idx, float v) {
    if (idx < 0 || idx >= DP_COUNT || !(v == v)) return;
    const db_param_info *pi = &db_params[idx];
    v = db_clampf(v, pi->min, pi->max);
    if (pi->is_int) v = floorf(v + 0.5f);
    e->param[idx] = v;
}

void db_engine_set_transport(db_engine *e, int running) {
    e->transport = running ? 1 : 0;
}

/* ---- output safety ------------------------------------------------------ */
/* Above 0.8 the output bends smoothly into a 0.98 ceiling. */
static inline float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.18f * db_tanh((a - 0.8f) * (1.0f / 0.18f));
    return x < 0.0f ? -y : y;
}

#define DB_LIM_THRESH 0.95f

static int engine_finite(const db_engine *e) {
    float acc = e->osc[0].phase + e->osc[1].phase + e->osc[1].pending + e->tri[0] + e->tri[1]
              + e->p_oct[0] + e->p_oct[1] + e->sub_oct + e->cut_oct + e->fm_s[0] + e->fm_s[1]
              + e->vcfm_s + e->tri2_dc + e->sub_phase + e->dsub_lp + e->clean_s + e->dirty_s
              + e->vcf.s[0] + e->vcf.s[1] + e->vcf.s[2] + e->vcf.s[3]
              + e->dly.t_s + e->dly.lp + e->dly.hp_x + e->dly.hp_y
              + e->hp_z1 + e->hp_z2 + e->lim_gain + e->vol_s + e->duck + e->fade;
    for (int i = 0; i < 2 * DB_HB_TAPS; ++i) acc += e->hist[i];
    return isfinite(acc);
}

/* ---- render ------------------------------------------------------------- */
void db_engine_render(db_engine *e, float *out, int frames) {
    if (frames <= 0) return;
    if (frames > DB_MAX_FRAMES) frames = DB_MAX_FRAMES;

    /* Stopped and faded out: nothing to compute. The next Play starts from a
     * clean slate (phases, filter, an empty delay line) and fades in. */
    if (!e->transport && e->fade <= 0.0f) {
        e->idle = 1;
        memset(out, 0, (size_t)frames * sizeof(float));
        return;
    }
    if (e->idle) { db_engine_restart(e); e->idle = 0; }

    const float *P = e->param;
    const float osr = e->osr;

    /* CHARACTER: duck, swap at the bottom, come back */
    const int want_char = (int)P[DP_CHARACTER];
    if (want_char != e->character && e->duck < 0.01f) e->character = want_char;
    const float duck_t = want_char != e->character ? 0.0f : 1.0f;
    const int ch = e->character;
    const int sync_on = ch != DB_CHAR_FM;

    /* MOTION: one step per block, ramped across it (mo_prev -> mo.out) */
    float mo0[DB_MOTION_N], dmo[DB_MOTION_N];
    db_motion_step(&e->mo, P[DP_MOTION], (float)frames / e->sr);
    for (int i = 0; i < DB_MOTION_N; ++i) {
        mo0[i] = e->mo_prev[i];
        dmo[i] = (e->mo.out[i] - mo0[i]) / (float)(frames * DB_OS);
        e->mo_prev[i] = e->mo.out[i];
    }
    const float *mo = e->mo.out;

    /* block-rate targets */
    const float fm_k = unit(P[DP_FM_DRIVE] + 0.30f * mo[DB_MO_FM]);
    const float D = db_fm_oct(fm_k);
    const float fm_t[2] = {
        ch == DB_CHAR_SYNC ? 0.0f : D,                    /* VCO2 -> VCO1 */
        ch == DB_CHAR_SYNC ? D : DB_FM_BACK * D,          /* VCO1 -> VCO2 */
    };
    const float sweep = unit(P[DP_SYNC_SWEEP] + 0.25f * mo[DB_MO_SYNC]);
    const float x = P[DP_DRIFT_X];
    /* fine at the centre; to the right it reaches far higher than to the left */
    const float x_oct = x * (0.3f + 0.7f * fabsf(x)) * (x > 0.0f ? DB_DRIFT_X_UP_OCT : DB_DRIFT_X_DN_OCT);
    const float p_t[2] = {
        vco_oct(P[DP_VCO1_FREQ]),
        vco_oct(P[DP_VCO2_FREQ]) + x_oct + 0.12f * mo[DB_MO_RATIO]
            + sweep * (sync_on ? DB_SYNC_OCT : DB_SWEEP_FM_OCT),
    };
    const float sub_t = sub_oct(P[DP_VCO1_FREQ]);
    const float y = db_clampf(P[DP_DRIFT_Y] + 0.35f * mo[DB_MO_VCF], -1.0f, 1.0f);
    const float vcfm_t = (y < 0.0f ? -1.0f : 1.0f) * powf(fabsf(y), 1.3f) * DB_DRIFT_Y_OCT;
    /* +DRIFT Y, FM DRIVE and SYNC SWEEP also open the filter -- from the knobs
     * alone, so MOTION does not open it twice */
    const float y_open = P[DP_DRIFT_Y] > 0.0f ? powf(P[DP_DRIFT_Y], 1.2f) * DB_DRIFT_Y_OPEN_OCT : 0.0f;
    const float cut_t = cut_oct(P[DP_VCF_CUT]) + y_open
                      + DB_FM_OPEN_OCT * P[DP_FM_DRIVE] + DB_SWEEP_OPEN_OCT * P[DP_SYNC_SWEEP];
    const float k = unit(P[DP_VCF_RESO]) * DB_RESO_K;
    const float in_gain = 0.6f * (1.0f + 0.5f * k);   /* part of the ladder's bass loss back */
    const float mix_t = P[DP_VCO_MIX];
    /* BASS: the clean sine comes in first; the dirty one joins later and
     * takes some of the clean one's place, so turning BASS up thickens the
     * low end rather than only making it louder. */
    const float b = unit(P[DP_BASS]);
    const float b_late = b * sqrtf(b);
    const float clean_t = 0.34f * powf(b, 0.8f) * (1.0f - 0.3f * b_late);
    const float dirty_t = 0.24f * b_late;
    const float dirty_amt_t = P[DP_DIRTY_SUB];
    /* the dirty sub's harmonics stay low-end weight, never fizz */
    const float dlp_coef = 1.0f - expf(-2.0f * DB_PI * (350.0f + 900.0f * dirty_amt_t) / osr);
    const float hz_max = 0.2f * osr;                  /* 17.6 kHz at 88.2 kHz */
    const float oct_max = log2f(hz_max), oct_min = 0.0f;   /* 1 Hz */
    const float fc_max = fminf(18000.0f, 0.22f * osr);     /* keeps db_tan accurate */
    const float cut_max = log2f(fc_max), cut_min = CUT_LO_OCT;
    const float pi_osr = DB_PI / osr;
    const float inv_osr = 1.0f / osr;
    const float wander_inc = 0.11f * inv_osr;
    const float sm = e->sm_coef, slow = e->slow_coef;

    const float dly_t = db_delay_s(P[DP_DELAY]) * e->sr;
    const float fb_t = db_delay_fb(P[DP_DELAY]);
    const float wet_t = db_delay_wet(P[DP_DELAY]);
    /* longer repeats are darker */
    const float lp_hz = fminf(7000.0f * powf(dly_t / e->sr / 0.06f, -0.4f), 0.4f * e->sr);
    const float lp_coef = 1.0f - expf(-2.0f * DB_PI * lp_hz / e->sr);
    const float hp_r = 1.0f - 2.0f * DB_PI * 20.0f / e->sr;
    const float vol = P[DP_VOLUME];
    const int running = e->transport;

    float mcut = mo0[DB_MO_CUT];
    const float dmcut = dmo[DB_MO_CUT];

    for (int i = 0; i < frames; ++i) {
        for (int j = 0; j < DB_OS; ++j) {
            /* de-zippered controls */
            e->p_oct[0] += (p_t[0] - e->p_oct[0]) * sm;
            e->p_oct[1] += (p_t[1] - e->p_oct[1]) * sm;
            e->fm_s[0] += (fm_t[0] - e->fm_s[0]) * sm;
            e->fm_s[1] += (fm_t[1] - e->fm_s[1]) * sm;
            e->vcfm_s += (vcfm_t - e->vcfm_s) * sm;
            e->cut_oct += (cut_t - e->cut_oct) * sm;
            e->mix_s += (mix_t - e->mix_s) * sm;
            e->clean_s += (clean_t - e->clean_s) * sm;
            e->dirty_s += (dirty_t - e->dirty_s) * sm;
            e->dirty_amt_s += (dirty_amt_t - e->dirty_amt_s) * sm;
            e->sub_oct += (sub_t - e->sub_oct) * slow;
            mcut += dmcut;

            /* FM swings each VCO between DB_FM_DOWN_OCT below and DB_FM_UP_OCT
             * above its own pitch, however deep FM DRIVE goes. Unbounded, deep
             * cross FM + sync falls into two dead ends: the master crawling
             * at a few Hz (sub-audio: the drone turns to clicks) or racing at
             * the ceiling with the slave reset every sample (DC) -- both
             * measured 15 dB quieter. The time-averaged pitch stays on VCO
             * FREQ (FM by a zero-mean triangle, in octaves).
             *
             * VCO1 first, FM'd by VCO2's triangle of the previous sample;
             * then VCO2, FM'd by VCO1's triangle NOW and reset by its wrap in
             * this same sample. Sync is one-way, so this order is the
             * whole dependency. */
            /* A synced slave restarts at phase 0 -- triangle -1 -- every master
             * cycle, so its triangle leans negative; fed back as FM that bias
             * drags the master down, which shortens the slave's cycles further
             * (VCO1 fell to 14 Hz at full FM+SYNC). The bias is taken out. */
            e->tri2_dc += (e->tri[1] - e->tri2_dc) * e->dc_coef;
            float m2 = e->tri[1] - e->tri2_dc;
            float inc1 = db_exp2(db_clampf(e->p_oct[0] + e->fm_s[0] * m2, fmaxf(oct_min, e->p_oct[0] - DB_FM_DOWN_OCT),
                                            fminf(oct_max, e->p_oct[0] + DB_FM_UP_OCT))) * inv_osr;
            float o1 = vco1_render(&e->osc[0], inc1);
            float t1 = osc_tri(e->osc[0].phase);
            float inc2 = db_exp2(db_clampf(e->p_oct[1] + e->fm_s[1] * t1, fmaxf(oct_min, e->p_oct[1] - DB_FM_DOWN_OCT),
                                            fminf(oct_max, e->p_oct[1] + DB_FM_UP_OCT))) * inv_osr;
            float o2 = vco2_render(&e->osc[1], inc2, sync_on && e->osc[0].wrapped, e->osc[0].age);
            e->tri[0] = t1;
            e->tri[1] = osc_tri(e->osc[1].phase);

            /* MIX (equal power) -> VCF. DRIFT Y modulates the cutoff at audio
             * rate: + by VCO2 (a bright, tearing sweep), - by the ring
             * product VCO1 x VCO2 (clangorous, inharmonic). */
            float g2 = e->mix_s, g1 = 1.0f - g2;
            float mix = (g1 * o1 + g2 * o2) * (1.0f / sqrtf(g1 * g1 + g2 * g2 + 1e-6f)) * 0.72f;
            float src = e->vcfm_s >= 0.0f ? o2 : o1 * o2;
            float c = db_clampf(e->cut_oct + 1.2f * mcut + e->vcfm_s * src, cut_min, cut_max);
            float g = db_tan(pi_osr * db_exp2(c));
            /* a whisper of noise lets a self-oscillating VCF start on its own */
            float xin = mix * in_gain + 1e-5f * db_noise(&e->rng);
            float yv = ladder_process(&e->vcf, xin, g, k);

            /* BASS ENGINE, after the VCF so no CUTOFF can thin it.
             * CLEAN: a sine on VCO1's pitch folded into 40..80 Hz, no FM.
             * DIRTY: the SAME phase, phase-modulated by VCO1's triangle
             * (one way: it never modulates back), saturated and low passed.
             * Sharing the phase keeps its fundamental in step with the clean
             * one, so the two add instead of cancelling. */
            float sinc = db_exp2(e->sub_oct) * inv_osr;
            float sp = e->sub_phase + sinc;
            sp -= floorf(sp);
            e->sub_phase = sp;
            float wp = e->wander_ph + wander_inc;
            e->wander_ph = wp - floorf(wp);
            float clean = sin2pi(sp);
            float da = e->dirty_amt_s;
            float idx = da * (0.14f + 0.06f * sin2pi(e->wander_ph));   /* cycles: <= ~1.25 rad */
            float ds = sin2pi(sp + idx * t1);
            float drive = 1.0f + 5.0f * da;
            ds = db_tanh(drive * ds) / db_tanh(drive);
            e->dsub_lp += (ds - e->dsub_lp) * dlp_coef;
            hb_push(e, 0.8f * yv + e->clean_s * clean + e->dirty_s * e->dsub_lp);
        }
        float s = hb_out(e);

        /* CHARACTER duck, transport fade (smoothstep of a linear ramp) */
        e->duck += (duck_t - e->duck) * e->duck_coef;
        if (running) { e->fade += e->fade_in; if (e->fade > 1.0f) e->fade = 1.0f; }
        else { e->fade -= e->fade_out; if (e->fade < 0.0f) e->fade = 0.0f; }
        float f = e->fade * e->fade * (3.0f - 2.0f * e->fade);
        s *= e->duck;

        /* DELAY: one macro (time, feedback, wet together) */
        e->fb_s += (fb_t - e->fb_s) * e->fb_coef;
        e->wet_s += (wet_t - e->wet_s) * e->fb_coef;
        float d = delay_process(&e->dly, s, dly_t, e->fb_s, e->t_coef, lp_coef, hp_r);
        float m = (s + e->wet_s * d) * f;

        /* 25 Hz high pass, emergency limiter, soft clip */
        float yo = e->hp_b0 * m + e->hp_z1;
        e->hp_z1 = db_flush(e->hp_b1 * m - e->hp_a1 * yo + e->hp_z2);
        e->hp_z2 = db_flush(e->hp_b2 * m - e->hp_a2 * yo);
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
        int keep = e->transport;
        db_engine_restart(e);
        e->transport = keep;
        memset(out, 0, (size_t)frames * sizeof(float));
    }
}
