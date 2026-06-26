#!/usr/bin/env python3
"""Generate range+offset-compressed Unicode tables for the C matcher.

Reads nucleo-matcher's `normalize.rs` (Latin diacritic stripping) and
`case_fold.rs` (simple case folding) and emits `ffz_unicode_tables.c`.

Compression: consecutive sorted keys whose target differs by a *constant*
offset are merged into one run `(start, end, offset)`. The script reconstructs
every original pair from the runs and asserts equality, so the compression is
provably lossless even though we cannot compile the C here.

Usage:
    python gen_unicode_tables.py <nucleo_src_dir> <out_c_file>
nucleo_src_dir defaults to the cargo registry copy used by this project.
"""
import re
import sys
import os

DEFAULT_NUCLEO = (
    r"C:/Users/Administrator/.cargo/registry/src/"
    r"index.crates.io-1949cf8c6b5b557f/nucleo-matcher-0.3.1/src/chars"
)


def parse_rust_char(tok: str) -> int:
    """Parse a Rust char literal body (between the single quotes) to a codepoint."""
    m = re.fullmatch(r"\\u\{([0-9A-Fa-f]+)\}", tok)
    if m:
        return int(m.group(1), 16)
    if tok.startswith("\\"):
        # escaped ascii like \\ or \' — not expected in these tables
        esc = {"\\\\": ord("\\"), "\\'": ord("'")}
        return esc[tok]
    # a single (possibly astral) character
    assert len(tok) == 1, f"unexpected char token: {tok!r}"
    return ord(tok)


PAIR_RE = re.compile(r"\(\s*'((?:\\u\{[0-9A-Fa-f]+\})|(?:\\.)|.)'\s*,\s*"
                     r"'((?:\\u\{[0-9A-Fa-f]+\})|(?:\\.)|.)'\s*\)")


def parse_pairs(text: str):
    pairs = []
    for m in PAIR_RE.finditer(text):
        k = parse_rust_char(m.group(1))
        v = parse_rust_char(m.group(2))
        pairs.append((k, v))
    return pairs


def extract_table_blocks(text: str):
    """Return only the pair-array literal bodies, excluding doc comments / machinery.

    Matches both `... [(char, char); N] = [ ... ];` and
    `... [(char, char)] = &[ ... ];` forms.
    """
    blocks = re.findall(r"\[\(char,\s*char\)(?:;\s*\d+)?\]\s*=\s*&?\[(.*?)\];",
                        text, re.DOTALL)
    # strip `// ...` line comments so apostrophes in comment text (e.g.
    # "WOMAN'S ...") can't be mis-parsed as char-pair literals.
    stripped = [re.sub(r"//[^\n]*", "", b) for b in blocks]
    return "\n".join(stripped)


def compress(pairs):
    """pairs: list of (key, target). Returns list of runs (start, end, offset)."""
    pairs = sorted(set(pairs), key=lambda p: p[0])
    # detect duplicate keys with conflicting targets
    seen = {}
    for k, v in pairs:
        if k in seen and seen[k] != v:
            raise SystemExit(f"conflicting target for U+{k:04X}: {seen[k]} vs {v}")
        seen[k] = v
    pairs = sorted(seen.items())
    runs = []
    i = 0
    n = len(pairs)
    while i < n:
        k0, v0 = pairs[i]
        off = v0 - k0
        j = i + 1
        while j < n:
            k, v = pairs[j]
            if k == pairs[j - 1][0] + 1 and (v - k) == off:
                j += 1
            else:
                break
        runs.append((k0, pairs[j - 1][0], off))
        i = j
    # verify lossless reconstruction
    rebuilt = []
    for s, e, o in runs:
        for c in range(s, e + 1):
            rebuilt.append((c, c + o))
    assert rebuilt == pairs, "compression is not lossless!"
    return runs, pairs


def emit_casefold(runs):
    """runs: (start,end,offset). Dict-pack: unique offsets -> u8 index, span u8.
    Emits start[] (u32), span[] (u8, =end-start), offidx[] (u8), off[] (i32)."""
    uniq = sorted(set(o for _, _, o in runs))
    idx = {o: i for i, o in enumerate(uniq)}
    assert len(uniq) <= 256, "offset dict exceeds u8 index"
    assert all((e - s) <= 255 for s, e, _ in runs), "run span exceeds u8"

    def col(name, ty, vals, fmt):
        out = [f"const {ty} {name}[] = {{"]
        line = "    "
        for v in vals:
            cell = fmt(v) + ", "
            if len(line) + len(cell) > 96:
                out.append(line.rstrip()); line = "    "
            line += cell
        if line.strip():
            out.append(line.rstrip())
        out.append("};")
        return "\n".join(out)

    parts = [
        col("ffz_casefold_start", "uint32_t", [s for s, _, _ in runs],
            lambda v: "0x%X" % v),
        col("ffz_casefold_span", "uint8_t", [e - s for s, e, _ in runs],
            lambda v: "%d" % v),
        col("ffz_casefold_offidx", "uint8_t", [idx[o] for _, _, o in runs],
            lambda v: "%d" % v),
        col("ffz_casefold_off", "int32_t", uniq, lambda v: "%d" % v),
        "const size_t ffz_casefold_start_len = "
        "sizeof(ffz_casefold_start)/sizeof(ffz_casefold_start[0]);",
    ]
    return "\n".join(parts)


