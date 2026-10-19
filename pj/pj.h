// pj — parallel shape-tape JSON parser.
//
// Layout: one 64-bit word per value (NaN-boxed). Doubles are stored raw; every
// other value lives in the quiet-NaN space JSON can never produce. Objects do
// not store keys: they point to an interned "shape" (hidden class, as in JS
// engines) holding the key sequence, so {"x0":1,"y0":2,"x1":3,"y1":4} is
// 5 words (40 B) vs 9 yyjson vals (144 B) or 14 simdjson tape words (112 B).
// Key lookup with a pj_qkey caches (shape -> slot): one compare + one load.
//
// The input is split into chunks parsed in parallel; chunks write disjoint
// regions of one tape linked by JUMP words, and a tiny sequential merge
// patches containers that span chunks.
#ifndef PJ_H
#define PJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PJ_PADDING 64 // readable bytes required past the end of the input

enum {
    PJ_TAG_SSTR  = 0x7FF8, // + len (0..6): short string inline in the low 48 bits
    PJ_TAG_REF   = 0x7FFF, // string in the input buffer: payload (offset << 16 | len)
    PJ_TAG_CONST = 0xFFF8, // payload 0 null, 1 false, 2 true
    PJ_TAG_INT   = 0xFFF9, // 48-bit signed int
    PJ_TAG_BIG   = 0xFFFA, // -> pj_big
    PJ_TAG_STR   = 0xFFFB, // -> {u32 len; char s[len]; '\0'}
    PJ_TAG_ARR   = 0xFFFC, // payload end index; next word = count; items follow
    PJ_TAG_OBJF  = 0xFFFD, // flat object: payload (n<<42 | shape>>6); n scalar values follow
    PJ_TAG_OBJD  = 0xFFFE, // deep object: payload end index; tape[end-1] is an OBJF shape word
    PJ_TAG_JUMP  = 0xFFFF, // continue at payload index (chunk seam)
};

typedef enum { PJ_NULL, PJ_FALSE, PJ_TRUE, PJ_NUM, PJ_STR, PJ_ARR, PJ_OBJ, PJ_BAD } pj_type;

#define PJ_PAYLOAD(w) ((w) & 0xFFFFFFFFFFFFull)
#define PJ_TAG(w)     ((uint32_t)((w) >> 48))
#define PJ_BOXED(w)   ((((w) >> 48) & 0x7FFF) >= 0x7FF8)
#define PJ_SHAPE_MAX  63 // keys per shape: n fits 6 bits, shape>>6 fits 42 bits (48-bit address spaces)
#define PJ_INLINE     static inline __attribute__((always_inline))

typedef struct pj_key { uint64_t pre; uint32_t len, hash; const char *s; uint32_t plain, _r; } pj_key; // plain: no '\\' in key

typedef struct pj_shape {
    // first cache line: what the parser reads to predict the next key
    struct pj_shape *last;         // parse-time transition cache
    uint32_t plen, pm;             // pat bytes to match ('"' key '"' ':'), as a length and a bit mask; plen 2 = no pattern
    uint8_t pat[32];               // last's key as it appears in the input, zero-padded
    const pj_key *key;             // last key
    uint32_t n, nkids;
    // second line
    struct pj_shape *parent;
    struct pj_shape *kids, *sib;   // parse-time children list (first PJ_KIDS_MAX children)
    const pj_key *keys;            // n keys, inline
    const uint8_t *ht;             // open-addressed (pre,len) -> slot table, 0xFF = empty
    uint32_t hmask, _r2;
    struct pj_shape *cfirst;       // parse-time: first-key shape of the last object value seen after this shape's key
    // third line: this shape's own key as a pattern (what a parent's plen/pm/pat hold when last == this)
    uint32_t inplen, inpm;
    uint8_t inpat[32];
} __attribute__((aligned(64))) pj_shape;

static inline uint32_t pj__khash(uint64_t pre, uint32_t len) { return (uint32_t)(((pre ^ len) * 0x9E3779B97F4A7C15ull) >> 40); }

typedef struct { uint8_t neg; uint8_t is_u64; uint64_t u; } pj_big;
typedef struct { const char *s; size_t len; } pj_str;
typedef uint64_t pj_val; // tape index

typedef struct pj_doc {
    const uint64_t *tape;
    pj_val root;
    const char *input; // strings without escapes point here: keep the input alive while using the doc
    void *priv;
} pj_doc;

typedef struct { int code; size_t pos; const char *msg; } pj_err;

