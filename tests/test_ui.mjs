/* Drifter UI test: ui_core.mjs driving Schwung's REAL page controller and
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
const { createUi, randomPatch, STYLES, STYLE_RANGES, RANDOM_CENTRE, RANDOM_KEEP, RANDOM_PAD,
        PAD_FIRST, PAD_LAST, LED } = await load(path.join(here, "../ui_core.mjs"));

let failures = 0;
function check(cond, msg) {
    if (!cond) { failures++; console.error("FAIL:", msg); }
}

const manifest = JSON.parse(fs.readFileSync(path.join(here, "../module.json"), "utf8"));
const chainParams = manifest.capabilities.chain_params;
const meta = Object.fromEntries(chainParams.map((p) => [p.key, p]));
const outDir = path.join(here, "../build/native");
const gridHierarchy = fs.readFileSync(path.join(outDir, "grid_hierarchy.json"), "utf8");

/* ---- randomPatch: ranges, exclusions, cross-key rules ------------------------ */
const keys = chainParams.map((p) => p.key);
const randomized = new Set([...Object.keys(STYLE_RANGES.CHAOTIC), "vco2_freq", ...RANDOM_CENTRE]);
check(keys.every((k) => randomized.has(k) !== RANDOM_KEEP.includes(k)),
      "every parameter is either written by RANDOM or kept, never both or neither");
check(STYLES.length === meta.random_style.options.length, "one range table per RANDOM STYLE option");
for (const st of STYLES)
    check(JSON.stringify(Object.keys(STYLE_RANGES[st]).sort()) === JSON.stringify(Object.keys(STYLE_RANGES.CHAOTIC).sort()),
          `style ${st} covers the same keys`);
/* tests/test_random.c renders these: 400 per style, one per line */
const dump = [];
for (let style = 0; style < STYLES.length; style++) {
    for (let n = 0; n < 3000; n++) {
        const p = randomPatch(Math.random, style);
        for (const [k, v] of Object.entries(p)) {
            const m = meta[k], x = Number(v);
            if (!(x >= m.min && x <= m.max)) { check(false, `${STYLES[style]} ${k}=${v} out of range`); break; }
        }
        for (const k of RANDOM_CENTRE) if (p[k] !== "0") check(false, `${k} not centred`);
        for (const k of RANDOM_KEEP) if (k in p) check(false, `${k} written`);
        const iv = (Number(p.vco2_freq) - Number(p.vco1_freq)) * 4;
        if (iv < -1.05 || iv > 2.05) check(false, `VCO2 ${iv} oct from VCO1`);
        if (Number(p.fm_drive) + Number(p.sync_sweep) > 1.3005) check(false, "FM + SYNC both at the top");
        if (Number(p.vcf_reso) > 0.8505) check(false, "resonance near self-oscillation");
        if (Number(p.vcf_cut) < 0.25) check(false, "cutoff closed");
        if (Number(p.bass) < 0.4) check(false, "no low end");
        if (n < 400) dump.push(style + " " + Object.entries(p).map(([k, v]) => `${k}=${v}`).join(" "));
        if (failures) break;
    }
}
fs.writeFileSync(path.join(outDir, "random_patches.txt"), dump.join("\n") + "\n");
/* the styles really differ */
const mean = (style, k) => { let s = 0; for (let i = 0; i < 500; i++) s += Number(randomPatch(Math.random, style)[k]); return s / 500; };
check(mean(1, "vcf_cut") < mean(2, "vcf_cut") && mean(1, "delay") > mean(2, "delay") && mean(2, "fm_drive") > mean(1, "fm_drive"),
      "DARK is darker and wetter, INDUSTRIAL harder");
