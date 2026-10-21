#!/usr/bin/env python3
"""dlopen the real dsp.so through the v2 ABI, as the chain host does."""
import ctypes as c
import json
import pathlib
import sys

LIB = sys.argv[1]
BLOCK = 128
SR = 44100


BPMFN = c.CFUNCTYPE(c.c_float)
BEATFN = c.CFUNCTYPE(c.c_double)
transport = {"beat": -1.0}
beat_cb = BEATFN(lambda: transport["beat"])


class Host(c.Structure):
    _fields_ = [("api_version", c.c_uint32), ("sample_rate", c.c_int),
                ("frames_per_block", c.c_int), ("mapped_memory", c.c_void_p),
                ("audio_out_offset", c.c_int), ("audio_in_offset", c.c_int),
                ("log", c.c_void_p), ("midi_send_internal", c.c_void_p),
                ("midi_send_external", c.c_void_p), ("get_clock_status", c.c_void_p),
                ("mod_emit_value", c.c_void_p), ("mod_clear_source", c.c_void_p),
                ("mod_host_ctx", c.c_void_p), ("get_bpm", c.c_void_p),
                ("midi_inject_to_move", c.c_void_p), ("slot_recv_channel", c.c_void_p),
                ("get_beat_position", BEATFN), ("reserved", c.c_void_p * 8)]


assert Host.get_beat_position.offset == 112 and Host.reserved.offset == 120


class API(c.Structure):
    _fields_ = [("version", c.c_uint32)] + [(k, c.c_void_p) for k in
               ("create", "destroy", "midi", "set", "get", "error", "render")]


lib = c.CDLL(LIB)
lib.move_plugin_init_v2.restype = c.POINTER(API)
lib.move_plugin_init_v2.argtypes = [c.c_void_p]
bad = Host(api_version=1, sample_rate=8000, frames_per_block=BLOCK)
assert not lib.move_plugin_init_v2(c.byref(bad)), "must refuse an 8 kHz host"
host = Host(api_version=1, sample_rate=SR, frames_per_block=BLOCK, get_beat_position=beat_cb)
api = lib.move_plugin_init_v2(c.byref(host)).contents
assert api.version == 2

create = c.CFUNCTYPE(c.c_void_p, c.c_char_p, c.c_char_p)(api.create)
destroy = c.CFUNCTYPE(None, c.c_void_p)(api.destroy)
midi = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(c.c_uint8), c.c_int, c.c_int)(api.midi)
set_param = c.CFUNCTYPE(None, c.c_void_p, c.c_char_p, c.c_char_p)(api.set)
get_param = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_char_p, c.c_char_p, c.c_int)(api.get)
get_error = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_char_p, c.c_int)(api.error)
render = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(c.c_int16), c.c_int)(api.render)

buf = (c.c_int16 * (BLOCK * 2))()
p = create(b".", None)
assert p, "create_instance"


def read(key, size=65536, inst=None):
    out = c.create_string_buffer(size)
    n = get_param(inst or p, key.encode(), out, size)
    return None if n < 0 else out.value.decode()


def write(key, value, inst=None):
    set_param(inst or p, key.encode(), str(value).encode())


def block(inst=None):
    render(inst or p, buf, BLOCK)
    return list(buf)


def midi_msg(*b, inst=None):
    midi(inst or p, (c.c_uint8 * len(b))(*b), len(b), 0)


def seconds(s, inst=None):
    out = []
    for _ in range(int(s * SR / BLOCK)):
        out += block(inst)[0::2]
    return out


def peak(samples):
    return max(abs(x) for x in samples)


def freq(samples):
    """Rising zero crossings with hysteresis -> Hz."""
    n, state = 0, 0
    for x in samples:
        if state <= 0 and x > 300:
            n, state = n + 1, 1
        elif x < -300:
            state = -1
    return n * SR / len(samples)