// buf needs PJ_PADDING readable bytes after len and must outlive the doc (strings reference it).
// nthreads: 1 = calling thread only, otherwise all cores (thread pool).
pj_doc *pj_parse(const char *buf, size_t len, int nthreads, pj_err *err);
pj_doc *pj_parse_file(const char *path, int nthreads, pj_err *err);
void pj_free(pj_doc *d);
const char *pj_arch(void); // SIMD layer compiled in: "neon", "avx2", "avx512", "avx512vbmi", "scalar"

// ---- accessors -------------------------------------------------------------

static inline const pj_shape *pj__shape(uint64_t w) { return (const pj_shape *)((w & ((1ull << 42) - 1)) << 6); }
static inline uint32_t pj__shape_n(uint64_t w) { return (uint32_t)(w >> 42) & 63; }

PJ_INLINE uint64_t pj__skip(const uint64_t *t, uint64_t p) {
    uint64_t w = t[p];
    switch (PJ_TAG(w)) {
    case PJ_TAG_ARR: case PJ_TAG_OBJD: return PJ_PAYLOAD(w);
    case PJ_TAG_OBJF: return p + 1 + pj__shape_n(w);
    case PJ_TAG_JUMP: return p; // never a value; makes pj_iter_next's size check fail
    default: return p + 1;
    }
}
// Follow chunk-seam JUMPs, never past `end` (the container's end index may itself hold a JUMP).
PJ_INLINE uint64_t pj__fix(const uint64_t *t, uint64_t p, uint64_t end) {
    while (__builtin_expect(p < end && PJ_TAG(t[p]) == PJ_TAG_JUMP, 0)) p = PJ_PAYLOAD(t[p]);
    return p;
}

static inline pj_val pj_root(const pj_doc *d) { return d->root; }

PJ_INLINE pj_type pj_typeof(const pj_doc *d, pj_val v) {
    uint64_t w = d->tape[v];
    if (!PJ_BOXED(w)) return PJ_NUM;
    uint32_t t = PJ_TAG(w);
    if (t < 0x8000) return PJ_STR;
    switch (t) {
    case PJ_TAG_CONST: return (pj_type)PJ_PAYLOAD(w);
    case PJ_TAG_INT: case PJ_TAG_BIG: return PJ_NUM;
    case PJ_TAG_STR: return PJ_STR;
    case PJ_TAG_ARR: return PJ_ARR;
    case PJ_TAG_OBJF: case PJ_TAG_OBJD: return PJ_OBJ;
    default: return PJ_BAD;
    }
}

PJ_INLINE double pj_get_num(const pj_doc *d, pj_val v) {
    uint64_t w = d->tape[v];
    double r;
    if (__builtin_expect(!PJ_BOXED(w), 1)) { memcpy(&r, &w, 8); return r; }
    if (PJ_TAG(w) == PJ_TAG_INT) return (double)((int64_t)(w << 16) >> 16);
    if (PJ_TAG(w) == PJ_TAG_BIG) {
        const pj_big *b = (const pj_big *)PJ_PAYLOAD(w);
        return b->neg ? -(double)b->u : (double)b->u;
    }
    return 0;
}

PJ_INLINE pj_str pj_get_str(const pj_doc *d, pj_val v) {
    uint64_t w = d->tape[v];
    uint32_t t = PJ_TAG(w);
    if (t >= PJ_TAG_SSTR && t <= PJ_TAG_SSTR + 6) return (pj_str){(const char *)&d->tape[v], t - PJ_TAG_SSTR};
    if (t == PJ_TAG_REF) return (pj_str){d->input + (PJ_PAYLOAD(w) >> 16), (size_t)(w & 0xFFFF)};
    if (t == PJ_TAG_STR) { const uint32_t *h = (const uint32_t *)PJ_PAYLOAD(w); return (pj_str){(const char *)(h + 1), *h}; }
    return (pj_str){0, 0};
}

static inline size_t pj_arr_size(const pj_doc *d, pj_val a) { return PJ_TAG(d->tape[a]) == PJ_TAG_ARR ? d->tape[a + 1] : 0; }

// Array iterator. It predicts that each element has the same size as the previous
// one, so the next position does not wait on loading the current element; the
// prediction is verified by a (well-predicted) branch.
typedef struct { const uint64_t *t; uint64_t pos, end, stride; } pj_iter;

