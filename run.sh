#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ -n "${LLMSWITCH_BIN:-}" ]]; then
    BIN="$LLMSWITCH_BIN"
elif [[ -x build-live2d/llm-switch && -f build-live2d/CMakeCache.txt ]] \
        && grep -q '^LLMSWITCH_LIVE2D:BOOL=ON$' build-live2d/CMakeCache.txt; then
    BIN="build-live2d/llm-switch"
else
    BIN="build/llm-switch"
fi
if [ ! -x "$BIN" ]; then
    echo "binary not found — run \`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 4\` first" >&2
    exit 1
fi
if [[ "$BIN" == "build-live2d/llm-switch" ]]; then
    echo "using Live2D-enabled build: $BIN" >&2
fi
export INTEL_FORCE_PROBE="${INTEL_FORCE_PROBE:-1}"   # Intel Arc B390 iris DRI workaround
exec "$BIN" "$@"
