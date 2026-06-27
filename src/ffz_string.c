// UTF-8 decoding, config constructors, and the small growable containers.
#include <stdlib.h>
#include <string.h>

#include "ffz_alloc.h"
#include "ffz_internal.h"

// --- config ---------------------------------------------------------------
static const uint8_t DELIM_DEFAULT[] = {'/', ',', ':', ';', '|'};
static const uint8_t DELIM_PATHS[] = {'/', '\\'};

// Precompute the class of every ASCII byte so classification is a table load.
static void fill_ascii_class(ffz_config *c) {
    for (int x = 0; x < 128; x++) {
        ffz_char_class k;
        if (x >= 'a' && x <= 'z') k = FFZ_CLASS_LOWER;
        else if (x >= 'A' && x <= 'Z') k = FFZ_CLASS_UPPER;
        else if (x >= '0' && x <= '9') k = FFZ_CLASS_NUMBER;
        // ASCII whitespace per Rust's u8::is_ascii_whitespace: space, \t, \n,
        // \f, \r — NOTE \v (0x0B) is excluded, matching nucleo.
        else if (x == ' ' || x == '\t' || x == '\n' || x == '\f' || x == '\r')
            k = FFZ_CLASS_WHITESPACE;
        else {
            k = FFZ_CLASS_NONWORD;
            for (size_t i = 0; i < c->delimiter_len; i++)
                if ((int)c->delimiter_chars[i] == x) { k = FFZ_CLASS_DELIMITER; break; }
        }
        c->ascii_class[x] = (uint8_t)k;
    }
}

ffz_config ffz_config_default(void) {
    ffz_config c;
    c.delimiter_chars = DELIM_DEFAULT;
    c.delimiter_len = sizeof(DELIM_DEFAULT);
    c.bonus_boundary_white = FFZ_BONUS_BOUNDARY + 2;
    c.bonus_boundary_delimiter = FFZ_BONUS_BOUNDARY + 1;
    c.initial_char_class = FFZ_CLASS_WHITESPACE;
    c.normalize = true;
    c.ignore_case = true;
    c.prefer_prefix = false;
    fill_ascii_class(&c);
    c.scoring_mode = FFZ_SCORE_FAST;
    return c;
}

ffz_config ffz_config_match_paths(void) {
    ffz_config c = ffz_config_default();
    c.delimiter_chars = DELIM_PATHS;
    c.delimiter_len = sizeof(DELIM_PATHS);
    c.bonus_boundary_white = FFZ_BONUS_BOUNDARY;
    c.initial_char_class = FFZ_CLASS_DELIMITER;
    fill_ascii_class(&c);
    c.scoring_mode = FFZ_SCORE_FAST;
    return c;
}

// --- indices --------------------------------------------------------------
void ffz_indices_push(ffz_indices *ix, uint32_t v) {
    if (ix->len == ix->cap) {
        size_t ncap = ix->cap ? ix->cap * 2 : 32;
        uint32_t *d = (uint32_t *)realloc(ix->data, ncap * sizeof(uint32_t));
        if (!d) return;  // OOM: drop rather than deref NULL (no overflow check
        ix->data = d;    // needed: ncap is bounded by needle_len <= 2048)
        ix->cap = ncap;
    }
    ix->data[ix->len++] = v;
}

void ffz_indices_clear(ffz_indices *ix) { ix->len = 0; }

void ffz_indices_free(ffz_indices *ix) {
    free(ix->data);
    ix->data = NULL;
    ix->len = ix->cap = 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

void ffz_indices_sort_dedup(ffz_indices *ix) {
    if (ix->len < 2) return;
    qsort(ix->data, ix->len, sizeof(uint32_t), cmp_u32);
    size_t w = 1;
    for (size_t r = 1; r < ix->len; r++)
        if (ix->data[r] != ix->data[w - 1]) ix->data[w++] = ix->data[r];
    ix->len = w;
}

// --- UTF-8 decode (codepoint level; invalid bytes -> U+FFFD) ---------------
static void buf_push(ffz_str_buf *buf, uint32_t cp) {
    if (buf->len == buf->cap) {
        size_t ncap = buf->cap ? buf->cap * 2 : 32;
        uint32_t *d = (uint32_t *)realloc(buf->cp, ncap * sizeof(uint32_t));
        if (!d) return;  // OOM: drop the codepoint rather than deref NULL
        buf->cp = d;
        buf->cap = ncap;
    }
    buf->cp[buf->len++] = cp;
}

// Index of the first byte >= 0x80 in p[0..n), or n if all-ASCII. SIMD on x86.
static size_t first_non_ascii(const uint8_t *p, size_t n) {
    size_t i = 0;
#if defined(FFZ_SSE2)
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
        unsigned m = (unsigned)_mm_movemask_epi8(x);  // high bit = byte >= 0x80
        if (m) return i + (size_t)ffz_ctz32(m);
    }
#endif
    for (; i < n; i++)
        if (p[i] >= 0x80) return i;
    return n;
}

// Decode one UTF-8 codepoint (shared by the haystack and pattern decoders).
uint32_t ffz_decode_cp(const uint8_t *s, size_t n, size_t *pos) {
    size_t i = *pos;
    uint8_t b0 = s[i];
    uint32_t cp;
    size_t need;
    if (b0 < 0x80) { *pos = i + 1; return b0; }
    else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; need = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; need = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; need = 3; }
    else { *pos = i + 1; return 0xFFFD; }
    if (i + need >= n) { *pos = i + 1; return 0xFFFD; }  // truncated
    for (size_t k = 1; k <= need; k++) {
        uint8_t bk = s[i + k];
        if ((bk & 0xC0) != 0x80) { *pos = i + 1; return 0xFFFD; }
        cp = (cp << 6) | (bk & 0x3F);
    }
    *pos = i + need + 1;
    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
        (need == 3 && cp < 0x10000)) cp = 0xFFFD;  // overlong encoding
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;  // out-of-range/surrogate
    return cp;
}

ffz_str ffz_str_from_utf8(const char *s, size_t n, ffz_str_buf *buf) {
    const uint8_t *p = (const uint8_t *)s;
    // Fast path: all-ASCII text is used as-is (the bytes ARE the codepoints),
    // enabling SIMD memchr in the matcher and avoiding any decode/copy.
    if (first_non_ascii(p, n) == n) {
        ffz_str out = {p, NULL, n};
        return out;
    }
    buf->len = 0;
    size_t i = 0;
    while (i < n) buf_push(buf, ffz_decode_cp(p, n, &i));
    ffz_str out = {NULL, buf->cp, buf->len};
    return out;
}

void ffz_str_buf_free(ffz_str_buf *buf) {
    free(buf->cp);
    buf->cp = NULL;
    buf->len = buf->cap = 0;
}