PJ_INLINE bool pj_arr_iter(const pj_doc *d, pj_val a, pj_iter *it) {
    uint64_t w = d->tape[a];
    it->t = d->tape; it->stride = 1;
    if (PJ_TAG(w) != PJ_TAG_ARR) { it->pos = it->end = 0; return false; }
    it->pos = a + 2; it->end = PJ_PAYLOAD(w);
    return true;
}
__attribute__((noinline)) uint64_t pj__iter_resync(pj_iter *it, uint64_t p);
#ifndef PJ_ITER_PF
#if defined(__x86_64__)
#define PJ_ITER_PF 256 // words ahead to prefetch: Intel's iteration is latency-bound without it (2.2x)
#else
#define PJ_ITER_PF 0   // Apple's prefetchers do better alone
#endif
#endif
PJ_INLINE bool pj_iter_next(pj_iter *it, pj_val *out) {
    uint64_t p = it->pos;
    if (p >= it->end) return false;
    if (PJ_ITER_PF) __builtin_prefetch(&it->t[p + PJ_ITER_PF]);
    uint64_t nx = p + it->stride;
    if (__builtin_expect(pj__skip(it->t, p) != nx, 0)) {
        p = pj__iter_resync(it, p); // follows seams, updates stride
        if (p >= it->end) return false;
        nx = p + it->stride;
    }
    *out = p;
    it->pos = nx;
    return true;
}

// Query key: build once, reuse across lookups (caches shape -> slot).
typedef struct { uint64_t pre, inl; uint32_t len; const char *s; const pj_shape *cs; int32_t slot; } pj_qkey;

static inline uint64_t pj__lenmask(size_t n) { return n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1); }
PJ_INLINE pj_qkey pj_qkey_make(const char *s) {
    pj_qkey k; size_t n = strlen(s); uint64_t pre = 0;
    memcpy(&pre, s, n < 8 ? n : 8);
    k.pre = pre; k.len = (uint32_t)n; k.s = s; k.cs = 0; k.slot = -1;
    k.inl = n <= 6 ? (((uint64_t)(PJ_TAG_SSTR + n) << 48) | pre) : 0;
    return k;
}
PJ_INLINE int pj__shape_find(const pj_shape *sh, const pj_qkey *k) {
    for (uint32_t i = 0; i < sh->n; i++) {
        const pj_key *x = &sh->keys[i];
        if (x->pre == k->pre && x->len == k->len && (k->len <= 8 || !memcmp(x->s + 8, k->s + 8, k->len - 8))) return (int)i;
    }
    return -1;
}
bool pj__obj_get_deep(const pj_doc *d, pj_val o, pj_qkey *k, pj_val *out);

PJ_INLINE bool pj_obj_get_k(const pj_doc *d, pj_val o, pj_qkey *k, pj_val *out) {
    uint64_t w = d->tape[o];
    if (__builtin_expect(PJ_TAG(w) == PJ_TAG_OBJF, 1)) {
        const pj_shape *sh = pj__shape(w);
        if (__builtin_expect(sh != k->cs, 0)) { k->cs = sh; k->slot = pj__shape_find(sh, k); }
        if (k->slot < 0) return false;
        *out = o + 1 + (uint32_t)k->slot;
        return true;
    }
    return pj__obj_get_deep(d, o, k, out);
}
// Lookup by C string (yyjson_obj_get-style API): per-shape hash on the key's
// first 8 bytes and length, then a verified match.
PJ_INLINE bool pj_obj_get(const pj_doc *d, pj_val o, const char *key, pj_val *out) {
    pj_qkey k = pj_qkey_make(key);
    uint64_t w = d->tape[o];
    if (__builtin_expect(PJ_TAG(w) == PJ_TAG_OBJF, 1)) {
        const pj_shape *sh = pj__shape(w);
        for (uint32_t i = pj__khash(k.pre, k.len);; i++) {
            uint32_t s = sh->ht[i & sh->hmask];
            if (s == 0xFF) return false;
            const pj_key *x = &sh->keys[s];
            if (x->pre == k.pre && x->len == k.len && (k.len <= 8 || !memcmp(x->s + 8, key + 8, k.len - 8))) { *out = o + 1 + s; return true; }
        }
    }
    return pj__obj_get_deep(d, o, &k, out);
}

typedef struct { const pj_doc *d; const pj_shape *sh; uint32_t i; uint64_t pos, end; } pj_obj_iter;
bool pj_obj_iter_init(const pj_doc *d, pj_val o, pj_obj_iter *it);
bool pj_obj_iter_next(pj_obj_iter *it, pj_str *key, pj_val *val);
size_t pj_obj_size(const pj_doc *d, pj_val o);

#endif
