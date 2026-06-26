#!/usr/bin/env bash
# Performance comparison: nucleo-matcher (Rust) vs ffz (C), identical harness.
# Requires gcc + cargo + python on PATH. Run from perf/.
set -euo pipefail
cd "$(dirname "$0")"

N="${1:-200000}"
echo "[setup] generating datasets (N=$N)..."
python gen_data.py "$N" >/dev/null

PTHREAD=""
[ "${OS:-}" = "Windows_NT" ] || PTHREAD="-pthread"

echo "[build] C bench (unity -O3, matching Rust's single-crate + LTO inlining)..."
gcc -std=c11 -O3 $PTHREAD -I../include perf_unity.c -o perf_c
echo "[build] Rust bench..."
cargo build --release --quiet
RUST=./target/release/perf_rust

NCPU=$(python -c "import os;print(os.cpu_count() or 2)")
HALF=$(python -c "import os;print(max(1,(os.cpu_count() or 2)//2))")
echo "[info] logical CPUs=$NCPU, multi-thread setting=$HALF"

CSV=results.csv
: > "$CSV"
run() { # group dataset queries mode threads index withidx
  local grp=$1 data=$2 q=$3 mode=$4 thr=$5 idx=$6 wi=$7
  echo "$grp,$(./perf_c   "$data" "$q" "$mode" "$thr" "$idx" "$wi" 2>/dev/null)" >> "$CSV"
  echo "$grp,$($RUST     "$data" "$q" "$mode" "$thr" "$idx" "$wi" 2>/dev/null)" >> "$CSV"
}

echo "[run] main matrix: 4 modes x {1,$HALF} threads x {index on,off} (ASCII, +indices)"
for mode in fuzzy prefix substring word; do
  q="q_${mode}.txt"
  for thr in 1 "$HALF"; do
    for idx in 1 0; do
      run main data_ascii.txt "$q" "$mode" "$thr" "$idx" 1
    done
  done
done

echo "[run] charset: fuzzy on CJK corpus (index on, +indices)"
for thr in 1 "$HALF"; do
  run cjk data_cjk.txt q_cjk.txt fuzzy "$thr" 1 1
done

echo "[run] score-only vs +indices: fuzzy ASCII index-on, 1 thread"
run scoreonly data_ascii.txt q_fuzzy.txt fuzzy 1 1 0

echo "[run] size scaling: fuzzy ASCII index-on {1,$HALF} threads"
for n in 50000 1000000; do
  python gen_data.py "$n" >/dev/null
  for thr in 1 "$HALF"; do
    run "size$n" data_ascii.txt q_fuzzy.txt fuzzy "$thr" 1 1
  done
done
python gen_data.py "$N" >/dev/null  # restore main N

echo
python fmt.py "$CSV"
