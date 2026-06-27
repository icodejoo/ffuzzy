#!/usr/bin/env python3
"""Format perf results.csv into side-by-side markdown tables.

CSV row: group,engine,mode,threads,index,n,nq,ms_per_filter,Mitems_s
Pairs the c/rust rows sharing (group,mode,threads,index,n) and shows the
speedup = rust_ms / c_ms  (>1 => C faster).
"""
import sys
from collections import defaultdict

rows = defaultdict(dict)  # key -> {engine: (ms, mitems)}
order = []
for line in open(sys.argv[1], encoding="utf-8"):
    p = line.strip().split(",")
    if len(p) != 9:
        continue
    grp, eng, mode, thr, idx, n, nq, ms, mit = p
    key = (grp, mode, int(thr), int(idx), int(n))
    if key not in rows:
        order.append(key)
    rows[key][eng] = (float(ms), float(mit))


def fmt_table(title, keys, cols):
    print(f"\n### {title}\n")
    print("| " + " | ".join(cols) + " |")
    print("|" + "|".join("---" for _ in cols) + "|")
    for k in keys:
        grp, mode, thr, idx, n = k
        c = rows[k].get("c")
        r = rows[k].get("rust")
        if not c or not r:
            continue
        speed = r[0] / c[0] if c[0] else 0
        idxs = "on" if idx else "off"
        row = {
            "mode": mode, "threads": thr, "index": idxs, "N": n,
            "C ms": f"{c[0]:.2f}", "Rust ms": f"{r[0]:.2f}",
            "C Mitem/s": f"{c[1]:.0f}", "Rust Mitem/s": f"{r[1]:.0f}",
            "C vs Rust": f"{speed:.2f}x",
        }
        print("| " + " | ".join(str(row[c2]) for c2 in cols) + " |")


main = [k for k in order if k[0] == "main"]
cjk = [k for k in order if k[0] == "cjk"]
sonly = [k for k in order if k[0] == "scoreonly"]
size = [k for k in order if k[0].startswith("size")]

print("# nucleo-matcher (Rust) vs ffz (C) — same harness, same data")
print("\n`C vs Rust` = Rust_ms / C_ms per filter; **>1 means C is faster**.")

fmt_table("Main matrix (ASCII corpus, score+top-50 indices)", main,
          ["mode", "threads", "index", "C ms", "Rust ms", "C Mitem/s",
           "Rust Mitem/s", "C vs Rust"])
fmt_table("Charset: fuzzy on CJK corpus", cjk,
          ["threads", "index", "C ms", "Rust ms", "C vs Rust"])
fmt_table("Score-only (no indices), fuzzy ASCII index-on, 1 thread", sonly,
          ["mode", "threads", "C ms", "Rust ms", "C vs Rust"])
fmt_table("Corpus-size scaling (fuzzy, index-on)",
          sorted(size, key=lambda k: (k[4], k[2])),
          ["N", "threads", "C ms", "Rust ms", "C Mitem/s", "Rust Mitem/s",
           "C vs Rust"])
