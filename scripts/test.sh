#!/usr/bin/env bash
# Native build + every test: engine (ASan/UBSan), real dlopen smoke through the
# v2 ABI, then the Move build (check_abi.py rejects a symbol newer than Move's
# glibc 2.34).   scripts/test.sh --quick   skips the 60 s stress render.
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
bash "$MODULE/scripts/build.sh" native
OUT="$MODULE/build/native"
"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -fno-math-errno \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$MODULE/dsp" "$MODULE/tests/test_engine.c" "$MODULE/dsp/db_engine.c" \
  -o "$OUT/test-engine" -lm
"$OUT/test-engine" "$@"
"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O2 -fno-math-errno -I"$MODULE/dsp" \
  "$MODULE/tests/test_engine.c" "$MODULE/dsp/db_engine.c" -o "$OUT/bench" -lm
"$OUT/bench" --bench
"$OUT/bench" --wav "$OUT/demo.wav"
python3 "$MODULE/tests/smoke.py" "$OUT/dsp.so"

# UI test (tests/test_ui.mjs): Schwung's real page engine from DRIFTER_SHARED
# (a copy of the device's /data/UserData/schwung/shared) or the checkout, and
# the draw harness from the checkout (SCHWUNG_ROOT, else ../schwung).
SCHWUNG="${SCHWUNG_ROOT:-$(cd "$MODULE/.." && pwd)/schwung}"
SHARED="${DRIFTER_SHARED:-$SCHWUNG/src/shared}"
NODE_EXE="$(command -v node.exe || true)"
[[ -z "$NODE_EXE" && -x "/mnt/c/Program Files/nodejs/node.exe" ]] && NODE_EXE="/mnt/c/Program Files/nodejs/node.exe"
if [[ ! -f "$SHARED/param_pages/page_controller.mjs" || ! -f "$SCHWUNG/tools/param-pages/harness.mjs" ]]; then
  echo "SKIP: UI test needs a Schwung checkout (set SCHWUNG_ROOT / DRIFTER_SHARED)" >&2
elif command -v node >/dev/null 2>&1; then
  SCHWUNG_ROOT="$SCHWUNG" DRIFTER_SHARED="$SHARED" node "$MODULE/tests/test_ui.mjs" | tail -1
elif [[ -n "$NODE_EXE" ]] && command -v wslpath >/dev/null 2>&1; then
  SCHWUNG_ROOT="$(wslpath -w "$SCHWUNG")" DRIFTER_SHARED="$(wslpath -w "$SHARED")" \
    WSLENV=SCHWUNG_ROOT:DRIFTER_SHARED "$NODE_EXE" "$(wslpath -w "$MODULE/tests/test_ui.mjs")" | tail -1
else
  echo "Node.js is required for the UI test" >&2; exit 1
fi
if command -v "${CROSS_PREFIX:-aarch64-linux-gnu-}gcc" >/dev/null 2>&1; then
  bash "$MODULE/scripts/build.sh" arm64
else
  echo "SKIP: no aarch64 cross compiler; ABI not checked" >&2
fi
