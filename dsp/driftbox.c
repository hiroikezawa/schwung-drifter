/* Driftbox SIII - Schwung plugin API v2 adapter. MIT.
 *
 * Inspired by the REON driftbox SIII. Not affiliated with or endorsed by REON.
 *
 * EVERY entry point here runs on the SPI audio callback (plugin_api_v1.h), so:
 * no allocation (instances come from a static pool), no file I/O, no logging,
 * bounded work.
 *
 * MIDI: monophonic, last-note priority (a stack of held notes, newest on top):
 * releasing the newest key falls back to the newest one still held, and the
 * gate closes only when none is. No ADSR -- the gate drives the VCA directly. */
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
 * renders it only every ~0.5 s. A MIDI note to the slot wakes it at once
 * (shadow_midi.c), but a TRIGGER or GATE HOLD from the knob grid is a
 * set_param, which does not -- so for DB_AWAKE_S after a parameter write (and
 * always while GATE HOLD is Play, waiting for the transport) a
 * block that would be silent carries a 6 LSB DC offset (-75 dBFS, inaudible)
 * and the slot stays awake. After that it parks again and costs nothing.
 * VOLUME 0 is always exact digital silence. */
#define DB_KEEPALIVE_LSB 6
#define DB_SILENCE_LSB 4
#define DB_AWAKE_S 30

typedef struct {
    atomic_int used;
    db_engine engine;
    uint8_t held[16];       /* held notes, most recent last */
    int nheld;
    int32_t awake_left;     /* host samples of keepalive remaining */
    /* MIDI clock fallback, used only when the host has no get_beat_position */
    int clk_running;
    float buf[DB_MAX_FRAMES];
} db_instance;

static db_instance g_pool[DB_INSTANCES];
static float g_sr = (float)MOVE_SAMPLE_RATE;
static const host_api_v1_t *g_host;

/* Stock knob grid: MAIN (volume and the DRIFT joystick first), then VCF/DLY,
 * VCO and MOD/LFO pages. Served from get_param -- a sound generator's
 * module.json hierarchy is never read. */
static const char db_hierarchy[] =
    "{\"modes\":null,\"levels\":{"
      "\"root\":{\"label\":\"Driftbox SIII\","
        "\"knobs\":[\"volume\",\"drift_x\",\"drift_y\",\"vco1_freq\",\"vco1_mod\",\"vco2_freq\",\"vco2_mod\",\"vcf_cut\"],"
        "\"params\":[\"volume\",\"drift_x\",\"drift_y\",\"vco1_freq\",\"vco1_mod\",\"vco2_freq\",\"vco2_mod\",\"vcf_cut\","
          "{\"level\":\"vcfdly\",\"label\":\"VCF/DLY\"},{\"level\":\"vco\",\"label\":\"VCO\"},{\"level\":\"mod\",\"label\":\"MOD/LFO\"}]},"
      "\"vcfdly\":{\"label\":\"VCF/DLY\","
        "\"knobs\":[\"vcf_reso\",\"delay_time\",\"delay_feedback\",\"drift_x_target\",\"drift_y_target\",\"trigger\"],"
        "\"params\":[\"vcf_reso\",\"delay_time\",\"delay_feedback\",\"drift_x_target\",\"drift_y_target\",\"trigger\"]},"
      "\"vco\":{\"label\":\"VCO\","
        "\"knobs\":[\"vco1_wave\",\"vco1_mode\",\"vco1_lfo\",\"vco1_vol\",\"vco2_wave\",\"vco2_mode\",\"vco2_lfo\",\"vco2_vol\"],"
        "\"params\":[\"vco1_wave\",\"vco1_mode\",\"vco1_lfo\",\"vco1_vol\",\"vco2_wave\",\"vco2_mode\",\"vco2_lfo\",\"vco2_vol\"]},"
      "\"mod\":{\"label\":\"MOD/LFO\","
        "\"knobs\":[\"vcf_mod\",\"vcf_vco1\",\"vcf_vco2\",\"vcf_lfo\",\"lfo_rate\",\"lfo_wave\",\"gate_hold\"],"
        "\"params\":[\"vcf_mod\",\"vcf_vco1\",\"vcf_vco2\",\"vcf_lfo\",\"lfo_rate\",\"lfo_wave\",\"gate_hold\"]}"
    "}}";

/* ---- lifecycle ---------------------------------------------------------- */
static void *create_instance(const char *module_dir, const char *defaults) {
    (void)module_dir; (void)defaults;
    for (int i = 0; i < DB_INSTANCES; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_pool[i].used, &expected, 1)) {
            db_instance *inst = &g_pool[i];
            db_engine_init(&inst->engine, g_sr);
            inst->nheld = 0;
            inst->awake_left = 0;
            inst->clk_running = 0;
            return inst;
        }
    }
    return NULL;
}

static void destroy_instance(void *ptr) {
    if (ptr) atomic_store(&((db_instance *)ptr)->used, 0);
}

