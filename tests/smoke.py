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
assert manifest["ui_chain"] == "ui_chain.js" and manifest["name"] == "Drifter"
meta = json.loads(read("chain_params"))
assert meta == manifest["capabilities"]["chain_params"] and len(meta) == 16
MAIN = "volume drift_x drift_y fm_drive sync_sweep vcf_cut vcf_reso motion".split()
SETUP = "vco1_freq vco2_freq vco_mix bass dirty_sub delay character random_style".split()
assert [m["key"] for m in meta] == MAIN + SETUP, "parameter IDs and order"
M = {m["key"]: m for m in meta}
assert M["character"]["options"] == ["FM", "SYNC", "FM+SYNC"] and M["character"]["default"] == 2, \
    "FM+SYNC is the default CHARACTER"
assert M["random_style"]["default"] == 0 and len(M["random_style"]["options"]) == 3
assert M["drift_x"]["min"] == -1 and M["drift_y"]["min"] == -1, "DRIFT X/Y are bipolar"
assert read("ui_hierarchy") == "", "served empty: the shadow UI then opens ui_chain.js"
grid = read("grid_hierarchy")
h = json.loads(grid)
(pathlib.Path(LIB).parent / "grid_hierarchy.json").write_text(grid)   # for tests/test_ui.mjs
L = h["levels"]
assert sorted(L) == ["root", "setup"], "two pages"
assert L["root"]["knobs"] == MAIN and L["setup"]["knobs"] == SETUP
assert [e["level"] for e in L["root"]["params"] if isinstance(e, dict)] == ["setup"]
assert read("no_such_key") is None
eb = c.create_string_buffer(64)
assert get_error(p, eb, 64) == 0

# --- every parameter: default reported, set/get round trip ---------------------
for m in meta:
    v = read(m["key"])
    assert abs(float(v) - m["default"]) < 1e-4, (m["key"], v)
for m in meta:
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
write("vcf_cut", M["vcf_cut"]["default"])
# keys of 0.7.0 and before: ignored, never a crash
for old in ("vco1_mod", "gate_hold", "trigger", "sub", "lfo_rate", "vcf_vco1"):
    write(old, 1)
    assert read(old) is None, old

# --- state round trip; a bad blob changes nothing --------------------------------
write("fm_drive", 0.61); write("character", 1); write("drift_x", -0.25)
blob = read("state")
st = json.loads(blob)
assert st["v"] == 3 and len(st["p"]) == 16, "every parameter"
q = create(b".", None)
write("state", blob, inst=q)
assert read("state", inst=q) == blob
assert read("drift_x", inst=q) == "-0.2500"
for badblob in ['{"v":3,"p":[1,2]}', blob.replace('"v":3', '"v":4'),
                blob.replace("0.6100", "NaN"), blob.replace("0.6100", "7"), "junk",
                blob.replace('"v":3', '"v":2')]:            # v2 has 29 values
    write("state", badblob, inst=q)
    assert read("state", inst=q) == blob, badblob
# a 0.6.0 / 0.7.0 (v2, 29 values) or 0.5.0 (v1, 28) state: loads, VOLUME / CUTOFF / RESO
# carried over, everything else this version's default
old_v2 = [0.63, 0, 0, 0, 0, 0.8, 0.565526, 0, 2, 0, 0, 0.5, 0.42, 0.66, 0, 0, 0, 0, 0.4, 0,
          0.5, 0.25, 0.7, 0, -0.5, 0, 1, 0.33, 0.5]
for ver, vals in ((2, old_v2), (1, old_v2[:-1])):
    r = create(b".", None)
    write("state", json.dumps({"v": ver, "p": vals}), inst=r)
    assert read("volume", inst=r) == "0.3300" and read("vcf_cut", inst=r) == "0.4200" \
        and read("vcf_reso", inst=r) == "0.6600", ("legacy state", ver)
    assert read("drift_x", inst=r) == "0.0000" and read("character", inst=r) == "2", ver
    seconds(0.1, inst=r)
    destroy(r)
destroy(q)
for k in ("fm_drive", "character", "drift_x"):
    write(k, M[k]["default"])

