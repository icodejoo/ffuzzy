//! Rust benchmark: the identical two-pass parallel filter as the C bench, but
//! powered by nucleo-matcher. Only the matching engine differs.
//!   perf_rust <dataset> <queryfile> <mode> <threads> <index 0|1> <withIdx 0|1>
//! Prints: rust,<mode>,<threads>,<index>,<n>,<nq>,<ms/filter>,<Mitems/s>
use std::fs;
use std::time::Instant;

use nucleo_matcher::pattern::{AtomKind, CaseMatching, Normalization, Pattern};
use nucleo_matcher::{Config, Matcher, Utf32Str, Utf32String};

const TOPK: usize = 50;
const MIN_SECONDS: f64 = 0.4;

enum Data {
    On(Vec<Utf32String>),  // index on: pre-segmented
    Off(Vec<String>),      // index off: convert per query
}

#[inline]
fn score_item(d: &Data, i: usize, buf: &mut Vec<char>, m: &mut Matcher,
              pat: &Pattern) -> Option<u32> {
    match d {
        Data::On(v) => pat.score(v[i].slice(..), m),
        Data::Off(v) => pat.score(Utf32Str::new(&v[i], buf), m),
    }
}

fn build_pattern(q: &str, mode: &str) -> Pattern {
    let (cm, nm) = (CaseMatching::Smart, Normalization::Smart);
    match mode {
        "prefix" => Pattern::new(q, cm, nm, AtomKind::Prefix),
        "substring" => Pattern::new(q, cm, nm, AtomKind::Substring),
        "word" => Pattern::new(q, cm, nm, AtomKind::Exact),
        _ => Pattern::parse(q, cm, nm),
    }
}

fn filter(d: &Data, n: usize, q: &str, mode: &str, nthreads: usize,
          with_indices: bool) -> i64 {
    let pat = build_pattern(q, mode);
    let mut scored: Vec<(u32, u32)> = if nthreads <= 1 {
        let mut m = Matcher::new(Config::DEFAULT);
        let mut buf = Vec::new();
        let mut out = Vec::new();
        for i in 0..n {
            if let Some(s) = score_item(d, i, &mut buf, &mut m, &pat) {
                out.push((i as u32, s));
            }
        }
        out
    } else {
        let chunk = (n + nthreads - 1) / nthreads;
        let parts: Vec<Vec<(u32, u32)>> = std::thread::scope(|s| {
            let mut hs = Vec::new();
            for t in 0..nthreads {
                let lo = t * chunk;
                if lo >= n { break; }
                let hi = (lo + chunk).min(n);
                let pr = &pat;
                hs.push(s.spawn(move || {
                    let mut m = Matcher::new(Config::DEFAULT);
                    let mut buf = Vec::new();
                    let mut out = Vec::new();
                    for i in lo..hi {
                        if let Some(sc) = score_item(d, i, &mut buf, &mut m, pr) {
                            out.push((i as u32, sc));
                        }
                    }
                    out
                }));
            }
            hs.into_iter().map(|h| h.join().unwrap()).collect()
        });
        parts.into_iter().flatten().collect()
    };

    // top-K by (score desc, index asc): partial select then sort the K kept,
    // mirroring the C bench's bounded heap (avoids a full O(n log n) sort).
    let cmp = |a: &(u32, u32), b: &(u32, u32)| b.1.cmp(&a.1).then(a.0.cmp(&b.0));
    let mut sink = scored.len() as i64;
    if scored.len() > TOPK {
        scored.select_nth_unstable_by(TOPK - 1, cmp);
        scored.truncate(TOPK);
    }
    scored.sort_by(cmp);
    if with_indices {
        let mut m = Matcher::new(Config::DEFAULT);
        let mut buf = Vec::new();
        let mut idx = Vec::new();
        for &(i, _) in scored.iter().take(TOPK) {
            idx.clear();
            match d {
                Data::On(v) => { pat.indices(v[i as usize].slice(..), &mut m, &mut idx); }
                Data::Off(v) => {
                    let hay = Utf32Str::new(&v[i as usize], &mut buf);
                    pat.indices(hay, &mut m, &mut idx);
                }
            }
            idx.sort_unstable();
            idx.dedup();
            sink += idx.len() as i64;
        }
    }
    sink
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 7 {
        eprintln!("usage: perf_rust data queries mode threads index withIndices");
        std::process::exit(2);
    }
    let items: Vec<String> =
        fs::read_to_string(&a[1]).unwrap().lines().map(|s| s.to_string()).collect();
    let queries: Vec<String> =
        fs::read_to_string(&a[2]).unwrap().lines().map(|s| s.to_string()).collect();
    let mode = a[3].clone();
    let nthreads: usize = a[4].parse().unwrap();
    let index_on: i32 = a[5].parse().unwrap();
    let with_indices = a[6] != "0";

    let n = items.len();
    let data = if index_on != 0 {
        Data::On(items.iter().map(|s| Utf32String::from(s.as_str())).collect())
    } else {
        Data::Off(items)
    };

    let mut sink: i64 = 0;
    for q in &queries {
        sink += filter(&data, n, q, &mode, nthreads, with_indices);
    }

    let t0 = Instant::now();
    let mut filters = 0i64;
    let mut elapsed;
    loop {
        for q in &queries {
            sink += filter(&data, n, q, &mode, nthreads, with_indices);
        }
        filters += queries.len() as i64;
        elapsed = t0.elapsed().as_secs_f64();
        if elapsed >= MIN_SECONDS { break; }
    }
    let ms_per_filter = elapsed * 1000.0 / filters as f64;
    let mitems_s = n as f64 * filters as f64 / elapsed / 1e6;
    println!("rust,{},{},{},{},{},{:.3},{:.1}", mode, nthreads, index_on, n,
             queries.len(), ms_per_filter, mitems_s);
    eprintln!("sink={sink}");
}
