/*
 * Drifter - Signal Chain UI core.
 *
 * Schwung's stock knob grid runs no module code, so it cannot take the pads.
 * The DSP therefore serves `ui_hierarchy` empty (the shadow UI then opens
 * ui_chain.js) and `grid_hierarchy` with the real pages (MAIN, SETUP), and
 * this file drives Schwung's OWN page engine (page_controller.mjs) with it:
 * the same grid, the same pages, plus the pads.
 *
 * THE PADS are taken (host_pad_block) while this screen is up -- the shim
 * blocks all 32 or none, and it hands them back to Move when the display
 * closes. Only one does anything:
 *   top-right (99)   RANDOM: a new drone in the RANDOM STYLE chosen on SETUP.
 * The other 31 are dark and do nothing.
 *
 * The random values are written through setParam -- the same path a knob
 * turn takes -- so the host's view of every parameter, the slot's saved
 * state and its automation all agree with what is heard. (With Move's Record
 * on, those writes are recorded like any knob turn.)
 *
 * Every dependency is injected (ui_chain.js binds the device, the tests bind
 * fakes), so nothing here touches a global.
 */

export const PAD_FIRST = 68, PAD_LAST = 99;
export const RANDOM_PAD = 99;
/* Move pad palette indices. */
export const LED = { RANDOM: 21, PRESSED: 120, OFF: 0 };
const LEDS_PER_TICK = 8;                 /* 32 pads repainted every 4 frames */
const NOTICE_MS = 1200;

export const STYLES = ["CHAOTIC", "DARK", "INDUST"];

/*
 * What RANDOM may set, per RANDOM STYLE: [lo, hi] in each knob's 0..1 units.
 * All three share the brief -- a heavy low end under complex overtones --
 * and differ in emphasis: DARK is lower, slower and wetter, INDUSTRIAL harder
 * (more FM and sync, more grit, drier), CHAOTIC the mixture with the most
 * MOTION. The ranges were set against tests/test_random.c, which RENDERS
 * thousands of these patches and measures level, low end, top end, DC and
 * silence; the cross-key rules are in randomPatch().
 *
 * VCO1 FREQ stays in 31..92 Hz (a bass drone); VCO2 is set as an INTERVAL
 * above it, not a free frequency, so the two always relate.
 */
export const STYLE_RANGES = {
    CHAOTIC: {
        vco1_freq: [0.08, 0.43], fm_drive: [0.20, 0.70], sync_sweep: [0.10, 0.70],
        vcf_cut: [0.38, 0.78], vcf_reso: [0.10, 0.72], motion: [0.45, 0.85],
        vco_mix: [0.25, 0.75], bass: [0.45, 0.80], dirty_sub: [0.20, 0.70], delay: [0.10, 0.60],
    },
    DARK: {
        vco1_freq: [0.05, 0.32], fm_drive: [0.05, 0.42], sync_sweep: [0.00, 0.38],
        vcf_cut: [0.28, 0.56], vcf_reso: [0.20, 0.68], motion: [0.20, 0.55],
        vco_mix: [0.20, 0.65], bass: [0.60, 0.90], dirty_sub: [0.30, 0.80], delay: [0.35, 0.85],
    },
    INDUST: {
        vco1_freq: [0.15, 0.43], fm_drive: [0.40, 0.85], sync_sweep: [0.30, 0.90],
        vcf_cut: [0.45, 0.82], vcf_reso: [0.15, 0.75], motion: [0.25, 0.65],
        vco_mix: [0.35, 0.85], bass: [0.40, 0.70], dirty_sub: [0.40, 0.90], delay: [0.00, 0.40],
    },
};
/* VCO2 above VCO1, in octaves: unison, fourth, fifth, octave, octave + fifth,
 * two octaves -- each with a few cents of detune so the pair beats. */
const INTERVALS = [0, 5 / 12, 7 / 12, 1, 1 + 7 / 12, 2];
const VCO_OCT = 4;                                /* VCO FREQ spans 4 octaves */
/* What RANDOM writes besides the ranges: the joystick goes back to the
 * centre, so the new drone starts where DRIFT can move it either way. */
export const RANDOM_CENTRE = ["drift_x", "drift_y"];
/* Never touched by RANDOM: the level, the mode and the style itself. */
export const RANDOM_KEEP = ["volume", "character", "random_style"];

const r3 = (x) => Math.round(x * 1000) / 1000;
const clamp01 = (x) => Math.min(1, Math.max(0, x));

/* A new patch: { key: "value" } as the wire strings setParam takes.
 * `rand` is Math.random-shaped (injected for tests). */
export function randomPatch(rand = Math.random, style = 0) {
    const name = STYLES[style] || STYLES[0];
    const R = STYLE_RANGES[name];
    const pick = ([lo, hi]) => lo + (hi - lo) * rand();
    const v = {};
    for (const k of Object.keys(R)) v[k] = pick(R[k]);

    /* VCO2: an interval above VCO1 (INDUSTRIAL may also go inharmonic) */
    let iv = INTERVALS[Math.min(INTERVALS.length - 1, Math.floor(rand() * INTERVALS.length))];
    if (name === "INDUST" && rand() < 0.4) iv = 0.15 + 1.6 * rand();
    iv += (rand() - 0.5) * 0.04;                       /* +-4 cents x ~10 */
    v.vco2_freq = v.vco1_freq + iv / VCO_OCT;
    if (v.vco2_freq > 1) v.vco2_freq -= 1 / VCO_OCT;

    /* FM and SYNC both at the top tear the drone apart; MOTION adds up to
     * +-0.3 FM and +-0.25 SWEEP of its own, so a lively patch starts lower. */
    if (v.fm_drive + v.sync_sweep > 1.3) v.sync_sweep = 1.3 - v.fm_drive;
    if (v.motion > 0.7) v.fm_drive *= 0.85;
    /* Resonance: the more FM, the less ringing on top of it; a low cutoff
     * self-oscillating is a sine rumble that fights the sub. */
    v.vcf_reso = Math.min(v.vcf_reso, 0.85 - 0.3 * v.fm_drive);
    if (v.vcf_cut < 0.40) v.vcf_reso = Math.min(v.vcf_reso, 0.55);
    /* The cutoff never sits below VCO1's own register: a closed filter
     * would leave the subs alone and the drone hollow. */
    v.vcf_cut = Math.max(v.vcf_cut, v.vco1_freq * 0.5 + 0.18);

    const wire = {};
    for (const [k, x] of Object.entries(v)) wire[k] = String(r3(clamp01(x)));
    for (const k of RANDOM_CENTRE) wire[k] = "0";
    return wire;
}

