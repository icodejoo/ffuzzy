#!/usr/bin/env bash
# Release Android .so build — LOCKED CONFIG: exact mode (full Unicode, byte-
# identical to nucleo) + -O2 + FFI-only exports (ffz.map) + stripped/gc-sections.
# Produces one libffz.so per ABI under build/android/<abi>/.
#
# Usage: ANDROID_NDK=/path/to/ndk bash scripts/build_android.sh [api]
set -euo pipefail
cd "$(dirname "$0")/.."

NDK="${ANDROID_NDK:-/d/sdk/android/ndk/27.2.12479018}"
API="${1:-21}"
HOST=windows-x86_64           # change to linux-x86_64 / darwin-x86_64 off-Windows
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
CLANG="$BIN/clang.exe"; [ -x "$CLANG" ] || CLANG="$BIN/clang"
SIZE="$BIN/llvm-size.exe";  [ -x "$SIZE" ] || SIZE="$BIN/llvm-size"

# Space-separated "abi:clang-target-triple" pairs (no bash arrays — POSIX sh).
TARGETS="arm64-v8a:aarch64-linux-android$API \
armeabi-v7a:armv7a-linux-androideabi$API \
x86_64:x86_64-linux-android$API \
x86:i686-linux-android$API"
# Smallest size: -Oz (clang), LTO, section GC, hidden visibility (only the
# ffz_ffi_* shim symbols carry visibility("default") and stay exported), strip.
# Set FFZ_COMPACT=1 to drop the ~5 KB exact Unicode class table.
EXTRA=""
[ "${FFZ_COMPACT:-0}" = "1" ] && EXTRA="-DFFZ_COMPACT_CLASS"
CFLAGS="-std=c11 -Oz -flto -fPIC -DNDEBUG -fvisibility=hidden \
-ffunction-sections -fdata-sections $EXTRA"
LDFLAGS="-shared -flto -Wl,--gc-sections -Wl,--exclude-libs,ALL -s"

root="$(pwd)"
for entry in $TARGETS; do
  abi="${entry%%:*}"; target="${entry##*:}"
  out="build/android/$abi"; mkdir -p "$out"
  tmp="$(mktemp -d)"
  # The Dart binding calls ffz_ffi_*, so the shim MUST be compiled & exported.
  ( cd "$tmp" && "$CLANG" --target="$target" $CFLAGS -I"$root/include" \
      -c "$root"/src/*.c "$root"/ffi/ffz_ffi.c )
  "$CLANG" --target="$target" $CFLAGS $LDFLAGS -o "$out/libffz.so" "$tmp"/*.o
  rm -rf "$tmp"
  printf "%-14s %8s B   %s/libffz.so\n" "$abi" "$(stat -c %s "$out/libffz.so")" "$out"
done
echo "done — exact + -O2 + FFI-only, stripped."
