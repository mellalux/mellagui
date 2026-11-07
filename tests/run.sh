#!/bin/sh
# Run the end-to-end test-suite. MELLAGUI_BIN selects the compositor binary (default build/mellagui).
cd "$(dirname "$0")/.." || exit 1
BIN="${MELLAGUI_BIN:-build/mellagui}"
[ -x "$BIN" ] || { echo "$BIN not built (run make)"; exit 1; }
export MELLAGUI_BIN="$(realpath "$BIN")"
exec python3 -I tests/test_mellagui.py "$@"
