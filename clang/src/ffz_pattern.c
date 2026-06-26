// Pattern / Atom layer: parses query syntax (`! ^ ' $`, escaped whitespace),
// splits words into atoms, and normalizes each needle. Port of nucleo
// `pattern.rs` at codepoint granularity.
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
        v->cap = v->cap ? v->cap * 2 : 16;
        v->d = (uint32_t *)realloc(v->d, v->cap * sizeof(uint32_t));
    }
    v->d[v->len++] = c;
}

// Decode one UTF-8 codepoint at *pi (within [0,n)); advances *pi. Invalid -> U+FFFD.
static uint32_t next_cp(const uint8_t *s, size_t n, size_t *pi) {
    size_t i = *pi;
    uint8_t b0 = s[i];
    uint32_t cp;
    size_t need;
    if (b0 < 0x80) { *pi = i + 1; return b0; }
    else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; need = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; need = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; need = 3; }
    else { *pi = i + 1; return 0xFFFD; }
    if (i + need >= n) { *pi = i + 1; return 0xFFFD; }
    for (size_t k = 1; k <= need; k++) {
        uint8_t bk = s[i + k];
        if ((bk & 0xC0) != 0x80) { *pi = i + 1; return 0xFFFD; }
        cp = (cp << 6) | (bk & 0x3F);
    }
    *pi = i + need + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    return cp;
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
    while (i < n) {
        uint32_t c = next_cp(s, n, &i);
        if (escape_ws) {
            if (saw_backslash) {
                if (c == ' ') { v.d[v.len - 1] = ' '; saw_backslash = false; continue; }
                // not an escaped space: keep the backslash already pushed
            }
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
    // else keep codepoints.
    bool ascii = true;
    for (size_t i = 0; i < v.len; i++)
        if (v.d[i] >= 0x80) { ascii = false; break; }
    if (ascii) {
        a->nb = (uint8_t *)malloc(v.len ? v.len : 1);
        for (size_t i = 0; i < v.len; i++) a->nb[i] = (uint8_t)v.d[i];
        a->nu = NULL;
        free(v.d);
    } else {
        a->nu = v.d;
        a->nb = NULL;
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
    if (p->n == ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 4;
        p->atoms = (ffz_atom *)realloc(p->atoms, ctx->cap * sizeof(ffz_atom));
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
    build_ctx ctx = {p, 0, cm, nm, kind, literal};
    for_each_word(query, n, emit_atom, &ctx);
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
    if (p->n == 0) return 0;
    int32_t total = 0;
    for (size_t i = 0; i < p->n; i++) {
        const ffz_atom *a = &p->atoms[i];
        m->cfg.ignore_case = a->ignore_case;
        m->cfg.normalize = a->normalize;
        ffz_str needle = {a->nb, a->nu, a->needle_len};
        if (a->negative) {
            int32_t s = ffz_match(m, haystack, needle, a->kind, NULL);
            if (s >= 0) return -1;  // negative atom matched -> reject
        } else {
            int32_t s = ffz_match(m, haystack, needle, a->kind, out);
            if (s < 0) return -1;
            total += s;
        }
    }
    return total;
}
