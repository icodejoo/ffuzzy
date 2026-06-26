#!/usr/bin/env bash
# Differential test: C ffz_pattern vs Rust nucleo-matcher Pattern.
# Requires gcc + cargo on PATH. Run from the difftest/ directory.
set -euo pipefail
cd "$(dirname "$0")"

echo "[1/4] building Rust oracle + regenerating exact class table..."
cargo build --release --quiet
cargo run --release --quiet --bin gen_class_table -- ../src/ffz_class_table.c

echo "[2/4] building C harness (strict: exact class + nucleo substring bugcompat)..."
gcc -std=c11 -O2 -Wall -Wextra -DFFZ_NUCLEO_SUBSTRING_BUGCOMPAT -I../include \
    ../src/*.c difftest_c.c -o difftest_c

echo "[3/4] running both over $(wc -l < queries.txt) queries x $(wc -l < corpus.txt) haystacks..."
./target/release/difftest_rust . > out_rust.txt
./difftest_c . > out_c.txt

echo "[4/4] comparing..."
total=$(wc -l < out_rust.txt)
if diff -u out_rust.txt out_c.txt > diff.txt; then
    echo "PASS: all $total (query,haystack) pairs identical (score + indices)."
    rm -f diff.txt
else
    mism=$(grep -c '^[-+][0-9]' diff.txt || true)
    echo "FAIL: differences found. First 40 diff lines:"
    head -40 diff.txt
    echo "(full diff in difftest/diff.txt; ~$mism changed lines)"
    exit 1
fi
