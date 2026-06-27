#!/usr/bin/env bash
# 在「极致压缩档」与「稳妥 600KB 档」之间切换原生库的编译配置。
#
#   bash scripts/size-profile.sh extreme   # 极致压缩(panic=abort + nightly build-std + immediate-abort)
#   bash scripts/size-profile.sh safe      # 稳妥(原始配置,~600KB,无 nightly/build-std,panic 走默认展开)
#   bash scripts/size-profile.sh status    # 查看当前档位
#
# 切换只改两处提交内文件:rust/Cargo.toml 的 panic 行、rust/cargokit.yaml 的 cargo 段。
# workflow 会自动探测 cargokit.yaml 里是否有生效的 build-std 来决定加不加 RUSTFLAGS。
#
# ⚠️ 改了配置 = crate-hash 变。切换后必须:提交 → 推送 → 重跑 Precompile 工作流 → 发新版本。
#    使用者按 hash 下载二进制,两套配置无法对同一个发布版本同时生效。
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cargo_toml="$root/rust/Cargo.toml"
ck="$root/rust/cargokit.yaml"

is_extreme() { grep -qE '^[[:space:]]+- "-Zbuild-std' "$ck"; }

status() {
  if is_extreme; then echo "当前档位:extreme(极致压缩)"; else echo "当前档位:safe(稳妥 ~600KB)"; fi
}

to_safe() {
  # 注释 Cargo.toml 的 panic 行(带 size-profile:panic 标记)
  sed -i -E 's|^panic = "abort"([[:space:]]+# size-profile:panic.*)|# panic = "abort"\1|' "$cargo_toml"
  # 注释 cargokit.yaml 标记区间内的非标记行
  sed -i -E '/size-profile:buildstd:begin/,/size-profile:buildstd:end/{/size-profile:buildstd:/b; /^#/b; s|^|# |}' "$ck"
}

to_extreme() {
  # 反注释 Cargo.toml 的 panic 行
  sed -i -E 's|^# panic = "abort"([[:space:]]+# size-profile:panic.*)|panic = "abort"\1|' "$cargo_toml"
  # 反注释 cargokit.yaml 标记区间内的行(去掉一层 "# ")
  sed -i -E '/size-profile:buildstd:begin/,/size-profile:buildstd:end/{/size-profile:buildstd:/b; s|^# ||}' "$ck"
}

case "${1:-status}" in
  extreme) to_extreme; status ;;
  safe)    to_safe;    status ;;
  status)  status ;;
  *) echo "用法: bash scripts/size-profile.sh [extreme|safe|status]"; exit 1 ;;
esac
