#!/usr/bin/env bash
# Build the Hertz WASM engine into web/public/ (hertz.js + hertz.wasm).
# Links the Emscripten-built MKF static libs from WebLibMKF for the FFT.
#   env: EMSDK_ENV     (default /home/alf/emsdk/emsdk_env.sh)
#        WEBLIBMKF     (default ~/OpenMagnetics/WebLibMKF)
set -euo pipefail
cd "$(dirname "$0")/.."

EMSDK_ENV="${EMSDK_ENV:-/home/alf/emsdk/emsdk_env.sh}"
WEBLIBMKF="${WEBLIBMKF:-$HOME/OpenMagnetics/WebLibMKF}"
[ -f "$EMSDK_ENV" ] || { echo "emsdk env not found: $EMSDK_ENV (set EMSDK_ENV)" >&2; exit 1; }
MKF_WASM_LIBS=("$WEBLIBMKF"/build/_deps/mkf-build/*.a)
[ -e "${MKF_WASM_LIBS[0]}" ] || { echo "no wasm libMKF under $WEBLIBMKF/build — build WebLibMKF first" >&2; exit 1; }
JSON_INC="$WEBLIBMKF/build/_deps/json-src/include"
[ -d "$JSON_INC" ] || { echo "nlohmann json not found at $JSON_INC" >&2; exit 1; }

source "$EMSDK_ENV" >/dev/null

mkdir -p web/public
em++ -O2 -std=c++20 -fwasm-exceptions --bind \
  -I cpp/include -I "$JSON_INC" \
  cpp/bindings/wasm_bindings.cpp \
  "${MKF_WASM_LIBS[@]}" \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createHertz \
  -sALLOW_MEMORY_GROWTH=1 -sDYNAMIC_EXECUTION=0 -sENVIRONMENT=web,worker \
  -o web/public/hertz.js

ls -la web/public/hertz.js web/public/hertz.wasm
