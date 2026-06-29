// Pattern / Atom layer: parses query syntax (`! ^ ' $`, escaped whitespace),
// splits words into atoms, and normalizes each needle. Port of nucleo
// `pattern.rs` at codepoint granularity.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ffz_alloc.h"
#include "ffz_internal.h"

struct ffz_atom {
    uint8_t *nb;    // needle as ASCII bytes (exclusive with nu)
    uint32_t *nu;   // needle as codepoints
    size_t needle_len;
    ffz_mode kind;
    bool negative;
    bool ignore_case;
    bool normalize;
};

struct ffz_pattern {
    ffz_atom *atoms;
    size_t n;
};

// --- a tiny growable codepoint vector ------------------------------------
typedef struct { uint32_t *d; size_t len, cap; } cpvec;
static void cpvec_push(cpvec *v, uint32_t c) {
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 16;
        uint32_t *d = (uint32_t *)realloc(v->d, ncap * sizeof(uint32_t));
        if (!d) return;  // OOM: drop (needle truncated, never crashes)
        v->d = d;
        v->cap = ncap;
    }
    v->d[v->len++] = c;
}

// Build a normalized atom from a raw UTF-8 slice.
static void atom_build(ffz_atom *a, const char *raw, size_t n,
                       ffz_case_matching cm, ffz_normalization nm,
                       ffz_mode kind, bool escape_ws, bool append_dollar) {
    bool ignore_case = (cm == FFZ_CASE_IGNORE) ||
                       (cm == FFZ_CASE_SMART);  // refined below for Smart
    bool normalize = (nm == FFZ_NORM_SMART);

    cpvec v = {0};
    const uint8_t *s = (const uint8_t *)raw;
    size_t i = 0;
    bool saw_backslash = false;
    size_t backslash_idx = 0;
    while (i < n) {
        uint32_t c = ffz_decode_cp(s, n, &i);
        if (escape_ws) {
            if (saw_backslash) {
                if (c == ' ') {
                    // Replace the backslash (only if it was successfully pushed).
                    if (v.len > backslash_idx) v.d[backslash_idx] = ' ';
                    saw_backslash = false;
                    continue;
                }
            }
            if (c == '\\') backslash_idx = v.len;  // record before push
            saw_backslash = (c == '\\');
        }
        // case handling
        switch (cm) {
            case FFZ_CASE_IGNORE: c = ffz_cp_to_lower(c); break;
            case FFZ_CASE_SMART:
                if (ffz_cp_is_upper(c)) ignore_case = false;
                break;
            case FFZ_CASE_RESPECT: ignore_case = false; break;
        }
        // normalization smart detection (do not fold the needle itself)
        if (nm == FFZ_NORM_SMART && ffz_cp_has_normalize(c)) normalize = false;
        cpvec_push(&v, c);
    }
    if (append_dollar) cpvec_push(&v, '$');

    // Pack as ASCII bytes when possible (enables the matcher's SIMD path),
    // else keep codepoints. Invariant on exit: needle_len > 0 => nb or nu is
    // non-NULL (so the matcher never dereferences a NULL needle buffer).
    bool ascii = true;
    for (size_t i = 0; i < v.len; i++)
        if (v.d[i] >= 0x80) { ascii = false; break; }
    if (ascii) {
        a->nb = (uint8_t *)malloc(v.len ? v.len : 1);
        if (a->nb) {
            for (size_t i = 0; i < v.len; i++) a->nb[i] = (uint8_t)v.d[i];
        } else {
            v.len = 0;  // OOM: degrade to an empty atom (dropped by emit_atom)
        }
        a->nu = NULL;
        free(v.d);
    } else {
        a->nu = v.d;       // valid for [0,v.len); NULL only if v.len == 0
        a->nb = NULL;
        if (!a->nu) v.len = 0;
    }
    a->needle_len = v.len;
    a->kind = kind;
    a->negative = false;
    a->ignore_case = ignore_case;
    a->normalize = normalize;
}

// Parse a single atom honoring `! ^ ' $` syntax.
static void atom_parse(ffz_atom *a, const char *raw, size_t n,
                       ffz_case_matching cm, ffz_normalization nm) {
    size_t off = 0, len = n;
    bool invert = false;
    // leading ! (or \! to escape)
    if (len >= 1 && raw[off] == '!') { invert = true; off++; len--; }
    else if (len >= 2 && raw[off] == '\\' && raw[off + 1] == '!') { off++; len--; }

    ffz_mode kind = FFZ_FUZZY;
    if (len >= 1 && raw[off] == '^') { kind = FFZ_PREFIX; off++; len--; }
    else if (len >= 1 && raw[off] == '\'') { kind = FFZ_SUBSTRING; off++; len--; }
    else if (len >= 2 && raw[off] == '\\' &&
             (raw[off + 1] == '^' || raw[off + 1] == '\'')) { off++; len--; }

    bool append_dollar = false;
    if (len >= 2 && raw[off + len - 2] == '\\' && raw[off + len - 1] == '$') {
        append_dollar = true; len -= 2;
    } else if (len >= 1 && raw[off + len - 1] == '$') {
        kind = (kind == FFZ_FUZZY) ? FFZ_POSTFIX : FFZ_EXACT;
        len -= 1;
    }
    if (invert && kind == FFZ_FUZZY) kind = FFZ_SUBSTRING;

    atom_build(a, raw + off, len, cm, nm, kind, true, append_dollar);
    a->negative = invert;
}