/* ---- MIDI --------------------------------------------------------------- */
static void apply_gate(db_instance *inst) {
    if (inst->nheld > 0) db_engine_set_gate(&inst->engine, 1, inst->held[inst->nheld - 1]);
    else db_engine_set_gate(&inst->engine, 0, -1);   /* keeps the last pitch */
}

static int remove_note(db_instance *inst, uint8_t n) {
    for (int i = 0; i < inst->nheld; ++i)
        if (inst->held[i] == n) {
            memmove(&inst->held[i], &inst->held[i + 1], (size_t)(inst->nheld - i - 1));
            inst->nheld--;
            return 1;
        }
    return 0;
}

static void note_on(db_instance *inst, uint8_t n) {
    remove_note(inst, n);
    if (inst->nheld == (int)sizeof(inst->held)) {
        memmove(&inst->held[0], &inst->held[1], sizeof(inst->held) - 1);
        inst->nheld--;
    }
    inst->held[inst->nheld++] = n;
    apply_gate(inst);
}

static void note_off(db_instance *inst, uint8_t n) {
    if (remove_note(inst, n)) apply_gate(inst);
}

static void on_midi(void *ptr, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!ptr || !msg || len < 1) return;
    db_instance *inst = ptr;
    uint8_t st = msg[0] & 0xF0;
    if (st == 0x90 && len >= 3) {
        if (msg[2]) note_on(inst, msg[1] & 0x7F);
        else note_off(inst, msg[1] & 0x7F);
    } else if (st == 0x80 && len >= 2) {
        note_off(inst, msg[1] & 0x7F);
    } else if (st == 0xB0 && len >= 3 && (msg[1] == 123 || msg[1] == 120)) {
        inst->nheld = 0;                      /* All Notes / Sound Off */
        apply_gate(inst);
    } else if (msg[0] == 0xFA || msg[0] == 0xFB || msg[0] == 0xF8) {
        inst->clk_running = 1;                /* Start / Continue / Clock */
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

/* ---- state blob: {"v":1,"p":[every parameter but TRIGGER, in order]} ------
 * Applied only if every value is valid and in range. */
#define DB_STATE_MAX 2048
#define DB_STATE_COUNT (DP_COUNT - 1)

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

static int restore_state(db_engine *e, const char *text) {
    if (!text || strlen(text) > DB_STATE_MAX) return 0;
    const char *s = skip_ws(text);
    if (*s != '{') return 0;
    float v, prm[DP_COUNT];
    const char *p = find_key(s, "v");
    if (!p || !read_number(p, &v) || v != 1.0f) return 0;
    p = find_key(s, "p");
    if (!p || *p != '[') return 0;
    p = skip_ws(p + 1);
    int n = 0;
    for (int i = 0; i < DP_COUNT; ++i) {
        if (i == DP_TRIGGER) continue;
        p = read_number(p, &prm[i]);
        if (!p) return 0;
        const db_param_info *pi = &db_params[i];
        if (prm[i] < pi->min || prm[i] > pi->max) return 0;
        if (pi->is_int && prm[i] != floorf(prm[i])) return 0;
        p = skip_ws(p);
        if (++n < DB_STATE_COUNT) {
            if (*p != ',') return 0;
            p = skip_ws(p + 1);
        }
    }
    if (*p != ']') return 0;
    for (int i = 0; i < DP_COUNT; ++i)
        if (i != DP_TRIGGER) db_engine_set_param(e, i, prm[i]);
    return 1;
}

static int write_state(const db_engine *e, char *buf, int size) {
    int n = snprintf(buf, size, "{\"v\":1,\"p\":[");
    int first = 1;
    for (int i = 0; i < DP_COUNT && n > 0 && n < size; ++i) {
        if (i == DP_TRIGGER) continue;
        n += snprintf(buf + n, size - n, db_params[i].is_int ? "%s%.0f" : "%s%.4f",
                      first ? "" : ",", e->param[i]);
        first = 0;
    }
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
    inst->awake_left = (int32_t)(DB_AWAKE_S * g_sr);
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
    else if (!strcmp(key, "ui_hierarchy")) return copy_text(buf, size, db_hierarchy);
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
 * clock is the fallback for a host without it. */
static int transport_running(const db_instance *inst) {
    if (g_host && g_host->get_beat_position) return g_host->get_beat_position() >= 0.0;
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
    db_engine_set_transport(&inst->engine, transport_running(inst));
    db_engine_render(&inst->engine, inst->buf, frames);
    int peak = 0;
    for (int i = 0; i < frames; ++i) {
        int16_t a = to_s16(inst->buf[i]);          /* mono, like the hardware */
        out[i * 2] = a;
        out[i * 2 + 1] = a;
        int m = abs(a);
        if (m > peak) peak = m;
    }
    /* GATE HOLD Play must start on the downbeat, so it never lets the slot
     * park while waiting for Play (a parked slot is rendered every ~0.5 s). */
    int awake = inst->awake_left > 0 || (int)inst->engine.param[DP_GATE_HOLD] == DB_HOLD_PLAY;
    if (inst->awake_left > 0) inst->awake_left -= frames;
    if (awake && peak <= DB_SILENCE_LSB && inst->engine.param[DP_VOLUME] > 0.0f)
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
