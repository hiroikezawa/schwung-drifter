/* Drifter RANDOM test: RENDERS the patches ui_core.mjs's randomPatch() makes
 * and measures what comes out -- a range check on the knobs alone cannot see
 * a patch that is silent, all fizz, all DC or ten times louder than the last.
 *
 *   test-random <patches.txt> [per_style]
 *
 * patches.txt is written by tests/test_ui.mjs: one patch per line,
 * "<style> key=value key=value ...". Each is rendered for 3 s with the
 * transport running and MOTION as the patch set it (VOLUME at its default),
 * and the last 2.5 s are measured. */
#include "db_engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define BLOCK 128
#define LEN (1034 * BLOCK)           /* 3.0 s, whole blocks */
#define SKIP (SR / 2)

static db_engine E;
static float cap[LEN];

typedef struct {
    double rms_db, peak, low, high, dc, min_win_db;
    int nonfinite;
} meas;

static double db(double x) { return 20.0 * log10(x + 1e-12); }

static meas measure(void) {
    meas m;
    memset(&m, 0, sizeof(m));
    float buf[BLOCK];
    for (int n = 0; n < LEN; n += BLOCK) {
        db_engine_render(&E, buf, BLOCK);
        memcpy(cap + n, buf, sizeof(buf));
    }
    double tot = 0, lo = 0, hi = 0, dc = 0;
    float sl = 0, sh = 0;
    const float a = 1.0f - expf(-2.0f * (float)M_PI * 150.0f / SR);
    const float b = 1.0f - expf(-2.0f * (float)M_PI * 4000.0f / SR);
    for (int i = SKIP; i < LEN; ++i) {
        float x = cap[i];
        if (!isfinite(x)) { m.nonfinite++; continue; }
        sl += (x - sl) * a;
        sh += (x - sh) * b;
        lo += (double)sl * sl;
        hi += (double)(x - sh) * (x - sh);
        tot += (double)x * x;
        dc += x;
        if (fabsf(x) > m.peak) m.peak = fabsf(x);
    }
    const int n = LEN - SKIP;
    m.rms_db = db(sqrt(tot / n));
    m.low = lo / (tot + 1e-20);
    m.high = hi / (tot + 1e-20);
    m.dc = dc / n;
    m.min_win_db = 1e9;
    const int w = SR / 10;
    for (int s = SKIP; s + w <= LEN; s += w) {
        double r = 0;
        for (int i = 0; i < w; ++i) r += (double)cap[s + i] * cap[s + i];
        double d = db(sqrt(r / w));
        if (d < m.min_win_db) m.min_win_db = d;
    }
    return m;
}

static int param_index(const char *key) {
    for (int i = 0; i < DP_COUNT; ++i) if (!strcmp(db_params[i].key, key)) return i;
    return -1;
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s patches.txt [per_style]\n", argv[0]); return 2; }
    const int per_style = argc > 2 ? atoi(argv[2]) : 150;
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    static const char *names[3] = { "CHAOTIC", "DARK", "INDUST" };
    static double rms[3][1000];
    int count[3] = { 0, 0, 0 }, fails = 0, total = 0;
    double worst_peak = 0, worst_low = 1, worst_high = 0, worst_dc = 0, worst_win = 1e9, lo_rms = 1e9, hi_rms = -1e9;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        int style = atoi(line);
        if (style < 0 || style > 2 || count[style] >= per_style || count[style] >= 1000) continue;
        db_engine_init(&E, SR);
        db_engine_set_param(&E, DP_RANDOM_STYLE, (float)style);
        for (char *tok = strtok(strchr(line, ' '), " \n"); tok; tok = strtok(NULL, " \n")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            int k = param_index(tok);
            if (k < 0) { printf("FAIL: unknown key %s\n", tok); fails++; continue; }
            db_engine_set_param(&E, k, (float)atof(eq + 1));
        }
        db_engine_set_transport(&E, 1);
        meas m = measure();
        rms[style][count[style]++] = m.rms_db;
        total++;
        /* the brief: a drone you can use, every time */
        const char *why = NULL;
        if (m.nonfinite) why = "NaN/Inf";
        else if (m.peak > 0.98) why = "peak at the ceiling";
        else if (m.rms_db < -28.0 || m.rms_db > -9.0) why = "level out of -28..-9 dBFS";
        else if (m.low < 0.25) why = "too little low end (<150 Hz)";
        else if (m.high > 0.12) why = "too much top (>4 kHz)";
        else if (fabs(m.dc) > 0.005) why = "DC";
        else if (m.min_win_db < -40.0) why = "drops out (a 100 ms window under -40 dBFS)";
        if (why) {
            fails++;
            if (fails <= 10)
                printf("FAIL %s #%d: %s (rms %.1f dB, peak %.2f, low %.2f, high %.3f, dc %.4f, min %.1f dB)\n  %s",
                       names[style], count[style], why, m.rms_db, m.peak, m.low, m.high, m.dc, m.min_win_db, line);
        }
        if (m.peak > worst_peak) worst_peak = m.peak;
        if (m.low < worst_low) worst_low = m.low;
        if (m.high > worst_high) worst_high = m.high;
        if (fabs(m.dc) > worst_dc) worst_dc = fabs(m.dc);
        if (m.min_win_db < worst_win) worst_win = m.min_win_db;
        if (m.rms_db < lo_rms) lo_rms = m.rms_db;
        if (m.rms_db > hi_rms) hi_rms = m.rms_db;
    }
    fclose(f);
    for (int s = 0; s < 3; ++s) {
        if (!count[s]) { printf("FAIL: no %s patches\n", names[s]); fails++; continue; }
        qsort(rms[s], count[s], sizeof(double), cmp);
        double p10 = rms[s][count[s] / 10], p50 = rms[s][count[s] / 2], p90 = rms[s][count[s] * 9 / 10];
        printf("  %-8s %3d patches: rms p10 %.1f / median %.1f / p90 %.1f dBFS\n", names[s], count[s], p10, p50, p90);
        if (p90 - p10 > 10.0) { printf("FAIL: %s level spread %.1f dB (p10..p90)\n", names[s], p90 - p10); fails++; }
    }
    printf("  worst: peak %.2f, low %.2f, high %.3f, |dc| %.4f, quietest 100 ms %.1f dBFS, rms %.1f .. %.1f\n",
           worst_peak, worst_low, worst_high, worst_dc, worst_win, lo_rms, hi_rms);
    printf("%s: random (%d rendered patches)\n", fails ? "FAILED" : "PASS", total);
    return fails ? 1 : 0;
}