# --- contract with the shadow UI ----------------------------------------------
manifest = json.loads((pathlib.Path(__file__).parent.parent / "module.json").read_text())
assert manifest["component_type"] == "sound_generator" and manifest["capabilities"]["chainable"]
assert manifest["ui_chain"] == "ui_chain.js"
meta = json.loads(read("chain_params"))
assert meta == manifest["capabilities"]["chain_params"] and len(meta) == 30
STABLE = """vco1_freq vco1_mod vco1_wave vco1_mode vco1_lfo vco1_vol
vco2_freq vco2_mod vco2_wave vco2_mode vco2_lfo vco2_vol
vcf_cut vcf_reso vcf_mod vcf_vco1 vcf_vco2 vcf_lfo lfo_rate lfo_wave
delay_time delay_feedback drift_x drift_x_target drift_y drift_y_target
gate_hold trigger volume sub""".split()
assert [m["key"] for m in meta] == STABLE, "stable parameter IDs"
keys = {m["key"] for m in meta}
assert read("ui_hierarchy") == "", "served empty: the shadow UI then opens ui_chain.js"
grid = read("grid_hierarchy")
h = json.loads(grid)
(pathlib.Path(LIB).parent / "grid_hierarchy.json").write_text(grid)   # for tests/test_ui.mjs
L = h["levels"]
assert L["root"]["knobs"] == ["volume", "drift_x", "drift_y", "vco1_freq",
                              "vco1_mod", "vco2_freq", "vco2_mod", "sub"]
assert L["mixvcf"]["knobs"] == ["vco1_vol", "vco2_vol", "vcf_cut", "vcf_reso",
                                "vcf_mod", "lfo_rate", "delay_time", "delay_feedback"]
assert L["setup1"]["knobs"] == ["drift_x_target", "drift_y_target", "vco1_wave", "vco1_mode",
                                "vco2_wave", "vco2_mode", "vco1_lfo", "vco2_lfo"]
assert L["setup2"]["knobs"] == ["vcf_vco1", "vcf_vco2", "vcf_lfo", "lfo_wave",
                                "gate_hold", "trigger"]
assert [e["level"] for e in L["root"]["params"] if isinstance(e, dict)] == ["mixvcf", "setup1", "setup2"]
on_pages = [k for lvl in L.values() for k in lvl["knobs"]]
assert sorted(on_pages) == sorted(STABLE) and len(set(on_pages)) == len(on_pages), \
    "every parameter on exactly one page"
for lvl in L.values():
    for k in lvl["knobs"]:
        assert k in keys, k
assert next(m for m in meta if m["key"] == "trigger")["access"] == "write"
assert read("no_such_key") is None
eb = c.create_string_buffer(64)
assert get_error(p, eb, 64) == 0

# --- every parameter: default reported, set/get round trip ---------------------
for m in meta:
    v = read(m["key"])
    assert abs(float(v) - m["default"]) < 1e-4, (m["key"], v)
for m in meta:
    if m["key"] == "trigger":
        continue
    if m["type"] == "enum":
        for i, name in enumerate(m["options"]):
            write(m["key"], i)
            assert read(m["key"]) == str(i), (m["key"], i)
            write(m["key"], name)                          # names work too
            assert read(m["key"]) == str(i), (m["key"], name)
    else:
        for v in (m["min"], m["max"], (m["min"] + m["max"]) / 2):
            write(m["key"], v)
            assert abs(float(read(m["key"])) - v) < 1e-4, (m["key"], v)
    write(m["key"], "nan"); write(m["key"], "0.3x"); write(m["key"], "")
    write(m["key"], m["default"])
    assert abs(float(read(m["key"])) - m["default"]) < 1e-4, (m["key"], "junk changed it")
write("vcf_cut", 5)
assert read("vcf_cut") == "1.0000", "clamped"
write("vcf_cut", 0.7)

# --- state round trip; a bad blob changes nothing --------------------------------
write("vco1_mod", 0.61); write("vco2_wave", 4); write("drift_x", -0.25)
blob = read("state")
st = json.loads(blob)
assert st["v"] == 2 and len(st["p"]) == 29, "every parameter but TRIGGER"
q = create(b".", None)
write("state", blob, inst=q)
assert read("state", inst=q) == blob
assert read("drift_x", inst=q) == "-0.2500"
for badblob in ['{"v":2,"p":[1,2]}', blob.replace('"v":2', '"v":3'),
                blob.replace('"v":2', '"v":1'),            # v1 has one value fewer
                blob.replace("0.6100", "NaN"), blob.replace("0.6100", "7"), "junk"]:
    write("state", badblob, inst=q)
    assert read("state", inst=q) == blob, badblob
# a 0.5.0 (v1) state has no SUB: it loads, and SUB takes its default
v1 = json.dumps({"v": 1, "p": st["p"][:-1]}, separators=(",", ":"))
r = create(b".", None)
write("sub", 0.9, inst=r)
write("state", v1, inst=r)
assert read("drift_x", inst=r) == "-0.2500" and read("sub", inst=r) == "0.5000", "v1 state loads"
destroy(r)
destroy(q)
for k in ("vco1_mod", "vco2_wave", "drift_x"):
    write(k, next(m for m in meta if m["key"] == k)["default"])