export function createUi(deps) {
    const {
        createController, decodeInput, applyInput,
        getParam, setParam, announce = () => {},
        layout = () => undefined, draw, clear,
        trailingMenus = null, runAction = null, now = () => Date.now(),
        shiftHeld = () => false,
        displayOn = () => true, padBlock = () => {},
        sendLed = () => {}, padSnapshot = () => null,
        rand = Math.random,
    } = deps;

    const st = {
        controller: null,
        shift: false,
        visible: false,
        heldPads: new Set(),
        ledCursor: 0,
        notice: "", noticeUntil: 0,
    };

    const bare = (fullKey) => {
        const i = fullKey.indexOf(":");
        return i < 0 ? fullKey : fullKey.slice(i + 1);
    };

    /* The controller asks for `synth:<key>`; the host's per-slot shims add
     * the prefix themselves. Only the hierarchy is redirected. */
    function ioGet(fullKey) {
        const k = bare(fullKey);
        if (k === "ui_hierarchy") return getParam("grid_hierarchy");
        return getParam(k);
    }
    const ioSet = (fullKey, value) => setParam(bare(fullKey), value);

    function showNotice(text) {
        st.notice = text;
        st.noticeUntil = now() + NOTICE_MS;
    }

    function init() {
        st.controller = createController({
            getParam: ioGet,
            setParam: ioSet,
            announce,
            trailingMenus: trailingMenus ? () => trailingMenus() || [] : undefined,
            now,
        });
        st.controller.load({ slot: 0, component: "synth", prefix: "synth" });
    }

    const title = () => (now() < st.noticeUntil ? st.notice : "Drifter");

    function padColor(note) {
        if (note !== RANDOM_PAD) return LED.OFF;
        return st.heldPads.has(note) ? LED.PRESSED : LED.RANDOM;
    }

    function paintPads() {
        for (let i = 0; i < LEDS_PER_TICK; ++i) {
            const note = PAD_FIRST + (st.ledCursor++ % 32);
            sendLed(note, padColor(note));
        }
    }

    function restorePads() {
        const snap = padSnapshot();
        for (let n = PAD_FIRST; n <= PAD_LAST; ++n) sendLed(n, (snap && snap[String(n)]) || 0);
    }

    /* Every write goes out; a refused one is counted and said, never hidden. */
    function randomize() {
        const style = Math.round(Number(getParam("random_style")));
        const patch = randomPatch(rand, Number.isFinite(style) ? style : 0);
        let failed = 0;
        for (const [key, value] of Object.entries(patch))
            if (setParam(key, value) === false) failed++;
        if (st.controller && typeof st.controller.revalue === "function") st.controller.revalue();
        showNotice(failed ? `RANDOM ${failed} FAILED` : `RANDOM ${STYLES[style] || STYLES[0]}`);
        announce(failed ? `Random, ${failed} failed` : "Random");
        return failed;
    }

    function tick() {
        const c = st.controller;
        if (!c) return;
        if (!displayOn()) { if (st.visible) releaseAll(); return; }
        st.visible = true;
        padBlock(true);                   /* restated every frame: the shim drops it on its own */
        st.shift = !!shiftHeld();
        const l = layout();
        if (l) c.setLayout(l);
        c.tick();
        clear();
        c.render(draw, { title: title() });
        c.renderOverlays(draw, { clearScreen: clear });
        paintPads();
    }

    function onPad(note, down) {
        if (!down) { st.heldPads.delete(note); sendLed(note, padColor(note)); return; }
        if (st.heldPads.has(note)) return;
        st.heldPads.add(note);
        if (note === RANDOM_PAD) randomize();
        sendLed(note, padColor(note));
    }

    function run(todo) {
        if (todo && todo.action === "menu" && todo.entry && todo.entry.action && runAction)
            return !!runAction(todo.entry.action);
        return false;
    }

    function onMidi(data) {
        const c = st.controller;
        if (!c || !data || data.length < 3 || !displayOn()) return;
        const status = data[0] & 0xf0;
        if ((status === 0x90 || status === 0x80) && data[1] >= PAD_FIRST && data[1] <= PAD_LAST) {
            onPad(data[1], status === 0x90 && data[2] > 0);
            return;
        }
        st.shift = !!shiftHeld();
        const intent = decodeInput(data, { shift: st.shift });
        if (!intent) return;
        run(applyInput(c, intent, { nowMs: now(), reveal: st.shift }));
    }

    /* Back peels the grid's layers; the last one leaves. */
    function handleBack() {
        const c = st.controller;
        if (!c) return false;
        const todo = applyInput(c, { type: "back" }, { nowMs: now() });
        return !(todo && todo.action === "exit");
    }

    function releaseAll() {
        st.heldPads.clear();
        restorePads();
        padBlock(false);
        st.visible = false;
    }

    return { init, tick, onMidi, handleBack, releaseAll, randomize, title, padColor, state: st };
}
