#!/usr/bin/env bash
# Recompile the ffz C engine to single-file WASM ES modules:
#   wasm/ffuzzy.engine.mjs        FULL — all Unicode tables (default ffuzzyModule)
#   wasm/ffuzzy-lite.engine.mjs   LITE — ASCII + CJK only (default ffuzzyModuleLite)
#
# Both target the browser (-sENVIRONMENT=web,worker) so they bundle cleanly for
# the web (no node: imports) yet still run in Node (SINGLE_FILE inlines the wasm).
#
# LITE = the same sources but with the full Unicode tables (src/ffz_unicode_tables.c)
# swapped for the empty passthrough stub (lite-tables.c): non-ASCII casefold /
# normalize become no-ops, dropping ~17 KB. ASCII fold + CJK matching still work.
#
# This is the slow path (needs Emscripten). The committed *.engine.mjs are the
# build inputs; `npm run build` (build.mjs) appends the wrapper to them to make
# the publishable ffuzzy.js / ffuzzy-lite.js — no Emscripten needed for that.
#
#   npm run build:engine     # then: npm run build
#
# Requires Emscripten (source /d/sdk/emsdk/emsdk_env.sh, or set EMSDK).
set -euo pipefail

WASM="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # wasm/
ROOT="$(cd "$WASM/.." && pwd)"                          # repo root
OPT="${OPT:--Oz}"                                        # size-optimized for the web

if ! command -v emcc >/dev/null 2>&1; then
  for env in "${EMSDK:-}/emsdk_env.sh" /c/sdk/emsdk/emsdk_env.sh /d/sdk/emsdk/emsdk_env.sh ~/emsdk/emsdk_env.sh; do
    [ -f "$env" ] && { source "$env" >/dev/null 2>&1 || true; break; }
  done
  # Fallback: set PATH directly if emsdk_env.sh sourcing fails (e.g. missing system Python)
  if ! command -v emcc >/dev/null 2>&1 && [ -d /c/sdk/emsdk/upstream/emscripten ]; then
    export PATH="/c/sdk/emsdk/python/3.13.3_64bit:/c/sdk/emsdk/upstream/emscripten:/c/sdk/emsdk/upstream/bin:/c/sdk/emsdk/node/22.16.0_64bit:$PATH"
  fi
fi
command -v emcc >/dev/null 2>&1 || { echo "error: emcc not found; source emsdk_env.sh" >&2; exit 1; }
echo "Using $(emcc --version | head -1)"

mapfile -t SYMS < <(grep -oE 'ffz_ffi_[a-z_0-9]+' "$ROOT/ffi/ffz_ffi.c" \
                    | grep -v 'install_crash_handler' | sort -u)
EXPORTS="_malloc,_free"
for s in "${SYMS[@]}"; do EXPORTS+=",_$s"; done
RUNTIME="ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,getValue,setValue,HEAPU8,HEAPU32,HEAP32"

COMMON=(-std=c11 -DFFZ_NO_THREADS -I"$ROOT/include"
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sSINGLE_FILE=1
  -sENVIRONMENT=web,worker -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0
  -sEXPORTED_FUNCTIONS="$EXPORTS" -sEXPORTED_RUNTIME_METHODS="$RUNTIME")

echo "--- FULL -> $WASM/ffuzzy.engine.mjs ($OPT) ---"
emcc $OPT "${COMMON[@]}" -sEXPORT_NAME=ffuzzyModule \
  "$ROOT"/src/*.c "$ROOT/ffi/ffz_ffi.c" \
  -o "$WASM/ffuzzy.engine.mjs"

echo "--- LITE -> $WASM/ffuzzy-lite.engine.mjs ($OPT, empty Unicode tables) ---"
# all engine sources EXCEPT the full Unicode tables, plus the passthrough stub
LITE_SRC=()
for f in "$ROOT"/src/*.c; do
  [ "$(basename "$f")" = "ffz_unicode_tables.c" ] || LITE_SRC+=("$f")
done
# -DFFZ_COMPACT_CLASS drops the ~12 KB class table too (ffz_chars.c falls back to
# an approximation; affects only non-ASCII scoring precision, never match/no-match).
emcc $OPT "${COMMON[@]}" -DFFZ_COMPACT_CLASS -sEXPORT_NAME=ffuzzyModuleLite \
  "${LITE_SRC[@]}" "$WASM/lite-tables.c" "$ROOT/ffi/ffz_ffi.c" \
  -o "$WASM/ffuzzy-lite.engine.mjs"

ls -la "$WASM"/ffuzzy.engine.mjs "$WASM"/ffuzzy-lite.engine.mjs
echo "done. Now run: npm run build   (appends the wrapper -> *.js)"