# --- audio: silence without a note, sound with one, note off stops ------------------
KEEP = {0, 6}          # 6 LSB keepalive while recently edited
assert read("gate_hold") == "1", "0.3.0: GATE HOLD Play by default"
assert set(seconds(0.3)) <= KEEP, "stopped: silent (keepalive DC only)"
transport["beat"] = 0.0
assert peak(seconds(0.3)) > 2000, "default patch sounds while Move plays"
transport["beat"] = -1.0
seconds(1.0)                           # the delay's tail rings out
assert peak(seconds(0.3)) <= 30, "Stop closes the gate"
write("gate_hold", 0)
write("delay_feedback", 0)
seconds(0.3)
assert set(seconds(0.5)) <= KEEP, "no note: silence (or keepalive DC)"
midi_msg(0x90, 60, 100)
on = seconds(0.5)
assert peak(on) > 2000 and len(set(on)) > 100, "note on -> audio"
midi_msg(0x80, 60, 0)
seconds(0.4)
assert peak(seconds(0.3)) <= 30, "note off stops"
midi_msg(0x90, 60, 100)
seconds(0.1)
midi_msg(0x90, 60, 0)                  # velocity-0 note on is a note off
seconds(0.4)
assert peak(seconds(0.3)) <= 30

# --- mono, last-note priority ------------------------------------------------------
write("vco2_vol", 0); write("vcf_cut", 1); write("vcf_reso", 0)
midi_msg(0x90, 48, 100)
f48 = freq(seconds(0.5)[2000:])
midi_msg(0x90, 60, 100)
f60 = freq(seconds(0.5)[2000:])
midi_msg(0x80, 60, 0)                  # back to the key still held
f48b = freq(seconds(0.5)[2000:])
assert abs(f48 / 130.8 - 1) < 0.03 and abs(f60 / 261.6 - 1) < 0.03, (f48, f60)
assert abs(f48b / f48 - 1) < 0.03, ("last-note priority falls back", f48b)
midi_msg(0xB0, 123, 0)                 # All Notes Off
seconds(0.4)
assert peak(seconds(0.3)) <= 30, "CC 123 closes the gate"

# --- trigger: fires on its option name or index, never on "0"/"-" --------------------
for fire in ("Trig", "1"):
    write("trigger", fire)
    assert peak(seconds(0.15)) > 2000, fire
    seconds(0.5)
    assert peak(seconds(0.2)) <= 30
for dud in ("0", "-"):
    write("trigger", dud)
    assert peak(seconds(0.2)) <= 30, dud
assert read("trigger") == "0"

# --- gate hold -------------------------------------------------------------------------
write("gate_hold", "Always")
assert peak(seconds(0.3)) > 2000, "gate hold Always drones"
write("gate_hold", 0)
seconds(0.4)
assert peak(seconds(0.3)) <= 30

# --- VOLUME 0 is exact silence -------------------------------------------------------------
write("volume", 0)
midi_msg(0x90, 60, 100)
seconds(0.2)
assert not any(seconds(0.3)), "VOLUME 0: exact silence"
midi_msg(0x80, 60, 0)
write("volume", 0.7)

# --- all modulation at once: bounded (int16 cannot NaN; the engine counts resets) ----
for k, v in [("vco1_mod", 1), ("vco2_mod", 1), ("vco1_mode", 1), ("vco1_lfo", 1), ("vco2_lfo", 1),
             ("vcf_reso", 1), ("vcf_mod", 1), ("vcf_vco1", 1), ("vcf_vco2", 1), ("vcf_lfo", 1),
             ("delay_feedback", 1), ("drift_x", 1), ("drift_y", 1), ("gate_hold", 2), ("vco2_vol", 1)]:
    write(k, v)
wild = seconds(3.0)
assert peak(wild) > 1000 and peak(wild) < 32767, "all-max: loud but not clipped"

# --- pool ----------------------------------------------------------------------------------
extra = [create(b".", None) for _ in range(5)]
assert all(extra) and not create(b".", None), "6 instances, then NULL"
for x in extra:
    destroy(x)
destroy(p)
host48 = Host(api_version=1, sample_rate=48000, frames_per_block=BLOCK, get_beat_position=beat_cb)   # kept alive
lib.move_plugin_init_v2(c.byref(host48))
q = create(b".", None)
assert q, "48 kHz host: rate-agnostic engine"
midi_msg(0x90, 60, 100, inst=q)
assert peak(seconds(0.3, inst=q)) > 2000, "48 kHz instance sounds"
destroy(q)
lib.move_plugin_init_v2(c.byref(host))
print("PASS: smoke (dlopen, v2 ABI)")
