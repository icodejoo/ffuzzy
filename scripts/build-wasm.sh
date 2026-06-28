#!/usr/bin/env bash
# Build the ffz C engine to WebAssembly for the browser / Flutter Web.
#
# Produces an ES6 module (MODULARIZE + EXPORT_ES6) that wraps a single-threaded
# .wasm:  build/wasm/ffuzzy.mjs + build/wasm/ffuzzy.wasm
#
# The module exports the flat C-ABI shim (ffi/ffz_ffi.c) plus malloc/free, so a
# JS/Dart host drives it exactly like the native FFI lib: allocate a UTF-8
# buffer on the wasm heap, call ffz_ffi_*, read results back through the
# accessor functions.  Single-threaded (no -pthread) so it needs no
# SharedArrayBuffer / COOP-COEP headers.
#
# Usage:
#   scripts/build-wasm.sh            # -O3 (speed)
#   OPT=-Oz scripts/build-wasm.sh    # -Oz (size)
#
# Requires Emscripten on PATH (run `source /d/sdk/emsdk/emsdk_env.sh` first, or
# set EMSDK to the emsdk checkout — this script will source it for you).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUTDIR="build/wasm"
OPT="${OPT:--O3}"

# --- locate Emscripten ------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
  for env in "${EMSDK:-}/emsdk_env.sh" /d/sdk/emsdk/emsdk_env.sh ~/emsdk/emsdk_env.sh; do
    if [ -f "$env" ]; then
      # shellcheck disable=SC1090
      source "$env" >/dev/null 2>&1 || true
      break
    fi
  done
fi
command -v emcc >/dev/null 2>&1 || {
  echo "error: emcc not found. Install Emscripten and 'source emsdk_env.sh'." >&2
  exit 1
}
echo "Using $(emcc --version | head -1)"

# --- exported C functions ---------------------------------------------------
# Every ffz_ffi_* in the shim EXCEPT the crash handler (compiled out: we do not
# define FFZ_HAVE_CRASH_HANDLER for wasm). Emscripten wants a leading underscore.
mapfile -t SYMS < <(grep -oE 'ffz_ffi_[a-z_]+' ffi/ffz_ffi.c \
                    | grep -v 'install_crash_handler' | sort -u)
EXPORTS="_malloc,_free"
for s in "${SYMS[@]}"; do EXPORTS+=",_$s"; done

RUNTIME="ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,getValue,setValue,HEAPU8,HEAPU32,HEAP32"

mkdir -p "$OUTDIR"

emcc $OPT -std=c11 -DFFZ_NO_THREADS -Iinclude \
  src/*.c ffi/ffz_ffi.c \
  -o "$OUTDIR/ffuzzy.mjs" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=ffuzzyModule \
  -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS="$RUNTIME"

echo "--- output ---"
ls -la "$OUTDIR"/ffuzzy.mjs "$OUTDIR"/ffuzzy.wasm