# --- audio: Move's transport decides, MIDI notes do not ----------------------------
KEEP = {0, 6}          # 6 LSB keepalive DC while stopped
assert set(seconds(0.5)) <= KEEP, "stopped: silent (keepalive DC only)"
midi_msg(0x90, 60, 100)
assert set(seconds(0.5)) <= KEEP, "a MIDI note does not sound it"
midi_msg(0x80, 60, 0)
transport["beat"] = 0.0
first = block()[0::2]
assert peak(first[:16]) < 2000, "Play fades in, no click"
run = seconds(1.0)
assert peak(run) > 3000 and len(set(run)) > 200, "drones while Move plays"
# notes, velocity and bend change NOTHING: two instances, notes sent to one only,
# render sample-identical audio (the engine is deterministic)
qa, qb = create(b".", None), create(b".", None)
same = True
for i in range(int(1.0 * SR / BLOCK)):
    if i % 20 == 3:
        midi_msg(0x90, 30 + i % 50, 127, inst=qb); midi_msg(0xE0, 0, i % 128, inst=qb)
    if i % 20 == 13:
        midi_msg(0x80, 30 + (i - 10) % 50, 0, inst=qb)
    if block(qa) != block(qb):
        same = False
assert same, "MIDI notes / bend changed the sound"
destroy(qa); destroy(qb)
transport["beat"] = -1.0
tail = seconds(0.1)
assert peak(tail[-500:]) <= 6, "Stop fades out (delay included) within 0.1 s"
assert set(seconds(0.5)) <= KEEP, "stopped: silent"
transport["beat"] = 3.5
assert peak(seconds(0.5)) > 3000, "Play again"
transport["beat"] = -1.0
seconds(0.2)

# --- VOLUME 0 is exact silence -------------------------------------------------------------
transport["beat"] = 0.0
write("volume", 0)
seconds(0.2)
assert not any(seconds(0.3)), "VOLUME 0: exact silence"
write("volume", 0.7)

# --- every knob at its top at once: bounded -----------------------------------------------
hi = lambda m: len(m["options"]) - 1 if m["type"] == "enum" else m["max"]
lo = lambda m: 0 if m["type"] == "enum" else m["min"]
for m in meta:
    write(m["key"], hi(m))
wild = seconds(3.0)
assert peak(wild) > 3000 and peak(wild) < 32767, "all-max: loud but not clipped"
for m in meta:
    write(m["key"], lo(m) if m["key"] != "volume" else 0.7)
low = seconds(2.0)
assert peak(low[-44100:]) > 1000, "all-min still sounds (the main VCOs at A0)"
transport["beat"] = -1.0
seconds(0.2)

# --- MIDI clock fallback (a host without get_beat_position) ---------------------------------
destroy(p)
host_noclk = Host(api_version=1, sample_rate=SR, frames_per_block=BLOCK)
lib.move_plugin_init_v2(c.byref(host_noclk))
q = create(b".", None)
assert set(seconds(0.3, inst=q)) <= KEEP, "no beat position, no clock: stopped"
midi_msg(0xFA, inst=q)
got = []
for _ in range(int(0.4 * SR / BLOCK)):
    midi_msg(0xF8, inst=q)
    got += block(q)[0::2]
assert peak(got) > 3000, "MIDI Start + clock: drones"
midi_msg(0xFC, inst=q)
seconds(0.1, inst=q)
assert set(seconds(0.3, inst=q)) <= KEEP, "MIDI Stop: silent"
midi_msg(0xFA, inst=q)
seconds(1.0, inst=q)                   # clock stops arriving: times out
assert set(seconds(0.3, inst=q)) <= KEEP, "a lost Stop does not drone forever"
destroy(q)

# --- pool ----------------------------------------------------------------------------------
lib.move_plugin_init_v2(c.byref(host))
extra = [create(b".", None) for _ in range(6)]
assert all(extra) and not create(b".", None), "6 instances, then NULL"
for x in extra:
    destroy(x)
host48 = Host(api_version=1, sample_rate=48000, frames_per_block=BLOCK, get_beat_position=beat_cb)   # kept alive
lib.move_plugin_init_v2(c.byref(host48))
q = create(b".", None)
assert q, "48 kHz host: rate-agnostic engine"
transport["beat"] = 0.0
assert peak(seconds(0.5, inst=q)) > 3000, "48 kHz instance sounds"
destroy(q)
lib.move_plugin_init_v2(c.byref(host))
print("PASS: smoke (dlopen, v2 ABI)")
