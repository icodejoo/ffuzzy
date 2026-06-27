#!/usr/bin/env bash
# Build the lite Android .so files (--no-default-features) and place them
# in android/src/lite/jniLibs/<abi>/ for use with --dart-define=FFUZZY_LITE=true.
#
# Prerequisites:
#   - Android NDK in $ANDROID_NDK_HOME or detected via $ANDROID_SDK_ROOT
#   - Rust toolchain with Android targets installed:
#       rustup target add aarch64-linux-android armv7-linux-androideabi x86_64-linux-android
#   - cargo-ndk: cargo install cargo-ndk
#
# Usage:
#   bash scripts/build_lite_android.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUST_DIR="${REPO_ROOT}/rust"
OUT_BASE="${REPO_ROOT}/android/src/lite/jniLibs"

ABI_TARGETS=(
    "aarch64-linux-android:arm64-v8a"
    "armv7-linux-androideabi:armeabi-v7a"
    "x86_64-linux-android:x86_64"
)

echo "==> Building ffuzzy lite (--no-default-features) for Android"
echo "    Rust dir: ${RUST_DIR}"
echo "    Output:   ${OUT_BASE}"
echo ""

for entry in "${ABI_TARGETS[@]}"; do
    target="${entry%%:*}"
    abi="${entry##*:}"
    out_dir="${OUT_BASE}/${abi}"
    mkdir -p "${out_dir}"

    echo "--> ${target} (${abi})"
    (
        cd "${RUST_DIR}"
        cargo ndk \
            --target "${target}" \
            --platform 21 \
            -- build --release --no-default-features
    )

    so_src="${RUST_DIR}/target/${target}/release/librust_lib_ffuzzy.so"
    if [[ ! -f "${so_src}" ]]; then
        echo "ERROR: expected output not found: ${so_src}" >&2
        exit 1
    fi
    cp "${so_src}" "${out_dir}/librust_lib_ffuzzy.so"
    size_kb=$(( $(wc -c < "${out_dir}/librust_lib_ffuzzy.so") / 1024 ))
    echo "    OK: ${out_dir}/librust_lib_ffuzzy.so (${size_kb} KB)"
done

echo ""
echo "==> Lite binaries ready. Build your app with:"
echo "    flutter build apk --dart-define=FFUZZY_LITE=true --target-platform android-arm64 --shrink"
