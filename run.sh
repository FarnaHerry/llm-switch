#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
BIN="${LLMSWITCH_BIN:-build/llm-switch}"
if [ ! -x "$BIN" ]; then
    echo "binary not found — run \`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 4\` first" >&2
    exit 1
fi
export INTEL_FORCE_PROBE="${INTEL_FORCE_PROBE:-1}"   # Intel Arc B390 iris DRI workaround
exec "./$BIN" "$@"
