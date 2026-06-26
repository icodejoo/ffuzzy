#!/usr/bin/env bash
# Android .so build. Default = COMPRESSED release: exact mode (full Unicode,
# byte-identical to nucleo) + -Oz + FFI-only exports + stripped/gc-sections +
# split libffz.so.debug sidecar. Produces one libffz.so per ABI under
# build/android/<abi>/.
#
# Env switches:
#   FFZ_SELFDEBUG=1        LOCATABLE build (-O1 -g, not stripped, crash handler
#                          in, asserts on) — crashes pinpointed in-process.
#   FFZ_CRASH_IN_RELEASE=1 keep the in-process crash handler in the compressed
#                          release lib (~32 KB -> ~58 KB; ship the .debug sidecar).
#   FFZ_COMPACT=1          drop the ~5 KB exact Unicode class table.
#
# Usage: ANDROID_NDK=/path/to/ndk bash scripts/build_android.sh [api]
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-/d/sdk/android/ndk/27.2.12479018}"
API="${1:-21}"
HOST="${FFZ_NDK_HOST:-windows-x86_64}"  # linux-x86_64 / darwin-x86_64 off-Windows
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
CLANG="$BIN/clang.exe"; [ -x "$CLANG" ] || CLANG="$BIN/clang"
SIZE="$BIN/llvm-size.exe";  [ -x "$SIZE" ] || SIZE="$BIN/llvm-size"
OBJCOPY="$BIN/llvm-objcopy.exe"; [ -x "$OBJCOPY" ] || OBJCOPY="$BIN/llvm-objcopy"
STRIP="$BIN/llvm-strip.exe"; [ -x "$STRIP" ] || STRIP="$BIN/llvm-strip"

# Space-separated "abi:clang-target-triple" pairs (no bash arrays — POSIX sh).
TARGETS="arm64-v8a:aarch64-linux-android$API \
armeabi-v7a:armv7a-linux-androideabi$API \
x86_64:x86_64-linux-android$API \
x86:i686-linux-android$API"
# Set FFZ_COMPACT=1 to drop the ~5 KB exact Unicode class table.
EXTRA=""
[ "${FFZ_COMPACT:-0}" = "1" ] && EXTRA="-DFFZ_COMPACT_CLASS"

# The in-process crash handler is compiled in for the LOCATABLE (selfdebug)
# build, or when FFZ_CRASH_IN_RELEASE=1. It needs .eh_frame unwind tables across
# the whole library so _Unwind_Backtrace can walk the stack — that is what
# inflates the stripped arm64 .so from ~32 KB to ~58 KB. Plain release leaves it
# out (pure ~32 KB) and relies on the OS tombstone + the .debug sidecar instead.
CRASH=0
[ "${FFZ_SELFDEBUG:-0}" = "1" ] && CRASH=1
[ "${FFZ_CRASH_IN_RELEASE:-0}" = "1" ] && CRASH=1
CRASH_DEF=""; CRASH_LIBS=""; CRASH_CF=""
if [ "$CRASH" = "1" ]; then
  CRASH_DEF="-DFFZ_HAVE_CRASH_HANDLER"
  CRASH_LIBS="-llog -ldl"               # logcat + dladdr symbol names
  CRASH_CF="-funwind-tables -fno-omit-frame-pointer"
fi

if [ "${FFZ_SELFDEBUG:-0}" = "1" ]; then
  # LOCATABLE dev/profile build: symbols kept in the .so (no strip, no hidden
  # visibility), asserts on. Native errors AND hard crashes are pinpointed
  # in-process (func+offset via dladdr; map to line with addr2line). Bigger.
  CFLAGS="-std=c11 -O1 -g -fPIC $CRASH_CF \
-ffunction-sections -fdata-sections $CRASH_DEF $EXTRA"
  LDFLAGS="-shared -Wl,--gc-sections $CRASH_LIBS"
else
  # COMPRESSED release: -Oz (clang), LTO, section GC, hidden visibility (only the
  # ffz_ffi_* symbols stay exported), stripped. -g debuginfo is split to a
  # libffz.so.debug sidecar (gnu-debuglink) so crash offsets in the stripped .so
  # can still be symbolized offline. NOT linked -s.
  CFLAGS="-std=c11 -Oz -flto -g -fPIC -DNDEBUG -fvisibility=hidden $CRASH_CF \
-ffunction-sections -fdata-sections $CRASH_DEF $EXTRA"
  LDFLAGS="-shared -flto -Wl,--gc-sections -Wl,--exclude-libs,ALL $CRASH_LIBS"
fi

root="$(pwd)"
for entry in $TARGETS; do
  abi="${entry%%:*}"; target="${entry##*:}"
  out="build/android/$abi"; mkdir -p "$out"
  tmp="$(mktemp -d)"
  # Always compile the FFI shim; add ffz_crash.c only when the handler is on.
  if [ "$CRASH" = "1" ]; then FFI_SRC="$root/ffi/*.c"; else FFI_SRC="$root/ffi/ffz_ffi.c"; fi
  ( cd "$tmp" && "$CLANG" --target="$target" $CFLAGS -I"$root/include" \
      -c "$root"/src/*.c $FFI_SRC )
  "$CLANG" --target="$target" $CFLAGS $LDFLAGS -o "$out/libffz.so" "$tmp"/*.o
  if [ "${FFZ_SELFDEBUG:-0}" != "1" ]; then
    # release: split debug symbols → ship the stripped .so, keep .debug sidecar
    "$OBJCOPY" --only-keep-debug "$out/libffz.so" "$out/libffz.so.debug" 2>/dev/null || true
    "$STRIP" --strip-all "$out/libffz.so"
    "$OBJCOPY" --add-gnu-debuglink="$out/libffz.so.debug" "$out/libffz.so" 2>/dev/null || true
  fi
  rm -rf "$tmp"
  printf "%-14s %8s B   %s/libffz.so\n" "$abi" "$(stat -c %s "$out/libffz.so")" "$out"
done
if [ "${FFZ_SELFDEBUG:-0}" = "1" ]; then
  echo "done — LOCATABLE (-O1 -g, symbols kept, NOT stripped). Bigger; crashes pinpointed in-process."
else
  echo "done — COMPRESSED (exact + -Oz + FFI-only, stripped + libffz.so.debug sidecar)."
fi
