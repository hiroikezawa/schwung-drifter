/* Drifter - Schwung plugin API v2 adapter. MIT.
 *
 * Inspired by the REON driftbox SIII. Not affiliated with or endorsed by REON.
 *
 * EVERY entry point here runs on the SPI audio callback (plugin_api_v1.h), so:
 * no allocation (instances come from a static pool), no file I/O, no logging,
 * bounded work.
 *
 * A drone, not a keyboard: it sounds while Move's transport runs and fades
 * out when it stops. MIDI notes, velocity and pitch bend are ignored; MIDI is
 * read only for the clock, as the transport fallback of a host that has no
 * get_beat_position. */
#include "host/plugin_api_v1.h"
#include "db_engine.h"
#include "db_metadata.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "instance pool needs lock-free atomics");
_Static_assert(MOVE_FRAMES_PER_BLOCK <= DB_MAX_FRAMES, "block larger than the engine's");
_Static_assert(sizeof(db_param_options) / sizeof(db_param_options[0]) == DP_COUNT,
               "module.json and db_params disagree on the parameter count");

#define EXPORT __attribute__((visibility("default")))

/* Four chain slots can each hold one; spares cover a swap in flight. */
#define DB_INSTANCES 6

/* The shim parks a slot whose output stays below 5 LSB for ~1 s and then
 * renders it only every ~0.5 s -- which would start the drone up to half a
 * second after Play. So while stopped, a block that would be silent carries a
 * 6 LSB DC offset (-75 dBFS, inaudible) and the slot stays awake. The engine
 * itself computes nothing while stopped, so this costs almost no CPU.
 * VOLUME 0 is always exact digital silence. */
#define DB_KEEPALIVE_LSB 6
#define DB_SILENCE_LSB 4

/* MIDI clock fallback: running from Start/Continue/Clock until Stop, or until
 * no clock has arrived for this long (a lost Stop must not drone forever). */
#define DB_CLOCK_TIMEOUT_S 0.5f

typedef struct {
    atomic_int used;
    db_engine engine;
    int clk_running;
    int32_t clk_silent;     /* host samples since the last clock byte */
    float buf[DB_MAX_FRAMES];
} db_instance;

static db_instance g_pool[DB_INSTANCES];
static float g_sr = (float)MOVE_SAMPLE_RATE;
static const host_api_v1_t *g_host;

/* Stock knob grid: MAIN is what is played live, SETUP what is set once.
 * Served as grid_hierarchy (see get_param). */
static const char db_hierarchy[] =
    "{\"modes\":null,\"levels\":{"
      "\"root\":{\"label\":\"MAIN\","
        "\"knobs\":[\"volume\",\"drift_x\",\"drift_y\",\"fm_drive\",\"sync_sweep\",\"vcf_cut\",\"vcf_reso\",\"motion\"],"
        "\"params\":[\"volume\",\"drift_x\",\"drift_y\",\"fm_drive\",\"sync_sweep\",\"vcf_cut\",\"vcf_reso\",\"motion\","
          "{\"level\":\"setup\",\"label\":\"SETUP\"}]},"
      "\"setup\":{\"label\":\"SETUP\","
        "\"knobs\":[\"vco1_freq\",\"vco2_freq\",\"vco_mix\",\"bass\",\"dirty_sub\",\"delay\",\"character\",\"random_style\"],"
        "\"params\":[\"vco1_freq\",\"vco2_freq\",\"vco_mix\",\"bass\",\"dirty_sub\",\"delay\",\"character\",\"random_style\"]}"
    "}}";

/* ---- lifecycle ---------------------------------------------------------- */
static void *create_instance(const char *module_dir, const char *defaults) {
    (void)module_dir; (void)defaults;
    for (int i = 0; i < DB_INSTANCES; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_pool[i].used, &expected, 1)) {
            db_instance *inst = &g_pool[i];
            db_engine_init(&inst->engine, g_sr);
            inst->clk_running = 0;
            inst->clk_silent = 0;
            return inst;
        }
    }
    return NULL;
}

static void destroy_instance(void *ptr) {
    if (ptr) atomic_store(&((db_instance *)ptr)->used, 0);
}

/* ---- MIDI: the clock only ------------------------------------------------ */
static void on_midi(void *ptr, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!ptr || !msg || len < 1) return;
    db_instance *inst = ptr;
    if (msg[0] == 0xFA || msg[0] == 0xFB || msg[0] == 0xF8) {
        inst->clk_running = 1;                /* Start / Continue / Clock */
        inst->clk_silent = 0;
    } else if (msg[0] == 0xFC) {
        inst->clk_running = 0;                /* Stop */
    }
}