check(mean(0, "motion") > mean(1, "motion"), "CHAOTIC moves the most");

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
const MAIN = ["volume", "drift_x", "drift_y", "fm_drive", "sync_sweep", "vcf_cut", "vcf_reso", "motion"];
const SETUP = ["vco1_freq", "vco2_freq", "vco_mix", "bass", "dirty_sub", "delay", "character", "random_style"];
check(JSON.stringify(c.page.keys.map(bare)) === JSON.stringify(MAIN), "page 1 = MAIN: " + c.page.keys);
const knobPages = c.pages.filter((pg) => pg.keys && pg.keys.length);
check(knobPages.length >= 2 && JSON.stringify(knobPages[1].keys.map(bare)) === JSON.stringify(SETUP),
      "page 2 = SETUP: " + (knobPages[1] && knobPages[1].keys));
check(c.pages.filter((pg) => pg.keys && pg.keys.some((k) => keys.includes(bare(k)))).length === 2,
      "exactly two parameter pages: " + c.pages.map((pg) => pg.title || pg.name));
check(!reads.includes("ui_hierarchy"), "the grid reads grid_hierarchy, never the empty ui_hierarchy");
check(fb.countLit() > 200, "page 1 draws");
console.log(fb.toBlocks ? fb.toBlocks() : fb.toAscii());
check(ui.title() === "Drifter", "title: " + ui.title());

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
w0 = writes.length;
ui.onMidi([0xb0, 78, 1]);                       /* knob 8 (MOTION) up */
ticks(10);
check(writes.slice(w0).some(([k, v]) => k === "motion" && Number(v) > 0.35), "knob 8 = MOTION: " + writes.slice(w0));

/* ---- another pad does nothing ------------------------------------------------- */
w0 = writes.length;
ui.onMidi([0x90, 70, 100]); ui.onMidi([0x80, 70, 0]);
ticks(4);
check(writes.length === w0, "pad 70 writes nothing: " + writes.slice(w0));

/* ---- RANDOM ------------------------------------------------------------------- */
const keep = Object.fromEntries(RANDOM_KEEP.map((k) => [k, values[k]]));
values.drift_x = "0.6"; values.drift_y = "-0.4";
w0 = writes.length;
ui.onMidi([0x90, RANDOM_PAD, 100]);
check(leds[RANDOM_PAD] === LED.PRESSED, "RANDOM lights while held");
const rw = writes.slice(w0);
const written = new Set(rw.map(([k]) => k));
check([...randomized].every((k) => written.has(k)), "RANDOM writes every randomized key: " + [...written]);
check(RANDOM_KEEP.every((k) => !written.has(k)), "RANDOM writes none of the kept keys: " + [...written]);
check(Object.entries(keep).every(([k, v]) => values[k] === v), "VOLUME, CHARACTER, STYLE unchanged");
check(values.drift_x === "0" && values.drift_y === "0", "DRIFT X/Y back to the centre");
check(ui.title() === "RANDOM CHAOTIC", "header says RANDOM and the style: " + ui.title());
ui.onMidi([0x90, RANDOM_PAD, 100]);             /* a repeat of a held pad is not a new press */
check(writes.length === w0 + rw.length, "held pad fires once");
ui.onMidi([0x80, RANDOM_PAD, 0]);
check(leds[RANDOM_PAD] === LED.RANDOM, "RANDOM back to its colour on release");
ticks(10);
clock += 2000; ticks(2);
check(ui.title() === "Drifter", "notice clears");
/* the style on SETUP is the one used */
values.random_style = "1";
ui.onMidi([0x90, RANDOM_PAD, 100]); ui.onMidi([0x80, RANDOM_PAD, 0]);
check(ui.title() === "RANDOM DARK", "style DARK: " + ui.title());
check(Number(values.vcf_cut) <= STYLE_RANGES.DARK.vcf_cut[1] + 1e-9 || Number(values.vcf_cut) <= Number(values.vco1_freq) * 0.5 + 0.18 + 1e-3,
      "DARK range used: cut " + values.vcf_cut);
values.random_style = "0";

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
console.log("PASS: ui (Drifter, real page engine, 9000 RANDOM patches)");
