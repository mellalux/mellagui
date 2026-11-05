#!/bin/sh
# Run the end-to-end test-suite. LUMEN_BIN selects the compositor binary (default build/lumen).
cd "$(dirname "$0")/.." || exit 1
BIN="${LUMEN_BIN:-build/lumen}"
[ -x "$BIN" ] || { echo "$BIN not built (run make)"; exit 1; }
export LUMEN_BIN="$(realpath "$BIN")"
exec python3 -I tests/test_lumen.py "$@"
