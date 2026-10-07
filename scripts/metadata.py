#!/usr/bin/env python3
"""Embed module.json's chain_params in the DSP so get_param never reads a file.

Also cross-checks the manifest against the engine's own parameter table, so the
knob metadata the host shows can never drift from what the DSP does, and emits
each enum's option names so set_param accepts a name as well as an index.
"""
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
manifest_text = (root / "module.json").read_text()
assert len(manifest_text.encode()) <= 8192, "module.json exceeds the loader's 8 KB cap"
params = json.loads(manifest_text)["capabilities"]["chain_params"]

engine = (root / "dsp" / "db_engine.c").read_text()
table = engine.split("db_params[DP_COUNT] = {", 1)[1].split("};", 1)[0]
rows = re.findall(r'\{"([a-z0-9_]+)",\s*([-0-9.]+)f,\s*([-0-9.]+)f,\s*([-0-9.]+)f,\s*([01])\}', table)
assert [p["key"] for p in params] == [r[0] for r in rows], ("chain_params order != db_params", rows)
for p, (key, lo, hi, d, is_int) in zip(params, rows):
    assert float(p["default"]) == float(d), (key, "default drift")
    if p["type"] == "enum":
        assert is_int == "1" and float(lo) == 0 and float(hi) == len(p["options"]) - 1, key
        assert all(o.isascii() for o in p["options"]), (key, "OLED font is ASCII")
    elif p["type"] == "int":
        assert is_int == "1" and float(p["min"]) == float(lo) and float(p["max"]) == float(hi), key
    else:
        assert is_int == "0" and float(p["min"]) == float(lo) and float(p["max"]) == float(hi), key

MAX_OPT = 6
assert all(len(p.get("options", [])) <= MAX_OPT for p in params)
opts = []
for p in params:
    names = p.get("options", [])
    opts.append("{" + (", ".join(json.dumps(n) for n in names) or "0") + "}")

literal = json.dumps(json.dumps(params, separators=(",", ":")), ensure_ascii=True)
out = pathlib.Path(sys.argv[1])
out.write_text(
    "/* Generated from module.json by scripts/metadata.py. Do not edit. */\n"
    "static const char db_chain_params[] = " + literal + ";\n"
    "#define DB_MAX_OPTIONS %d\n" % MAX_OPT +
    "static const char *const db_param_options[][DB_MAX_OPTIONS] = {\n    " +
    ",\n    ".join(opts) + "\n};\n")
