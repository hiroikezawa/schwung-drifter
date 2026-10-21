/* DRIFTER UI test: ui_core.mjs driving Schwung's REAL page controller and
 * input decoder, drawn through the device-font harness, against a fake param
 * store serving the DSP's real contract (grid_hierarchy is dumped by smoke.py
 * from the built dsp.so). Run by scripts/test.sh.
 *
 * Schwung's page engine comes from DRIFTER_SHARED (a copy of the device's
 * /data/UserData/schwung/shared -- the version that actually runs), else from
 * the Schwung checkout's src/shared; the draw harness from the checkout
 * (SCHWUNG_ROOT, else ../schwung beside this repository). */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = process.env.SCHWUNG_ROOT || path.resolve(here, "../../schwung");
const shared = process.env.DRIFTER_SHARED || path.join(root, "src/shared");
const load = (p) => import(pathToFileURL(p).href);
const { createController } = await load(path.join(shared, "param_pages/page_controller.mjs"));
const { decodeInput, applyInput } = await load(path.join(shared, "param_pages/page_input.mjs"));
const { createFramebuffer, drawContext } = await load(path.join(root, "tools/param-pages/harness.mjs"));
const { createUi, randomPatch, RANDOM_RANGES, RANDOM_KEEP, RANDOM_PAD, PAD_FIRST, PAD_LAST, LED } =
    await load(path.join(here, "../ui_core.mjs"));

let failures = 0;
function check(cond, msg) {
    if (!cond) { failures++; console.error("FAIL:", msg); }
}

const manifest = JSON.parse(fs.readFileSync(path.join(here, "../module.json"), "utf8"));
const chainParams = manifest.capabilities.chain_params;
const meta = Object.fromEntries(chainParams.map((p) => [p.key, p]));
const gridHierarchy = fs.readFileSync(path.join(here, "../build/native/grid_hierarchy.json"), "utf8");

/* ---- randomPatch: ranges, exclusions, cross-key rules ------------------------ */
const keys = chainParams.map((p) => p.key);
check(keys.every((k) => (k in RANDOM_RANGES) !== RANDOM_KEEP.includes(k)),
      "every parameter is either randomized or kept, never both or neither");
check(["volume", "gate_hold", "drift_x", "drift_y", "sub"].every((k) => RANDOM_KEEP.includes(k)),
      "VOLUME, GATE HOLD, DRIFT X/Y and SUB are never randomized");
for (let n = 0; n < 5000; n++) {
    const p = randomPatch();
    for (const [k, v] of Object.entries(p)) {
        const m = meta[k], x = Number(v);
        const r = RANDOM_RANGES[k];
        const ok = m.type === "enum" ? Number.isInteger(x) && x >= 0 && x < m.options.length && r.includes(x)
                                     : x >= r[0] - 1e-9 && x <= r[1] + 1e-9 && x >= m.min && x <= m.max;
        if (!ok) { check(false, `${k}=${v} out of range`); break; }
    }
    if (p.vco1_wave === "4" && p.vco2_wave === "4") check(false, "two noise VCOs");
    if (p.vco1_mode === "1" && p.vco2_mode === "1") check(false, "two synced VCOs");
    if (Number(p.vcf_mod) > 0.15 && p.vcf_vco1 === "0" && p.vcf_vco2 === "0" && p.vcf_lfo === "0")
        check(false, "VCF MOD with no source");
    if (failures) break;
}
/* the extremes of the ranges are reachable, nothing silent is */
const lo = randomPatch(() => 0), hi = randomPatch(() => 0.999999);
check(Number(lo.vcf_cut) >= 0.45 && Number(hi.vcf_reso) <= 0.85 && Number(hi.delay_feedback) <= 0.7,
      "cutoff never closed, reso below self-oscillation, feedback below the wash");
check(Number(lo.vco1_vol) >= 0.5 && Number(lo.vco1_freq) >= 0.45, "VCO1 always audible, never sub-audio");

/* ---- fake DSP ------------------------------------------------------------------ */
const values = {};
for (const p of chainParams) values[p.key] = String(p.default);
const writes = [], reads = [];
let shown = true, padBlocked = false;
const leds = {};
const store = {
    get(key) {
        reads.push(key);
        if (key === "ui_hierarchy") return "";
        if (key === "grid_hierarchy") return gridHierarchy;
        if (key === "chain_params") return JSON.stringify(chainParams);
        if (key.endsWith(":modulated")) return "0";
        if (key.endsWith(":effective")) return values[key.slice(0, -10)] ?? null;
        if (key in values) return values[key];
        return "";
    },
    set(key, value) {
        writes.push([key, String(value)]);
        if (key in values) values[key] = String(value);
        return true;
    },
};

