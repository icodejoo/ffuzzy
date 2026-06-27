//! Reference oracle: runs nucleo-matcher's Pattern over every (query, haystack)
//! pair and prints one line per pair in a format the C harness reproduces:
//!
//!   <qi> <hi> MISS
//!   <qi> <hi> <score>|<i0,i1,...>      (indices sorted + deduped)
//!
//! Config::DEFAULT + CaseMatching::Smart + Normalization::Smart, matching the
//! C side's ffz_config_default() + FFZ_CASE_SMART + FFZ_NORM_SMART.
use std::fs;
use std::io::{BufWriter, Write};

use nucleo_matcher::pattern::{CaseMatching, Normalization, Pattern};
use nucleo_matcher::{Config, Matcher, Utf32Str};

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| ".".to_string());
    let queries = fs::read_to_string(format!("{dir}/queries.txt")).unwrap();
    let corpus = fs::read_to_string(format!("{dir}/corpus.txt")).unwrap();
    let queries: Vec<&str> = queries.lines().collect();
    let haystacks: Vec<&str> = corpus.lines().collect();

    let mut matcher = Matcher::new(Config::DEFAULT);
    let stdout = std::io::stdout();
    let mut w = BufWriter::new(stdout.lock());
    let mut buf = Vec::new();

    for (qi, q) in queries.iter().enumerate() {
        let pat = Pattern::parse(q, CaseMatching::Smart, Normalization::Smart);
        for (hi, h) in haystacks.iter().enumerate() {
            buf.clear();
            let hs = Utf32Str::new(h, &mut buf);
            let mut idx = Vec::new();
            match pat.indices(hs, &mut matcher, &mut idx) {
                Some(score) => {
                    idx.sort_unstable();
                    idx.dedup();
                    let istr: Vec<String> = idx.iter().map(|i| i.to_string()).collect();
                    writeln!(w, "{qi} {hi} {score}|{}", istr.join(",")).unwrap();
                }
                None => writeln!(w, "{qi} {hi} MISS").unwrap(),
            }
        }
    }
}
