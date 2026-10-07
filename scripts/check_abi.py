#!/usr/bin/env python3
"""Move runs glibc 2.34; reject a dsp.so that needs anything newer."""
import re
import subprocess
import sys
path = sys.argv[1]
output = subprocess.check_output(["readelf", "--version-info", path], text=True)
versions = {tuple(map(int, s.split("."))) for s in re.findall(r"GLIBC_([0-9.]+)", output)}
assert versions and max(versions) <= (2, 34), f"Move-incompatible GLIBC: {sorted(versions)}"
header = subprocess.check_output(["readelf", "-h", path], text=True)
assert "AArch64" in header, "not an AArch64 build"
symbols = subprocess.check_output(["readelf", "--dyn-syms", "-W", path], text=True)
exported = {l.split()[-1] for l in symbols.splitlines()
            if " FUNC " in l and " GLOBAL " in l and " UND " not in l}
assert exported == {"move_plugin_init_v2"}, f"exports must be the v2 entry point only: {exported}"
print("PASS: AArch64, v2 export only, max GLIBC", ".".join(map(str, max(versions))))
