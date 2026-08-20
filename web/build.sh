#!/usr/bin/env bash
# Compiles the analysis core to WebAssembly.
#
# Needs emscripten on the PATH:
#   git clone https://github.com/emscripten-core/emsdk
#   cd emsdk && ./emsdk install latest && ./emsdk activate latest
#   source ./emsdk_env.sh
#
# Then from the repo root:  ./web/build.sh
#
# The output (cspectrum.js + cspectrum.wasm) is committed, so the demo page
# works from a plain checkout without anyone needing emscripten.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/spindoctor"

mkdir -p "$OUT"

# Only the parts that make sense in a browser. No cs_source (that is
# miniaudio, and Web Audio already gives us samples), no cs_telemetry (no
# threads, no files), no cs_engine (it pulls in cs_source).
SRC=(
  "$ROOT/web/cs_web.c"
  "$ROOT/src/cs_config.c"
  "$ROOT/src/cs_biquad.c"
  "$ROOT/src/cs_features.c"
  "$ROOT/src/cs_analysis.c"
  "$ROOT/src/cs_envelope.c"
  "$ROOT/src/cs_monitor.c"
  "$ROOT/deps/kissfft/kiss_fft.c"
  "$ROOT/deps/kissfft/kiss_fftr.c"
)

emcc "${SRC[@]}" \
  -I"$ROOT/src" -I"$ROOT/deps/kissfft" \
  -O3 \
  -o "$OUT/cspectrum.js" \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createCSpectrum \
  -s EXPORT_ES6=0 \
  -s ENVIRONMENT=web,worker,node \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=16MB \
  -s FILESYSTEM=0 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPF32"]' \
  -s EXPORTED_FUNCTIONS='["_csw_init","_csw_free","_csw_input_ptr","_csw_input_cap","_csw_push","_csw_feature","_csw_feature_peak","_csw_peak","_csw_dominant_hz","_csw_blocks","_csw_time","_csw_spectrum","_csw_spectrum_len","_csw_bin_hz","_csw_analyse_envelope","_csw_env_ready","_csw_env_hz","_csw_env_prominence","_csw_env_spectrum","_csw_env_spectrum_len","_csw_env_bin_hz","_csw_monitor_start","_csw_monitor_stop","_csw_monitor_on","_csw_state","_csw_health","_csw_progress","_csw_alarms","_csw_worst_feature","_csw_feature_ewma","_malloc","_free"]'

echo
echo "built:"
ls -la "$OUT/cspectrum.js" "$OUT/cspectrum.wasm"