// --- word splitting (unescaped whitespace separates atoms) ----------------
typedef void (*atom_emit)(void *ud, const char *word, size_t wlen);

static void for_each_word(const char *p, size_t n, atom_emit emit, void *ud) {
    size_t start = 0;
    bool saw_backslash = false;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == ' ' && !saw_backslash) {
            if (i > start) emit(ud, p + start, i - start);
            start = i + 1;
            saw_backslash = false;
        } else {
            saw_backslash = (c == '\\');
        }
    }
    if (n > start) emit(ud, p + start, n - start);
}

typedef struct {
    ffz_pattern *p;
    size_t cap;
    ffz_case_matching cm;
    ffz_normalization nm;
    ffz_mode forced_kind;
    bool literal;  // true => ffz_pattern_new (no syntax parsing)
} build_ctx;

static void emit_atom(void *ud, const char *word, size_t wlen) {
    build_ctx *ctx = (build_ctx *)ud;
    ffz_pattern *p = ctx->p;
    if (p->n >= FFZ_MAX_ATOMS) return;  // DoS guard: drop atoms past the cap
    if (p->n == ctx->cap) {
        size_t ncap = ctx->cap ? ctx->cap * 2 : 4;
        ffz_atom *na = (ffz_atom *)realloc(p->atoms, ncap * sizeof(ffz_atom));
        if (!na) return;  // OOM: drop this atom rather than deref NULL
        p->atoms = na;
        ctx->cap = ncap;
    }
    ffz_atom *a = &p->atoms[p->n];
    if (ctx->literal)
        atom_build(a, word, wlen, ctx->cm, ctx->nm, ctx->forced_kind, true, false);
    else
        atom_parse(a, word, wlen, ctx->cm, ctx->nm);
    if (a->needle_len == 0) { free(a->nb); free(a->nu); return; }  // drop empty
    p->n++;
}

static ffz_pattern *build(const char *query, size_t n, ffz_case_matching cm,
                          ffz_normalization nm, bool literal, ffz_mode kind) {
    ffz_pattern *p = (ffz_pattern *)calloc(1, sizeof(*p));
    if (!p) return NULL;  // OOM: callers treat NULL as "no pattern"
    build_ctx ctx = {p, 0, cm, nm, kind, literal};
    if (literal) {
        /* Non-fuzzy modes (exact/prefix/postfix/substring): treat the full query
         * as ONE atom — spaces are part of the literal, not term separators.
         * e.g. exact("Super Gems 1000") must equal "Super Gems 1000", not match
         * three separate atoms. */
        emit_atom(&ctx, query, n);
    } else {
        for_each_word(query, n, emit_atom, &ctx);
    }
    return p;
}

ffz_pattern *ffz_pattern_parse(const char *query, size_t n,
                               ffz_case_matching cm, ffz_normalization nm) {
    return build(query, n, cm, nm, false, FFZ_FUZZY);
}

ffz_pattern *ffz_pattern_new(const char *query, size_t n, ffz_case_matching cm,
                             ffz_normalization nm, ffz_mode kind) {
    return build(query, n, cm, nm, true, kind);
}

void ffz_pattern_free(ffz_pattern *p) {
    if (!p) return;
    for (size_t i = 0; i < p->n; i++) {
        free(p->atoms[i].nb);
        free(p->atoms[i].nu);
    }
    free(p->atoms);
    free(p);
}

int32_t ffz_pattern_match(ffz_matcher *m, const ffz_pattern *p,
                          ffz_str haystack, ffz_indices *out) {
    if (!p || p->n == 0) return 0;
    bool saved_ic = m->cfg.ignore_case;
    bool saved_nm = m->cfg.normalize;
    int64_t total64 = 0;
    for (size_t i = 0; i < p->n; i++) {
        const ffz_atom *a = &p->atoms[i];
        m->cfg.ignore_case = a->ignore_case;
        m->cfg.normalize = a->normalize;
        ffz_str needle = {a->nb, a->nu, a->needle_len};
        if (a->negative) {
            int32_t s = ffz_match(m, haystack, needle, a->kind, NULL);
            if (s >= 0) {
                m->cfg.ignore_case = saved_ic;
                m->cfg.normalize   = saved_nm;
                return -1;  // negative atom matched -> reject
            }
        } else {
            int32_t s = ffz_match(m, haystack, needle, a->kind, out);
            if (s < 0) {
                m->cfg.ignore_case = saved_ic;
                m->cfg.normalize   = saved_nm;
                return -1;
            }
            total64 += s;
        }
    }
    m->cfg.ignore_case = saved_ic;
    m->cfg.normalize   = saved_nm;
    int32_t result = total64 > INT32_MAX ? INT32_MAX
                   : total64 < INT32_MIN ? INT32_MIN
                   : (int32_t)total64;
    return result;
}
