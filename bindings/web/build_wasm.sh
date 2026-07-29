#!/usr/bin/env bash
# Builds Aulos for the browser: one ES module, WebAssembly inlined.
#
#   source /opt/emsdk/emsdk_env.sh
#   bindings/web/build_wasm.sh
#
# Output: bindings/web/aulos-wasm.mjs  (self contained, no .wasm side file)
#
# Why SINGLE_FILE: an AudioWorkletGlobalScope has no fetch() and no
# XMLHttpRequest, so a separate .wasm file cannot be loaded from inside the
# audio thread. Inlining it as base64 removes the problem entirely.
#
# Why MA_NO_DEVICE_IO: the browser owns the audio device (WebAudio pulls from
# us), so miniaudio's device layer is dead weight here. Decoding and
# resampling - the parts Aulos actually uses - stay in.
set -euo pipefail

cd "$(dirname "$0")/../.."
ROOT="$PWD"
OUT="$ROOT/bindings/web/aulos-wasm.mjs"

command -v emcc >/dev/null || { echo "emcc not found - source /opt/emsdk/emsdk_env.sh" >&2; exit 1; }

EXPORTS='["_aulw_create","_aulw_set_listener","_aulw_play_3d","_aulw_set_position","_aulw_get_stats","_aulw_render_planar","_aul_destroy","_aul_load_bank","_aul_last_error","_aul_update","_aul_play","_aul_stop","_aul_stop_all","_aul_is_playing","_aul_set_volume","_aul_set_pitch","_aul_set_parameter","_aul_set_bus_volume","_aul_get_bus_volume","_aul_event_exists","_aul_render","_malloc","_free"]'
RUNTIME='["FS","UTF8ToString","stringToNewUTF8","HEAPF32","HEAPF64","HEAPU8"]'

emcc \
  "$ROOT/src/aulos.cpp" \
  "$ROOT/src/miniaudio_impl.c" \
  "$ROOT/bindings/web/aulos_web.c" \
  -I"$ROOT/include" -I"$ROOT/extern" \
  -O3 -flto \
  -DAULOS_NO_DEVICE=1 -DMA_NO_DEVICE_IO=1 \
  -Wno-unused-command-line-argument \
  --no-entry \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createAulos \
  -sSINGLE_FILE=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=33554432 \
  -sFILESYSTEM=1 \
  -sENVIRONMENT=web,worker,node \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS="$RUNTIME" \
  -o "$OUT"

echo "built $OUT ($(du -h "$OUT" | cut -f1))"