/* ---- parameters --------------------------------------------------------- */
static int param_index(const char *key) {
    for (int i = 0; i < DP_COUNT; ++i) if (!strcmp(key, db_params[i].key)) return i;
    return -1;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

/* A whole-string number, rejecting NaN/Inf/trailing junk. */
static int parse_float(const char *s, float *out) {
    char *end;
    float v = strtof(s, &end);
    if (end == s || *skip_ws(end) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* An enum accepts its index or its option name (case-insensitive). */
static int parse_value(int idx, const char *s, float *out) {
    if (parse_float(s, out)) return 1;
    const char *const *names = db_param_options[idx];
    for (int i = 0; i < DB_MAX_OPTIONS && names[i]; ++i)
        if (!strcasecmp(s, names[i])) { *out = (float)i; return 1; }
    return 0;
}

/* ---- state blob: {"v":3,"p":[every parameter, in order]} -----------------
 * Applied only if every value is valid and in range.
 *
 * v1 / v2 (0.5.0 .. 0.7.0, the keyboard synth) had a different parameter
 * set. They load WITHOUT error, but only what still means the same thing is
 * carried over -- VOLUME, CUTOFF and RESONANCE; everything else takes this
 * version's default, DRIFT X/Y included (centred). The old blob was a list of
 * 28 (v1) or 29 (v2) values; VOLUME is its 28th, CUTOFF and RESO its 13th and
 * 14th. */
#define DB_STATE_MAX 2048
#define DB_LEGACY_MAX 29
#define DB_LEGACY_CUT 12
#define DB_LEGACY_RESO 13
#define DB_LEGACY_VOLUME 27

static const char *find_key(const char *s, const char *key) {
    size_t n = strlen(key);
    for (const char *p = s; (p = strchr(p, '"')) != NULL; ++p) {
        if (!strncmp(p + 1, key, n) && p[n + 1] == '"') {
            const char *q = skip_ws(p + n + 2);
            if (*q == ':') return skip_ws(q + 1);
        }
    }
    return NULL;
}

/* JSON number grammar only: strtof alone would also take "nan", hex, "+1". */
static const char *read_number(const char *p, float *out) {
    const char *start = p;
    if (*p == '-') ++p;
    if (*p == '0') ++p;
    else if (*p >= '1' && *p <= '9') { while (isdigit((unsigned char)*p)) ++p; }
    else return NULL;
    if (*p == '.') {
        ++p;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) ++p;
    }
    char tmp[32];
    size_t n = (size_t)(p - start);
    if (n == 0 || n >= sizeof(tmp)) return NULL;
    memcpy(tmp, start, n);
    tmp[n] = 0;
    float v = strtof(tmp, NULL);
    if (!isfinite(v)) return NULL;
    *out = v;
    return p;
}

/* "[n, n, ...]" -> up to max numbers; returns the count, or -1. */
static int read_array(const char *p, float *out, int max) {
    if (!p || *p != '[') return -1;
    p = skip_ws(p + 1);
    if (*p == ']') return 0;
    for (int n = 0; n < max; ) {
        p = read_number(p, &out[n]);
        if (!p) return -1;
        ++n;
        p = skip_ws(p);
        if (*p == ']') return n;
        if (*p != ',') return -1;
        p = skip_ws(p + 1);
    }
    return -1;                               /* longer than max */
}

static int restore_state(db_engine *e, const char *text) {
    if (!text || strlen(text) > DB_STATE_MAX) return 0;
    const char *s = skip_ws(text);
    if (*s != '{') return 0;
    float v, prm[DB_LEGACY_MAX > DP_COUNT ? DB_LEGACY_MAX : DP_COUNT];
    const char *p = find_key(s, "v");
    if (!p || !read_number(p, &v)) return 0;
    int n = read_array(find_key(s, "p"), prm, (int)(sizeof(prm) / sizeof(prm[0])));
    if (v == 1.0f || v == 2.0f) {
        if (n != (v == 1.0f ? 28 : 29)) return 0;
        const float cut = prm[DB_LEGACY_CUT], reso = prm[DB_LEGACY_RESO], vol = prm[DB_LEGACY_VOLUME];
        if (!(cut >= 0.0f && cut <= 1.0f && reso >= 0.0f && reso <= 1.0f && vol >= 0.0f && vol <= 1.0f))
            return 0;
        for (int i = 0; i < DP_COUNT; ++i) db_engine_set_param(e, i, db_params[i].def);
        db_engine_set_param(e, DP_VOLUME, vol);
        db_engine_set_param(e, DP_VCF_CUT, cut);
        db_engine_set_param(e, DP_VCF_RESO, reso);
        return 1;
    }
    if (v != 3.0f || n != DP_COUNT) return 0;
    for (int i = 0; i < DP_COUNT; ++i) {
        const db_param_info *pi = &db_params[i];
        if (prm[i] < pi->min || prm[i] > pi->max) return 0;
        if (pi->is_int && prm[i] != floorf(prm[i])) return 0;
    }
    for (int i = 0; i < DP_COUNT; ++i) db_engine_set_param(e, i, prm[i]);
    return 1;
}

static int write_state(const db_engine *e, char *buf, int size) {
    int n = snprintf(buf, size, "{\"v\":3,\"p\":[");
    for (int i = 0; i < DP_COUNT && n > 0 && n < size; ++i)
        n += snprintf(buf + n, size - n, db_params[i].is_int ? "%s%.0f" : "%s%.4f",
                      i ? "," : "", e->param[i]);
    if (n > 0 && n < size) n += snprintf(buf + n, size - n, "]}");
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

static void set_param(void *ptr, const char *key, const char *val) {
    if (!ptr || !key || !val) return;
    db_instance *inst = ptr;
    float f;
    if (!strcmp(key, "state")) { (void)restore_state(&inst->engine, val); return; }
    int idx = param_index(key);
    if (idx < 0 || !parse_value(idx, val, &f)) return;
    db_engine_set_param(&inst->engine, idx, f);
}

static int copy_text(char *buf, int size, const char *text) {
    size_t n = strlen(text);
    if (n >= (size_t)size) { buf[0] = 0; return -1; }
    memcpy(buf, text, n + 1);
    return (int)n;
}

static int get_param(void *ptr, const char *key, char *buf, int size) {
    if (!ptr || !key || !buf || size <= 0) return -1;
    buf[0] = 0;
    const db_instance *inst = ptr;
    const db_engine *e = &inst->engine;
    int idx = param_index(key);
    int n = -1;
    if (idx >= 0) {
        const db_param_info *pi = &db_params[idx];
        n = pi->is_int ? snprintf(buf, size, "%d", (int)e->param[idx])
                       : snprintf(buf, size, "%.4f", e->param[idx]);
    }
    else if (!strcmp(key, "state")) return write_state(e, buf, size);
    else if (!strcmp(key, "chain_params")) return copy_text(buf, size, db_chain_params);
    /* ui_hierarchy is served EMPTY on purpose: with a hierarchy there the
     * shadow UI draws its stock grid itself and never opens ui_chain.js, which
     * is what takes the pads (the RANDOM pad). ui_chain.js draws the same grid
     * from grid_hierarchy with Schwung's own page engine. */
    else if (!strcmp(key, "ui_hierarchy")) return 0;
    else if (!strcmp(key, "grid_hierarchy")) return copy_text(buf, size, db_hierarchy);
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

static int get_error(void *ptr, char *buf, int size) {
    (void)ptr;
    if (buf && size > 0) buf[0] = 0;
    return 0;
}

/* ---- audio -------------------------------------------------------------- */
/* Move's transport. The host's beat position is < 0 while stopped; MIDI
 * clock is the fallback for a host without it. With neither, it is stopped:
 * never a drone nobody can stop. */
static int transport_running(db_instance *inst, int frames) {
    if (g_host && g_host->get_beat_position) return g_host->get_beat_position() >= 0.0;
    if (inst->clk_running) {
        inst->clk_silent += frames;
        if (inst->clk_silent > (int32_t)(DB_CLOCK_TIMEOUT_S * g_sr)) inst->clk_running = 0;
    }
    return inst->clk_running;
}

static inline int16_t to_s16(float x) {
    if (!(x == x)) x = 0.0f;
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    return (int16_t)lrintf(x * 32767.0f);
}

static void render_block(void *ptr, int16_t *out, int frames) {
    if (!out || frames <= 0) return;
    if (!ptr || frames > DB_MAX_FRAMES) {
        memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    db_instance *inst = ptr;
    db_engine_set_transport(&inst->engine, transport_running(inst, frames));
    db_engine_render(&inst->engine, inst->buf, frames);
    int peak = 0;
    for (int i = 0; i < frames; ++i) {
        int16_t a = to_s16(inst->buf[i]);          /* mono, like the hardware */
        out[i * 2] = a;
        out[i * 2 + 1] = a;
        int m = abs(a);
        if (m > peak) peak = m;
    }
    /* never let the slot park: Play must start the drone on the downbeat */
    if (peak <= DB_SILENCE_LSB && inst->engine.param[DP_VOLUME] > 0.0f)
        for (int i = 0; i < frames * 2; ++i) out[i] = DB_KEEPALIVE_LSB;
}

static plugin_api_v2_t g_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .get_error = get_error,
    .render_block = render_block,
};

EXPORT plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    /* The engine is rate-agnostic; the host has always been 44.1 kHz. */
    if (host && host->sample_rate) {
        if (host->sample_rate < 22050 || host->sample_rate > 96000) return NULL;
        g_sr = (float)host->sample_rate;
    }
    g_host = host;
    return &g_api;
}
