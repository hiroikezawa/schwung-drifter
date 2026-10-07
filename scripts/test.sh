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
if command -v "${CROSS_PREFIX:-aarch64-linux-gnu-}gcc" >/dev/null 2>&1; then
  bash "$MODULE/scripts/build.sh" arm64
else
  echo "SKIP: no aarch64 cross compiler; ABI not checked" >&2
fi