let clock = 1000;
const fb = createFramebuffer();
const ctx = drawContext(fb);
const ui = createUi({
    createController, decodeInput, applyInput,
    getParam: (k) => store.get(k),
    setParam: (k, v) => store.set(k, v),
    draw: ctx,
    clear: () => fb.clearScreen(),
    layout: () => "movy",
    now: () => clock,
    displayOn: () => shown,
    padBlock: (on) => { padBlocked = on; },
    sendLed: (n, col) => { leds[n] = col; return true; },
    padSnapshot: () => ({ "68": 5, "99": 7 }),
});

function ticks(n) { for (let i = 0; i < n; i++) { clock += 17; ui.tick(); } }
const bare = (k) => k.slice(k.indexOf(":") + 1);

ui.init();
ticks(40);
const c = ui.state.controller;
const MAIN = ["volume", "drift_x", "drift_y", "vco1_freq", "vco1_mod", "vco2_freq", "vco2_mod", "sub"];
check(JSON.stringify(c.page.keys.map(bare)) === JSON.stringify(MAIN), "page 1 = MAIN: " + c.page.keys);
check(c.pages.length >= 4, `MAIN / MIX/VCF / SETUP 1 / SETUP 2: ${c.pages.length} pages`);
check(!reads.includes("ui_hierarchy"), "the grid reads grid_hierarchy, never the empty ui_hierarchy");
check(fb.countLit() > 200, "page 1 draws");
console.log(fb.toBlocks ? fb.toBlocks() : fb.toAscii());
check(ui.title() === "DRIFTER", "title: " + ui.title());

/* ---- the pads: blocked while shown, only 99 lit -------------------------------- */
check(padBlocked, "pads taken while the screen is up");
check(leds[RANDOM_PAD] === LED.RANDOM, "RANDOM pad lit");
let others = true;
for (let n = PAD_FIRST; n < RANDOM_PAD; n++) if (leds[n] !== LED.OFF) others = false;
check(others, "the other 31 pads are dark");

/* ---- a knob still works ---------------------------------------------------------- */
let w0 = writes.length;
ui.onMidi([0xb0, 71, 127]);                     /* knob 1 (VOLUME) down */
ticks(10);
check(writes.slice(w0).some(([k, v]) => k === "volume" && Number(v) < 0.7), "knob 1 = VOLUME: " + writes.slice(w0));

/* ---- another pad does nothing ------------------------------------------------- */
w0 = writes.length;
ui.onMidi([0x90, 70, 100]); ui.onMidi([0x80, 70, 0]);
ticks(4);
check(writes.length === w0, "pad 70 writes nothing: " + writes.slice(w0));

/* ---- RANDOM ------------------------------------------------------------------- */
const keep = Object.fromEntries(RANDOM_KEEP.filter((k) => k !== "trigger").map((k) => [k, values[k]]));
w0 = writes.length;
ui.onMidi([0x90, RANDOM_PAD, 100]);
check(leds[RANDOM_PAD] === LED.PRESSED, "RANDOM lights while held");
const rw = writes.slice(w0);
const written = new Set(rw.map(([k]) => k));
check(Object.keys(RANDOM_RANGES).every((k) => written.has(k)), "RANDOM writes every randomized key: " + [...written]);
check(RANDOM_KEEP.every((k) => !written.has(k)), "RANDOM writes none of the kept keys: " + [...written]);
check(Object.entries(keep).every(([k, v]) => values[k] === v), "kept values unchanged");
check(ui.title() === "RANDOM", "header says RANDOM: " + ui.title());
ui.onMidi([0x90, RANDOM_PAD, 100]);             /* a repeat of a held pad is not a new press */
check(writes.length === w0 + rw.length, "held pad fires once");
ui.onMidi([0x80, RANDOM_PAD, 0]);
check(leds[RANDOM_PAD] === LED.RANDOM, "RANDOM back to its colour on release");
ticks(10);
check(c.state.values && Number(c.state.values["synth:vcf_cut"] ?? c.state.values.vcf_cut ?? values.vcf_cut) === Number(values.vcf_cut),
      "the grid shows the new value");
clock += 2000; ticks(2);
check(ui.title() === "DRIFTER", "notice clears");

/* a second press gives a different patch */
const before = JSON.stringify(values);
ui.onMidi([0x90, RANDOM_PAD, 100]); ui.onMidi([0x80, RANDOM_PAD, 0]);
check(JSON.stringify(values) !== before, "a second RANDOM changes the patch");

/* ---- leaving: pads handed back, LEDs restored ---------------------------------- */
shown = false;
ticks(1);
check(!padBlocked, "pads handed back when the display closes");
check(leds[68] === 5 && leds[99] === 7 && leds[80] === 0, "Move's pad colours restored");
shown = true;
ticks(4);
check(padBlocked, "taken again when shown");

/* ---- Back on the first page leaves ---------------------------------------------- */
check(ui.handleBack() === false, "Back on MAIN leaves the module");

if (failures) { console.error(`${failures} UI failure(s)`); process.exit(1); }
console.log("PASS: ui (DRIFTER, real page engine)");
