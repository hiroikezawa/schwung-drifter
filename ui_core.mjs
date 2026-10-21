/*
 * DRIFTER - Signal Chain UI core.
 *
 * Schwung's stock knob grid runs no module code, so it cannot take the pads.
 * The DSP therefore serves `ui_hierarchy` empty (the shadow UI then opens
 * ui_chain.js) and `grid_hierarchy` with the real pages, and this file drives
 * Schwung's OWN page engine (page_controller.mjs) with it: the same grid,
 * the same pages, plus the pads.
 *
 * THE PADS are taken (host_pad_block) while this screen is up -- the shim
 * blocks all 32 or none, and it hands them back to Move when the display
 * closes. Only one does anything:
 *   top-right (99)   RANDOM: every knob gets a new random value, except
 *                    VOLUME, GATE HOLD, DRIFT X/Y and their TARGETs, SUB and
 *                    TRIGGER. The ranges keep the patch audible (see
 *                    RANDOM_RANGES).
 * The other 31 are dark and do nothing; closing the screen gives them back to
 * Move as note pads.
 *
 * The random values are written through setParam -- the same path a knob
 * turn takes -- so the host's view of every parameter, the slot's saved
 * state and its automation all agree with what is heard. (With Move's Record
 * on, those writes are recorded like any knob turn.)
 *
 * Automation is shown by Schwung's own reads: `<key>:effective` and
 * `<key>:modulated` are passed through to the host untouched.
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

/*
 * What RANDOM may set, and to what. Floats: [lo, hi] in the knob's own 0..1
 * (or -1..1) units. Enums: the option indices it may pick.
 *
 * "Always audible" is the brief, so the ends that go silent or painful are
 * left out: VCO FREQ stays 40 Hz..1.2 kHz at C4 (no sub-audio LFO range),
 * VCO1 is never mixed out, the VCF never closes below ~380 Hz, RESO stays
 * under self-oscillation, FM / VCF MOD stop before pure noise, FEEDBACK stops
 * before the self-sustaining wash. Constraints across keys are in
 * randomPatch().
 */
export const RANDOM_RANGES = {
    vco1_freq: [0.45, 0.77],
    vco1_mod: [0.0, 0.6],
    vco2_freq: [0.40, 0.80],
    vco2_mod: [0.0, 0.6],
    vco1_vol: [0.5, 1.0],
    vco2_vol: [0.0, 1.0],
    vcf_cut: [0.45, 0.95],
    vcf_reso: [0.0, 0.85],
    vcf_mod: [0.0, 0.6],
    lfo_rate: [0.1, 0.75],
    delay_time: [0.0, 1.0],
    delay_feedback: [0.0, 0.7],
    vco1_wave: [0, 1, 2, 3, 4],
    vco2_wave: [0, 1, 2, 3, 4],
    vco1_mode: [0, 1],
    vco2_mode: [0, 1],
    vco1_lfo: [0, 1],
    vco2_lfo: [0, 1],
    vcf_vco1: [0, 1],
    vcf_vco2: [0, 1],
    vcf_lfo: [0, 1],
    lfo_wave: [0, 1],
};
const ENUM_KEYS = new Set(["vco1_wave", "vco2_wave", "vco1_mode", "vco2_mode", "vco1_lfo",
                           "vco2_lfo", "vcf_vco1", "vcf_vco2", "vcf_lfo", "lfo_wave"]);
/* Never touched by RANDOM: the level, how it is gated, the joystick and what
 * it drives, the bass and the momentary trigger. */
export const RANDOM_KEEP = ["volume", "gate_hold", "drift_x", "drift_y", "drift_x_target",
                            "drift_y_target", "sub", "trigger"];

const WAVE_NOISE = 4, MODE_SYNC = 1;

/* A new patch: { key: "value" } for every key in RANDOM_RANGES, as the wire
 * strings setParam takes. `rand` is Math.random-shaped (injected for tests). */
export function randomPatch(rand = Math.random) {
    const out = {};
    for (const [key, range] of Object.entries(RANDOM_RANGES)) {
        if (ENUM_KEYS.has(key)) {
            out[key] = range[Math.min(range.length - 1, Math.floor(rand() * range.length))];
        } else {
            const [lo, hi] = range;
            out[key] = Math.round((lo + (hi - lo) * rand()) * 1000) / 1000;
        }
    }
    /* Two noise VCOs have no pitch left at all; two synced VCOs reset each
     * other into a buzz -- one of each is plenty. */
    if (out.vco1_wave === WAVE_NOISE && out.vco2_wave === WAVE_NOISE) out.vco2_wave = 0;
    if (out.vco1_mode === MODE_SYNC && out.vco2_mode === MODE_SYNC) out.vco1_mode = 0;
    /* A VCF MOD depth with no source switched on does nothing; give it one. */
    if (out.vcf_mod > 0.15 && !out.vcf_vco1 && !out.vcf_vco2 && !out.vcf_lfo) out.vcf_lfo = 1;
    const wire = {};
    for (const [k, v] of Object.entries(out)) wire[k] = String(v);
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

    const title = () => (now() < st.noticeUntil ? st.notice : "DRIFTER");

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
        const patch = randomPatch(rand);
        let failed = 0;
        for (const [key, value] of Object.entries(patch))
            if (setParam(key, value) === false) failed++;
        if (st.controller && typeof st.controller.revalue === "function") st.controller.revalue();
        showNotice(failed ? `RANDOM ${failed} FAILED` : "RANDOM");
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
