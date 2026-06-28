#!/usr/bin/env bash
# Recompile the ffz C engine to a single-file WASM ES module:
#   wasm/ffuzzy.engine.mjs   (SINGLE_FILE — wasm base64-inlined, default export = ffuzzyModule)
#
# This is the slow path (needs Emscripten). The committed *.engine.mjs are the
# build inputs; `npm run build` (build.mjs) appends the wrapper to them to make
# the publishable ffuzzy.js / ffuzzy-lite.js — no Emscripten needed for that.
#
#   npm run build:engine     # then: npm run build
#
# Requires Emscripten (source /d/sdk/emsdk/emsdk_env.sh, or set EMSDK).
#
# LITE: the ASCII+CJK lite engine (ffuzzy-lite.engine.mjs) uses slimmed Unicode
# tables and is NOT yet reproducible here — its recipe (gen_unicode_tables.py
# options + table swap) must be documented first. Until then the committed
# ffuzzy-lite.engine.mjs is the frozen legacy build. TODO.
set -euo pipefail

WASM="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # wasm/
ROOT="$(cd "$WASM/.." && pwd)"                          # repo root
OPT="${OPT:--Oz}"                                        # size-optimized for the web

if ! command -v emcc >/dev/null 2>&1; then
  for env in "${EMSDK:-}/emsdk_env.sh" /d/sdk/emsdk/emsdk_env.sh ~/emsdk/emsdk_env.sh; do
    [ -f "$env" ] && { source "$env" >/dev/null 2>&1 || true; break; }
  done
fi
command -v emcc >/dev/null 2>&1 || { echo "error: emcc not found; source emsdk_env.sh" >&2; exit 1; }
echo "Using $(emcc --version | head -1)"

mapfile -t SYMS < <(grep -oE 'ffz_ffi_[a-z_0-9]+' "$ROOT/ffi/ffz_ffi.c" \
                    | grep -v 'install_crash_handler' | sort -u)
EXPORTS="_malloc,_free"
for s in "${SYMS[@]}"; do EXPORTS+=",_$s"; done
RUNTIME="ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,getValue,setValue,HEAPU8,HEAPU32,HEAP32"

echo "--- building FULL engine -> $WASM/ffuzzy.engine.mjs ($OPT, SINGLE_FILE) ---"
emcc $OPT -std=c11 -DFFZ_NO_THREADS -I"$ROOT/include" \
  "$ROOT"/src/*.c "$ROOT/ffi/ffz_ffi.c" \
  -o "$WASM/ffuzzy.engine.mjs" \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=ffuzzyModule \
  -sSINGLE_FILE=1 \
  -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS="$RUNTIME"

ls -la "$WASM/ffuzzy.engine.mjs"
echo "done. Now run: npm run build   (appends the wrapper -> ffuzzy.js / ffuzzy-lite.js)"
echo "NOTE: ffuzzy-lite.engine.mjs is the frozen legacy lite build (recipe TODO)."