def emit_flat(keys_name, vals_name, pairs):
    """Emit two parallel arrays: u32 keys (sorted) and u8 ascii targets."""
    out = [f"const uint32_t {keys_name}[] = {{"]
    line = "    "
    for (k, _v) in pairs:
        cell = "0x%X, " % k
        if len(line) + len(cell) > 96:
            out.append(line.rstrip()); line = "    "
        line += cell
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    out.append(f"const uint8_t {vals_name}[] = {{")
    line = "    "
    for (_k, v) in pairs:
        cell = "0x%X, " % v
        if len(line) + len(cell) > 96:
            out.append(line.rstrip()); line = "    "
        line += cell
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    out.append(f"const size_t {keys_name}_len = "
               f"sizeof({keys_name})/sizeof({keys_name}[0]);")
    return "\n".join(out)


def main():
    nucleo = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_NUCLEO
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "..", "src", "ffz_unicode_tables.c")

    with open(os.path.join(nucleo, "normalize.rs"), encoding="utf-8") as f:
        norm_text = f.read()
    with open(os.path.join(nucleo, "case_fold.rs"), encoding="utf-8") as f:
        fold_text = f.read()

    # Parse ONLY the pair-array literal bodies (excludes doc-comment asserts
    # and the const-fn lookup machinery in normalize.rs).
    norm_pairs = parse_pairs(extract_table_blocks(norm_text))
    fold_pairs = parse_pairs(extract_table_blocks(fold_text))

    # normalize targets must be ASCII (the C side treats normalize result as <128)
    bad = [(k, v) for k, v in norm_pairs if v >= 0x80]
    assert not bad, f"non-ascii normalize targets: {bad[:5]}"
    # drop the trivial ascii A-Z -> a-z entries from casefold (handled by an
    # arithmetic fast-path in C); keep the table to non-ASCII keys only.
    fold_pairs = [(k, v) for k, v in fold_pairs if k >= 0x80]

    # normalize: scattered ASCII targets -> store flat (u32 key + u8 target);
    # casefold: block-structured offsets -> range+offset runs compress well.
    _, norm_flat = compress(norm_pairs)
    fold_runs, fold_flat = compress(fold_pairs)
    max_off = max(abs(o) for _, _, o in fold_runs)
    assert max_off < (1 << 31), "casefold offset overflows i32"

    header = """// AUTO-GENERATED by tools/gen_unicode_tables.py — DO NOT EDIT.
// Source: nucleo-matcher 0.3.1 (normalize.rs + case_fold.rs).
// normalize: flat (key,ascii-target); casefold: range+offset runs.
// Regenerate with:  python tools/gen_unicode_tables.py
#include "ffz_unicode.h"
"""
    body = "\n\n".join([
        header,
        "// Latin diacritic stripping (e.g. U+00C0 'A-grave' -> 'A'). ASCII targets.",
        emit_flat("ffz_normalize_keys", "ffz_normalize_vals", norm_flat),
        "// Simple case folding (non-ASCII). Dict-packed: parallel start/span/"
        "offidx arrays + a small unique-offset dictionary.",
        emit_casefold(fold_runs),
    ]) + "\n"

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)

    def kb(n):
        return f"{n/1024:.1f}KB"

    n_uniq = len(set(o for _, _, o in fold_runs))
    norm_bytes = len(norm_flat) * 5   # u32 key + u8 target
    fold_bytes = len(fold_runs) * 6 + n_uniq * 4  # start u32 + span u8 + idx u8 + dict
    print(f"normalize: {len(norm_flat)} pairs, flat ~{kb(norm_bytes)}")
    print(f"casefold : {len(fold_flat)} pairs -> {len(fold_runs)} runs, "
          f"{n_uniq} uniq offsets, dict-packed ~{kb(fold_bytes)}")
    print(f"total table data: ~{kb(norm_bytes+fold_bytes)}  "
          f"(max casefold offset {max_off})")
    print(f"written: {os.path.abspath(out_path)}")


if __name__ == "__main__":
    main()
