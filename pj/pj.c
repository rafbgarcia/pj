// pj — parallel shape-tape JSON parser. See pj.h for the layout.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // clock_gettime, newlocale/strtod_l, getpagesize under -std=c11
#endif
#include "pj.h"

#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#include <xlocale.h>
#endif

#include "pow5.h"

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define INLINE      static inline __attribute__((always_inline))

#define MAX_DEPTH   1024
#define MAX_THREADS 64
#define CHUNK_BYTES (2u << 20)
#define MIN_PARALLEL (256u << 10)
#define SPEC_MIN    (16u << 20)

static inline uint64_t W(uint32_t tag, uint64_t payload) { return ((uint64_t)tag << 48) | payload; }
static inline uint64_t load8(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t load4(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
INLINE bool is_digit(uint8_t c) { return (uint8_t)(c - '0') < 10; }
// '-' or a digit, as one bit test (bytes 64 apart alias, so callers must still validate).
#define NUM_START(c) ((0x1FF9ull >> ((uint8_t)((c) - '-') & 63)) & 1)

// Digit-gather index table used by dig_value: for n digits with n1 before the '.',
// output lane j (of 32, right-aligned) takes digit k = j - (32 - n) from source byte
// k + (k >= n1), skipping the '.'; 0xFF = zero lane. Built once.
static uint8_t g_dig_idx[20][20][32] __attribute__((aligned(64)));

// UTF-8 lookup tables (Keiser & Lemire, as in simdjson).
enum { TOO_SHORT = 1 << 0, TOO_LONG = 1 << 1, OVERLONG_3 = 1 << 2, TOO_LARGE = 1 << 3, SURROGATE = 1 << 4,
       OVERLONG_2 = 1 << 5, TOO_LARGE_1000 = 1 << 6, OVERLONG_4 = 1 << 6, TWO_CONTS = 1 << 7,
       CARRY = TOO_SHORT | TOO_LONG | TWO_CONTS };
static const uint8_t U8_B1H[16] __attribute__((unused)) = {TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG,
    TWO_CONTS, TWO_CONTS, TWO_CONTS, TWO_CONTS, TOO_SHORT | OVERLONG_2, TOO_SHORT, TOO_SHORT | OVERLONG_3 | SURROGATE,
    TOO_SHORT | TOO_LARGE | TOO_LARGE_1000 | OVERLONG_4};
static const uint8_t U8_B1L[16] __attribute__((unused)) = {CARRY | OVERLONG_3 | OVERLONG_2 | OVERLONG_4, CARRY | OVERLONG_2, CARRY, CARRY,
    CARRY | TOO_LARGE, CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
    CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000 | SURROGATE, CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000};
static const uint8_t U8_B2H[16] __attribute__((unused)) = {TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT,
    TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE_1000 | OVERLONG_4, TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE,
    TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE, TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE,
    TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT};

// ---- SIMD layer: one of these, chosen at compile time ----------------------------
// Each provides: blk64 (64-byte block) with blk_load/blk_eq/blk_term; str_mask/nonws_mask
// (SCANW bytes, 1 << MSHIFT bits per byte); key_eq32; dig32 with dig_scan/dig_n2/dig_value;
// u8chk with u8_init/u8_block/u8_ok; cpu_relax.
#if defined(PJ_FORCE_SCALAR)
#include "arch_scalar.h"
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include "arch_neon.h"
#elif defined(__x86_64__) && defined(__AVX2__)
#include "arch_x86.h"
#else
#include "arch_scalar.h"
#endif

const char *pj_arch(void) { return PJ_ARCH_NAME; }

// ---- platform ------------------------------------------------------------------

uint64_t tnow(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
static int ncpu_online(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }

// ---- thread pool -------------------------------------------------------------

typedef void (*job_fn)(void *ctx, int worker, int item);

static struct {
    int n;
    pthread_t th[MAX_THREADS];
    pthread_mutex_t mu, busy;
    pthread_cond_t cv;
    _Atomic uint64_t gen;
    _Atomic int next, pending;
    int nitems;
    job_fn fn;
    void *ctx;
    pthread_once_t once;
} P = {.mu = PTHREAD_MUTEX_INITIALIZER, .busy = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .once = PTHREAD_ONCE_INIT};

static void run_items(int w) {
    for (;;) {
        int i = atomic_fetch_add_explicit(&P.next, 1, memory_order_relaxed);
        if (i >= P.nitems) break;
        P.fn(P.ctx, w, i);
    }
}

static void *worker_main(void *arg) {
    int w = (int)(intptr_t)arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    uint64_t seen = 0;
    for (;;) {
        uint64_t g;
        int spins = 0;
        while ((g = atomic_load_explicit(&P.gen, memory_order_acquire)) == seen) {
            if (++spins < 200000) { cpu_relax(); continue; }
            pthread_mutex_lock(&P.mu);
            while ((g = atomic_load_explicit(&P.gen, memory_order_acquire)) == seen) pthread_cond_wait(&P.cv, &P.mu);
            pthread_mutex_unlock(&P.mu);
            break;
        }
        seen = g;
        run_items(w);
        atomic_fetch_sub_explicit(&P.pending, 1, memory_order_release);
    }
    return 0;
}

static void pool_init(void) {
    int ncpu = ncpu_online();
    const char *e = getenv("PJ_THREADS");
    if (e && atoi(e) > 0) ncpu = atoi(e);
    if (ncpu > MAX_THREADS) ncpu = MAX_THREADS;
    if (ncpu < 1) ncpu = 1;
    P.n = ncpu;
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    for (int w = 1; w < P.n; w++) pthread_create(&P.th[w], &at, worker_main, (void *)(intptr_t)w);
}

static int pool_size(void) { pthread_once(&P.once, pool_init); return P.n; }

// Runs fn(ctx, worker, item) for item in [0, nitems) on all workers; caller is worker 0.
static void pool_run(job_fn fn, void *ctx, int nitems) {
    P.fn = fn; P.ctx = ctx; P.nitems = nitems;
    atomic_store_explicit(&P.next, 0, memory_order_relaxed);
    atomic_store_explicit(&P.pending, P.n - 1, memory_order_relaxed);
    pthread_mutex_lock(&P.mu);
    atomic_fetch_add_explicit(&P.gen, 1, memory_order_release);
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);
    run_items(0);
    while (atomic_load_explicit(&P.pending, memory_order_acquire) != 0) cpu_relax();
}

// ---- arenas ------------------------------------------------------------------

typedef struct blk { struct blk *next; } blk;

typedef struct {
    uint8_t *cur, *end;
    blk *blocks;
    size_t bytes;
} arena;

static void *arena_alloc(arena *a, size_t n, size_t align) {
    uintptr_t p = ((uintptr_t)a->cur + align - 1) & ~(uintptr_t)(align - 1);
    if (UNLIKELY(!a->cur || p + n > (uintptr_t)a->end)) {
        size_t bs = n + 128 > (64 << 10) ? n + 128 : (64 << 10);
        blk *b = aligned_alloc(64, (bs + 63) & ~(size_t)63);
        b->next = a->blocks; a->blocks = b; a->bytes += bs;
        a->cur = (uint8_t *)b + 64; a->end = (uint8_t *)b + bs;
        p = ((uintptr_t)a->cur + align - 1) & ~(uintptr_t)(align - 1);
    }
    a->cur = (uint8_t *)(p + n);
    return (void *)p;
}

static void arena_free(arena *a) {
    for (blk *b = a->blocks, *n; b; b = n) { n = b->next; free(b); }
    a->blocks = 0; a->cur = a->end = 0; a->bytes = 0;
}

// ---- shapes (per worker) -----------------------------------------------------

typedef struct {
    arena ar;
    uint64_t *bm; size_t bmcap; // bitmap scratch: per 64-byte block {terminators, non-whitespace}
    pj_shape *root;
    pj_shape **tab;
    uint32_t cap, cnt;
} wctx;

// len-byte equality of an interned key (>= 32 readable bytes) and input bytes (padded).
INLINE bool key_eq(const char *a, const uint8_t *b, uint32_t len) {
    if (len <= 8) return ((load8(a) ^ load8(b)) & pj__lenmask(len)) == 0;
    if (len <= 32) return key_eq32((const uint8_t *)a, b, len);
    return !memcmp(a, b, len);
}

static inline uint64_t mix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)(a ^ 0x9E3779B97F4A7C15ull) * (b ^ 0xD6E8FEB86659FD93ull);
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}
static uint32_t key_hash(const uint8_t *s, uint32_t len, uint64_t pre) {
    uint64_t h = mix(pre, len);
    for (uint32_t i = 8; i < len; i += 8) h = mix(h, load8(s + i) & pj__lenmask(len - i));
    return (uint32_t)h;
}

// Builds the per-shape lookup table over keys[0..n): open-addressed (pre, len) -> slot.
static void shape_ht(arena *ar, pj_shape *s) {
    uint32_t hs = 8;
    while (hs < 2 * s->n) hs *= 2;
    uint8_t *ht = arena_alloc(ar, hs, 8);
    memset(ht, 0xFF, hs);
    for (uint32_t i = 0; i < s->n; i++) {
        uint32_t j = pj__khash(s->keys[i].pre, s->keys[i].len);
        while (ht[j & (hs - 1)] != 0xFF) {
            if (!memcmp(s->keys[ht[j & (hs - 1)]].s, s->keys[i].s, s->keys[i].len) && s->keys[ht[j & (hs - 1)]].len == s->keys[i].len) break; // duplicate key: keep first
            j++;
        }
        if (ht[j & (hs - 1)] == 0xFF) ht[j & (hs - 1)] = (uint8_t)i;
    }
    s->hmask = hs - 1;
    __atomic_store_n(&s->ht, ht, __ATOMIC_RELEASE);
}

// A parse-time shape only knows its parent and last key. The key array and lookup table that
// accessors use are built once, when an object actually ends with this shape: objects with n
// keys create n prefix shapes, and copying keys into each of them was O(n^2).
static __attribute__((noinline)) void shape_final(arena *ar, pj_shape *s) {
    pj_key *ks = arena_alloc(ar, s->n * sizeof(pj_key), 8);
    uint32_t i = s->n;
    for (const pj_shape *x = s; i; x = x->parent) ks[--i] = *x->key;
    s->keys = ks;
    shape_ht(ar, s);
}
#define SHAPE_FINAL(ar, s) do { if (UNLIKELY(!(s)->ht)) shape_final((ar), (s)); } while (0)

static void set_inpat(pj_shape *c);
static pj_shape *new_shape(wctx *w, pj_shape *parent, const pj_key *key) {
    pj_shape *s = arena_alloc(&w->ar, sizeof(pj_shape), 64);
    memset(s, 0, sizeof *s);
    s->plen = 2; s->pm = 3; s->pat[0] = s->pat[1] = 0xFF;
    s->parent = parent;
    s->n = parent ? parent->n + 1 : 0;
    if (s->n) {
        pj_key *k = arena_alloc(&w->ar, sizeof(pj_key), 8);
        *k = *key;
        s->key = k;
        set_inpat(s);
    } else {
        shape_ht(&w->ar, s); // root: final from the start
    }
    return s;
}

static void wctx_init(wctx *w) {
    memset(w, 0, sizeof *w);
    w->root = new_shape(w, 0, 0);
    w->cap = 256;
    w->tab = calloc(w->cap, sizeof(pj_shape *));
}

// Worker contexts (shapes, their predictions, the shape table and bitmap scratch) outlive the doc
// that used them: pj_free returns them here and the next parse starts with warm shapes, as
// simdjson's parser keeps its buffers. The doc that pointed into them is gone, so nothing is
// shared. A context whose arena grew past WCACHE_BYTES (documents with ever-new keys) is freed.
#ifndef PJ_WCACHE
#define PJ_WCACHE 1
#endif
#define WCACHE_N 64
#define WCACHE_BYTES (8u << 20)
static wctx g_wc[WCACHE_N];
static int g_nwc;
static pthread_mutex_t g_wc_mu = PTHREAD_MUTEX_INITIALIZER;
static void wctx_get(wctx *w) {
#if PJ_WCACHE
    pthread_mutex_lock(&g_wc_mu);
    if (g_nwc) { *w = g_wc[--g_nwc]; pthread_mutex_unlock(&g_wc_mu); return; }
    pthread_mutex_unlock(&g_wc_mu);
#endif
    wctx_init(w);
}
static void wctx_put(wctx *w) {
#if PJ_WCACHE
    if (w->ar.bytes <= WCACHE_BYTES) {
        pthread_mutex_lock(&g_wc_mu);
        if (g_nwc < WCACHE_N) { g_wc[g_nwc++] = *w; pthread_mutex_unlock(&g_wc_mu); return; }
        pthread_mutex_unlock(&g_wc_mu);
    }
#endif
    arena_free(&w->ar); free(w->tab); free(w->bm);
}

static inline uint32_t shape_slot_hash(const pj_shape *parent, uint32_t kh) { return (uint32_t)mix((uintptr_t)parent, kh); }

// Records c as s's predicted next shape, with c's key as the byte pattern '"' key '"' ':' that
// the parser matches in one 32-byte compare (escaped or long keys: plen 2, never used).
static void set_inpat(pj_shape *c) {
    const pj_key *k = c->key;
    memset(c->inpat, 0, sizeof c->inpat);
    if (k->plain && k->len + 3 <= sizeof c->inpat) {
        c->inpat[0] = '"'; memcpy(c->inpat + 1, k->s, k->len); c->inpat[1 + k->len] = '"'; c->inpat[2 + k->len] = ':';
        c->inplen = k->len + 3;
    } else {
        c->inpat[0] = c->inpat[1] = 0xFF; c->inplen = 2;
    }
    c->inpm = c->inplen >= 32 ? ~0u : (1u << c->inplen) - 1;
}
_Static_assert(offsetof(pj_shape, pat) == offsetof(pj_shape, plen) + 8 && offsetof(pj_shape, inpat) == offsetof(pj_shape, inplen) + 8, "pattern layout");
// plen, pm, pat <- c's inplen, inpm, inpat: one 40-byte copy, no call
INLINE void set_last(pj_shape *s, pj_shape *c) {
    s->last = c;
    memcpy(&s->plen, &c->inplen, 40);
}

// An object's first key is predicted from its context (the parent's shape after the key, or the
// enclosing array), not from the root's single last transition, which thrashes when objects of
// different kinds nest (twitter: tweet / user / entities / metadata all start at the root).

#define PJ_KIDS_MAX 8
#ifndef PJ_RPRED
#define PJ_RPRED 1
#endif
static __attribute__((noinline)) pj_shape *shape_next_slow(wctx *w, pj_shape *s, const uint8_t *kp, uint32_t klen, uint64_t pre) {
    // most shapes have a few children: compare them directly before hashing the key
    if (s->nkids <= PJ_KIDS_MAX)
        for (pj_shape *c = s->kids; c; c = c->sib)
            if (c->key->len == klen && key_eq(c->key->s, kp, klen)) { if (s->last != c) set_last(s, c); return c; }
    uint32_t kh = key_hash(kp, klen, pre);
    uint32_t m = w->cap - 1, i = shape_slot_hash(s, kh) & m;
    for (pj_shape *c; (c = w->tab[i]); i = (i + 1) & m) {
        if (c->parent == s && c->key->hash == kh && c->key->len == klen && !memcmp(c->key->s, kp, klen)) { if (s->last != c) set_last(s, c); return c; }
    }
    char *ks = arena_alloc(&w->ar, klen + 32, 1);
    memcpy(ks, kp, klen); memset(ks + klen, 0, 32);
    pj_key k = {pre, klen, kh, ks, !memchr(ks, '\\', klen), 0};
    pj_shape *c = new_shape(w, s, &k);
    w->tab[i] = c;
    if (s->nkids++ < PJ_KIDS_MAX) { c->sib = s->kids; s->kids = c; }
    if (++w->cnt * 2 > w->cap) {
        uint32_t nc = w->cap * 2; pj_shape **nt = calloc(nc, sizeof(pj_shape *));
        for (uint32_t j = 0; j < w->cap; j++) {
            pj_shape *x = w->tab[j]; if (!x) continue;
            uint32_t q = shape_slot_hash(x->parent, x->key->hash) & (nc - 1);
            while (nt[q]) q = (q + 1) & (nc - 1);
            nt[q] = x;
        }
        free(w->tab); w->tab = nt; w->cap = nc;
    }
    set_last(s, c);
    return c;
}

INLINE pj_shape *shape_next(wctx *w, pj_shape *s, const uint8_t *kp, uint32_t klen) {
    uint64_t pre = load8(kp) & pj__lenmask(klen);
    pj_shape *c = s->last;
    if (LIKELY(c && c->key->len == klen && key_eq(c->key->s, kp, klen))) return c;
    return shape_next_slow(w, s, kp, klen, pre);
}

static inline uint64_t shape_word(const pj_shape *s) { return W(PJ_TAG_OBJF, ((uint64_t)s->n << 42) | ((uintptr_t)s >> 6)); }

// ---- bit helpers -------------------------------------------------------------

// Escape scanner from simdjson: returns mask of escaped chars.
INLINE uint64_t esc_next(uint64_t bs, uint64_t *carry) {
    if (!bs) { uint64_t e = *carry; *carry = 0; return e; }
    const uint64_t ODD = 0xAAAAAAAAAAAAAAAAull;
    uint64_t pot = bs & ~*carry;
    uint64_t etc = (((pot << 1) | ODD) - pot) ^ ODD;
    uint64_t escaped = etc ^ (bs | *carry);
    *carry = (etc & bs) >> 63;
    return escaped;
}

// ---- number parsing ----------------------------------------------------------

static const double P10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                             1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

INLINE bool is_eight_digits(uint64_t v) {
    return (((v & 0xF0F0F0F0F0F0F0F0ull) | (((v + 0x0606060606060606ull) & 0xF0F0F0F0F0F0F0F0ull) >> 4)) == 0x3333333333333333ull);
}
INLINE uint32_t parse_eight(uint64_t v) {
    const uint64_t mask = 0x000000FF000000FFull, mul1 = 0x000F424000000064ull, mul2 = 0x0000271000000001ull;
    v -= 0x3030303030303030ull;
    v = (v * 10) + (v >> 8);
    return (uint32_t)((((v & mask) * mul1) + (((v >> 16) & mask) * mul2)) >> 32);
}

// Eisel-Lemire (simdjson's compute_float_64). false -> needs fallback/out of range.
INLINE bool eisel_lemire(int64_t power, uint64_t i, bool neg, double *d) {
    if (i == 0 || power < -342) { *d = neg ? -0.0 : 0.0; return true; }
    if (power > 308) return false;
    int64_t exponent = (((152170 + 65536) * power) >> 16) + 1024 + 63;
    int lz = __builtin_clzll(i);
    i <<= lz;
    uint32_t index = 2 * (uint32_t)(power + 342);
    __uint128_t fp = (__uint128_t)i * pj_pow5_128[index];
    uint64_t lower = (uint64_t)fp, upper = (uint64_t)(fp >> 64);
    if ((upper & 0x1FF) == 0x1FF) {
        __uint128_t sp = (__uint128_t)i * pj_pow5_128[index + 1];
        uint64_t sh = (uint64_t)(sp >> 64);
        lower += sh;
        if (sh > lower) upper++;
    }
    uint64_t upperbit = upper >> 63;
    uint64_t mantissa = upper >> (upperbit + 9);
    lz += (int)(1 ^ upperbit);
    int64_t real_exponent = exponent - lz;
    if (UNLIKELY(real_exponent <= 0)) {
        if (-real_exponent + 1 >= 64) { *d = neg ? -0.0 : 0.0; return true; }
        mantissa >>= -real_exponent + 1;
        mantissa += (mantissa & 1);
        mantissa >>= 1;
        real_exponent = (mantissa < (1ull << 52)) ? 0 : 1;
    } else {
        if (UNLIKELY((lower <= 1) && (power >= -4) && (power <= 23) && ((mantissa & 3) == 1))) {
            if ((mantissa << (upperbit + 64 - 53 - 2)) == upper) mantissa &= ~1ull;
        }
        mantissa += mantissa & 1;
        mantissa >>= 1;
        if (mantissa >= (1ull << 53)) { mantissa = (1ull << 52); real_exponent++; }
        mantissa &= ~(1ull << 52);
        if (UNLIKELY(real_exponent > 2046)) return false;
    }
    uint64_t bits = mantissa | ((uint64_t)real_exponent << 52) | ((uint64_t)neg << 63);
    memcpy(d, &bits, 8);
    return true;
}


// Eisel-Lemire specialized for m * 10^-k, 1 <= k <= 19, m != 0 (result always normal); returns
// the double's bits. After rounding the mantissa is in [2^52, 2^53] with its hidden bit set, so
// adding (exponent - 1) << 52 both drops that bit and carries the 2^53 rounding overflow into
// the exponent: no overflow check, no mask.
INLINE uint64_t el_neg_small(unsigned k, uint64_t i, uint64_t sign) {
    int64_t power = -(int64_t)k;
    int64_t exponent = (((152170 + 65536) * power) >> 16) + 1024 + 63;
    int lz = __builtin_clzll(i);
    i <<= lz;
    const uint64_t *t = &pj_pow5_128[2 * (uint32_t)(power + 342)];
    __uint128_t fp = (__uint128_t)i * t[0];
    uint64_t lower = (uint64_t)fp, upper = (uint64_t)(fp >> 64);
    if (UNLIKELY((upper & 0x1FF) == 0x1FF)) {
        __uint128_t sp = (__uint128_t)i * t[1];
        uint64_t sh = (uint64_t)(sp >> 64);
        lower += sh;
        if (sh > lower) upper++;
    }
    uint64_t upperbit = upper >> 63;
    uint64_t mantissa = upper >> (upperbit + 9);
    if (UNLIKELY(lower <= 1) && k <= 4 && (mantissa & 3) == 1) {
        // The asm keeps this a (never-taken) branch: clang 18 if-converted it into ~8 selects
        // on the upper -> mantissa chain of every number (gcc keeps the branch; num_known -27% on Neoverse V2).
        __asm__ volatile("" : "+r"(mantissa));
        if ((mantissa << (upperbit + 64 - 53 - 2)) == upper) mantissa &= ~1ull;
    }
    mantissa = (mantissa + 1) >> 1; // == (m + (m & 1)) >> 1
    return (mantissa + ((uint64_t)(exponent - lz - 2 + (int64_t)upperbit) << 52)) | sign;
}

static locale_t c_locale;
static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;
static void c_locale_init(void) { c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0); }

static __attribute__((noinline)) bool strtod_fallback(const uint8_t *s, const uint8_t *e, double *d) {
    pthread_once(&c_locale_once, c_locale_init);
    size_t n = (size_t)(e - s);
    char tmp[512], *b = n < sizeof tmp ? tmp : malloc(n + 1);
    memcpy(b, s, n); b[n] = 0;
    *d = strtod_l(b, 0, c_locale);
    if (b != tmp) free(b);
    return __builtin_isfinite(*d);
}

// ---- chunk parser ------------------------------------------------------------

enum { K_DOC, K_ARR, K_OBJ, K_UNK };
enum { ST_NONE, ST_AFTER_VALUE, ST_AFTER_KEY, ST_EXPECT_VALUE, ST_ARR_OPEN, ST_OBJ_OPEN, ST_EXPECT_KEY };

typedef struct { uint8_t type; uint32_t count; uint64_t end, trailer; } closed_rec;
typedef struct { uint8_t type; uint32_t count; uint64_t hdr; pj_shape *shape; } open_rec;

typedef struct {
    size_t beg, end;
    uint64_t t0, tcap, next_region;
    uint8_t *S;
    // results
    uint64_t t_end; uint8_t *S_end; // where this chunk's JUMP word and string arena ended
    int err; size_t errpos; const char *msg;
    uint8_t c0, end_state, uk_type, empty;
    uint32_t uk_count;
    closed_rec *closed; int nclosed, ccap;
    open_rec *open; int nopen;
    int wi;          // worker that parsed it: its open shapes live in that worker's arena
} chunk_t;

typedef struct {
    uint64_t hdr;
    pj_shape *shape;
    uint32_t count;
    uint8_t kind, deep, extras, utype;
    union {
        pj_shape **ctx;  // K_OBJ: where this object's first-key shape is remembered (parent context)
        pj_shape *apred; // other frames: first-key shape of the last object value inside
    };
} lframe; // 32 bytes: stk[d] is a shift

typedef struct {
    const uint8_t *buf; size_t len;
    uint64_t *T;
    chunk_t *ch; int nch;
    size_t *bnd; uint8_t *parity;
    wctx *w; uint8_t *w_init;
} job_t;

static __attribute__((noinline)) const uint8_t *skip_ws_slow(const uint8_t *p) {
    for (;;) {
        uint64_t m = nonws_mask(p);
        if (m) return p + (__builtin_ctzll(m) >> MSHIFT);
        p += SCANW;
    }
}
INLINE const uint8_t *skip_ws(const uint8_t *p) {
    if (LIKELY(*p > ' ')) return p;
    if (LIKELY(*p == ' ' && p[1] > ' ')) return p + 1;
    return skip_ws_slow(p);
}

static const int8_t HEXV[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
static inline int hex4(const uint8_t *p) { // table lookups: no per-digit branches
    int a = HEXV[p[0]], b = HEXV[p[1]], c = HEXV[p[2]], d = HEXV[p[3]];
    if ((a | b | c | d) < 0) return -1;
    return (a << 12) | (b << 8) | (c << 4) | d;
}

#ifndef PJ_SCOPY
#define PJ_SCOPY 1
#endif
// Slow string path: p points at the first special char (after `start`, the byte after
// the opening quote). Copies into dst with unescaping. Returns pointer past the closing
// quote, or NULL on error. *out_len = bytes written.
static __attribute__((noinline)) const uint8_t *str_slow(const uint8_t *start, const uint8_t *p, uint8_t *dst, uint32_t *out_len, bool *copied) {
#if PJ_SCOPY
    // Unescaping copy: plain runs are copied SCANW bytes at a time while they are scanned (one
    // load, one store, no memcpy call); stores run up to SCANW - 1 bytes past the output, which
    // never outgrows the input (escapes only shrink), inside the chunk's 2x string region.
    if (*p != '\\') return 0; // control character
    uint8_t *o = dst;
    for (const uint8_t *q = start; q < p; q += SCANW, o += SCANW) memcpy(o, q, SCANW);
    o = dst + (p - start);
    // simple escapes from a table: a jump table's indirect branch mispredicted on mixed \n / \" (16% of str_slow)
    static const uint8_t ESC[256] = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = '\r', ['t'] = '\t'};
    for (;;) {
        uint8_t e = p[1], x = ESC[e];
        if (LIKELY(x)) { *o++ = x; p += 2; }
        else if (e == 'u') {
            int cp1 = hex4(p + 2);
            if (cp1 < 0) return 0;
            p += 6;
            uint32_t u = (uint32_t)cp1;
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (p[0] != '\\' || p[1] != 'u') return 0;
                int lo = hex4(p + 2);
                if (lo < 0xDC00 || lo > 0xDFFF) return 0;
                u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                p += 6;
            } else if (u >= 0xDC00 && u <= 0xDFFF) return 0;
            if (u < 0x80) *o++ = (uint8_t)u;
            else if (u < 0x800) { *o++ = 0xC0 | (u >> 6); *o++ = 0x80 | (u & 63); }
            else if (u < 0x10000) { *o++ = 0xE0 | (u >> 12); *o++ = 0x80 | ((u >> 6) & 63); *o++ = 0x80 | (u & 63); }
            else { *o++ = 0xF0 | (u >> 18); *o++ = 0x80 | ((u >> 12) & 63); *o++ = 0x80 | ((u >> 6) & 63); *o++ = 0x80 | (u & 63); }
        } else return 0;
        for (;;) {
            memcpy(o, p, SCANW);
            uint64_t b = str_mask(p);
            if (b) { size_t k = __builtin_ctzll(b) >> MSHIFT; p += k; o += k; break; }
            p += SCANW; o += SCANW;
        }
        if (*p == '"') break;
        if (*p != '\\') return 0;
    }
    *out_len = (uint32_t)(o - dst);
    *copied = true;
    return p + 1;
#else

    uint8_t *o = dst;
    bool cp = false;
    const uint8_t *run = start;
    for (;;) {
        uint8_t c = *p;
        if (c == '"') break;
        if (c == '\\') {
            if (!cp) cp = true;
            memcpy(o, run, (size_t)(p - run)); o += p - run;
            uint8_t e = p[1];
            switch (e) {
            case '"': *o++ = '"'; p += 2; break;
            case '\\': *o++ = '\\'; p += 2; break;
            case '/': *o++ = '/'; p += 2; break;
            case 'b': *o++ = '\b'; p += 2; break;
            case 'f': *o++ = '\f'; p += 2; break;
            case 'n': *o++ = '\n'; p += 2; break;
            case 'r': *o++ = '\r'; p += 2; break;
            case 't': *o++ = '\t'; p += 2; break;
            case 'u': {
                int cp1 = hex4(p + 2);
                if (cp1 < 0) return 0;
                p += 6;
                uint32_t u = (uint32_t)cp1;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    if (p[0] != '\\' || p[1] != 'u') return 0;
                    int lo = hex4(p + 2);
                    if (lo < 0xDC00 || lo > 0xDFFF) return 0;
                    u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                    p += 6;
                } else if (u >= 0xDC00 && u <= 0xDFFF) return 0;
                if (u < 0x80) *o++ = (uint8_t)u;
                else if (u < 0x800) { *o++ = 0xC0 | (u >> 6); *o++ = 0x80 | (u & 63); }
                else if (u < 0x10000) { *o++ = 0xE0 | (u >> 12); *o++ = 0x80 | ((u >> 6) & 63); *o++ = 0x80 | (u & 63); }
                else { *o++ = 0xF0 | (u >> 18); *o++ = 0x80 | ((u >> 12) & 63); *o++ = 0x80 | ((u >> 6) & 63); *o++ = 0x80 | (u & 63); }
                break;
            }
            default: return 0;
            }
            run = p;
        } else if (c < 0x20) {
            return 0;
        } else {
            p++;
        }
        // fast skip of plain runs
        for (;;) {
            uint64_t b = str_mask(p);
            if (b) { p += __builtin_ctzll(b) >> MSHIFT; break; }
            p += SCANW;
        }
    }
    if (cp) {
        memcpy(o, run, (size_t)(p - run)); o += p - run;
        *out_len = (uint32_t)(o - dst);
    } else {
        *out_len = (uint32_t)(p - start);
    }
    *copied = cp;
    return p + 1;
#endif
}

// Scan a string starting after the opening quote. Returns pointer past closing quote.
// *sp/*sl = bytes (input if no escapes, else dst).
INLINE const uint8_t *scan_str(const uint8_t *p, uint8_t *dst, const uint8_t **sp, uint32_t *sl) {
    const uint8_t *start = p;
    for (;;) {
        uint64_t b = str_mask(p);
        if (b) { p += __builtin_ctzll(b) >> MSHIFT; break; }
        p += SCANW;
    }
    if (LIKELY(*p == '"')) { *sp = start; *sl = (uint32_t)(p - start); return p + 1; }
    bool copied;
    const uint8_t *r = str_slow(start, p, dst, sl, &copied);
    *sp = copied ? dst : start;
    return r;
}

static const uint8_t *skip_str(const uint8_t *p) { // after opening quote; lenient (lookahead only)
    for (;;) {
        uint8_t c = *p;
        if (c == '"') return p + 1;
        if (c == '\\') { if (!p[1]) return p + 1; p += 2; continue; }
        if (!c) return p;
        p++;
    }
}

// Where scan_str should unescape so that str_word needs no move.
INLINE uint8_t *sdst(uint8_t *S) { return (uint8_t *)(((uintptr_t)S + 3) & ~(uintptr_t)3) + 4; }

INLINE uint64_t str_word(const uint8_t *sp, uint32_t sl, uint8_t **S, const uint8_t *in, size_t inlen) {
    if (sl <= 6) return W(PJ_TAG_SSTR + sl, load8(sp) & pj__lenmask(sl));
    size_t off = (size_t)(sp - in);
    if (LIKELY(sp >= in && off < inlen && off < (1ull << 32) && sl < 65536)) return W(PJ_TAG_REF, ((uint64_t)off << 16) | sl);
    uint8_t *h = (uint8_t *)(((uintptr_t)*S + 3) & ~(uintptr_t)3);
    uint32_t n = sl;
    if (sp != h + 4) memmove(h + 4, sp, sl);
    memcpy(h, &n, 4);
    h[4 + sl] = 0;
    *S = h + 5 + sl;
    return W(PJ_TAG_STR, (uintptr_t)h);
}

#define FAIL(m) do { C->err = 1; C->msg = (m); C->errpos = (size_t)(p - buf); goto done; } while (0)

// Parses one number at p (general path). Returns new p or NULL.
static __attribute__((noinline)) const uint8_t *parse_num_slow(const uint8_t *p, uint64_t *out, uint8_t **S) {
    const uint8_t *start = p;
    bool neg = *p == '-';
    p += neg;
    const uint8_t *ds = p;
    uint64_t m = 0;
    if (*p == '0') {
        p++;
    } else if (LIKELY(is_digit(*p))) {
        do { m = m * 10 + (uint64_t)(*p - '0'); p++; } while (is_digit(*p));
    } else {
        return 0;
    }
    int64_t exp = 0;
    bool isint = true;
    size_t ndig = (size_t)(p - ds);
    if (*p == '.') {
        isint = false;
        p++;
        const uint8_t *fs = p;
        uint64_t v;
        while (is_eight_digits(v = load8(p))) { m = m * 100000000 + parse_eight(v); p += 8; }
        while (is_digit(*p)) { m = m * 10 + (uint64_t)(*p - '0'); p++; }
        if (p == fs) return 0;
        exp = -(int64_t)(p - fs);
        ndig += (size_t)(p - fs);
    }
    if ((*p | 0x20) == 'e') {
        isint = false;
        p++;
        bool eneg = *p == '-';
        if (*p == '-' || *p == '+') p++;
        if (!is_digit(*p)) return 0;
        int64_t e = 0;
        do { if (e < 0x10000000) e = e * 10 + (*p - '0'); p++; } while (is_digit(*p));
        exp += eneg ? -e : e;
    }
    if (UNLIKELY(ndig > 19)) {
        const uint8_t *q = ds;
        size_t lz = 0;
        while (*q == '0' || *q == '.') { if (*q == '0') lz++; q++; }
        if (isint && ndig == 20) { // may still fit in uint64
            uint64_t v = 0; bool ovf = false;
            for (const uint8_t *r = ds; r < p; r++) { ovf |= __builtin_mul_overflow(v, 10, &v); ovf |= __builtin_add_overflow(v, (uint64_t)(*r - '0'), &v); }
            if (!ovf && !neg) {
                pj_big *b = (pj_big *)(((uintptr_t)*S + 7) & ~(uintptr_t)7);
                *S = (uint8_t *)(b + 1);
                b->neg = 0; b->is_u64 = 1; b->u = v;
                *out = W(PJ_TAG_BIG, (uintptr_t)b);
                return p;
            }
        }
        if (ndig - lz > 19) {
            double d;
            if (!strtod_fallback(start, p, &d)) return 0;
            memcpy(out, &d, 8);
            return p;
        }
    }
    if (isint) {
        if (ndig <= 19 && (ndig < 19 || m < 10000000000000000000ull)) {
            if (m < (1ull << 47) || (neg && m == (1ull << 47))) {
                int64_t v = neg ? -(int64_t)m : (int64_t)m;
                *out = W(PJ_TAG_INT, (uint64_t)v & 0xFFFFFFFFFFFFull);
                return p;
            }
            pj_big *b = (pj_big *)(((uintptr_t)*S + 7) & ~(uintptr_t)7);
            *S = (uint8_t *)(b + 1);
            b->neg = neg; b->is_u64 = !neg && m > INT64_MAX; b->u = m;
            if (neg && m > (1ull << 63)) { double d; if (!strtod_fallback(start, p, &d)) return 0; memcpy(out, &d, 8); return p; }
            *out = W(PJ_TAG_BIG, (uintptr_t)b);
            return p;
        }
        double d;
        if (!strtod_fallback(start, p, &d)) return 0;
        memcpy(out, &d, 8);
        return p;
    }
    double d;
    if (m <= (1ull << 53) && exp >= -22 && exp <= 22) {
        d = (double)m;
        d = exp < 0 ? d / P10[-exp] : d * P10[exp];
        if (neg) d = -d;
    } else if (!eisel_lemire(exp, m, neg, &d)) {
        if (!strtod_fallback(start, p, &d)) return 0;
    }
    memcpy(out, &d, 8);
    return p;
}


static void dig_idx_init(void) {
    for (int n1 = 0; n1 < 20; n1++)
        for (int n = 0; n < 20; n++)
            for (int j = 0; j < 32; j++) {
                int k = j - (32 - n);
                g_dig_idx[n1][n][j] = k < 0 ? 0xFF : (uint8_t)(k + (k >= n1));
            }
}
static pthread_once_t g_dig_once = PTHREAD_ONCE_INIT;

INLINE uint64_t num_word(uint64_t m, unsigned n2, bool neg) {
    if (n2) {
        uint64_t sign = (uint64_t)neg << 63;
        if (UNLIKELY(m == 0)) return sign;
        return el_neg_small(n2, m, sign);
    }
    return W(PJ_TAG_INT, (uint64_t)(neg ? -(int64_t)m : (int64_t)m) & 0xFFFFFFFFFFFFull);
}

// Number whose extent [p, p+len) is already known (from the terminator bitmap).
// Validation is a single predictable branch; nothing here feeds the parse position.
INLINE bool num_known(const uint8_t *p, unsigned len, uint64_t *out) {
    bool neg = *p == '-';
    p += neg; len -= neg;
    dig32 D;
#if PJ_DIGCNT
    unsigned n1 = dig_scan_lo(p, &D); // 15 if p[0..16) are all digits (fails n1 <= 14 below)
#else
    unsigned n1 = dig_scan(p, &D);
#endif
#if PJ_DIGMASK
    // Validation from bit masks of the extent: every non-digit is a '.', there is at most one,
    // it has digits on both sides, and the leading digit is not a redundant 0. Each check is true
    // on valid input and none splits on the number's shape, so the branches predict; few
    // temporaries are live (the loop around this is at the register limit).
    {
        uint64_t nd = _bzhi_u64(D.m, len);                          // non-digits in the extent
#if PJ_INTLOAD
        // Integers first (the branch num_word takes on n2): no non-digit in the extent means
        // n1 == len, so only the length and leading-zero checks apply. The value is right-aligned by
        // a fixed-pattern permute of the scanned digits: no gather index (n1), no high half.
        if (!nd) {
            if (UNLIKELY(len - 1 > 13)) return false;                // 1..14 digits
            if (UNLIKELY(((D.m & 2) | (uint8_t)(*p ^ '0')) == 0)) return false; // leading zero
            uint64_t m = int_value(&D, len);
            *out = W(PJ_TAG_INT, (uint64_t)(neg ? -(int64_t)m : (int64_t)m) & 0xFFFFFFFFFFFFull);
            return true;
        }
        uint64_t bad = _bzhi_u64(D.m & ~dig_dots(&D), len) | (nd & (nd - 1));
        if (UNLIKELY(bad)) return false;
        if (UNLIKELY(n1 - 1 > 13)) return false;                    // 1..14 integer digits
        if (UNLIKELY(len > 20)) return false;                       // <= 19 digits
        if (UNLIKELY(n1 + 1 == len)) return false;                  // '.' last
        if (UNLIKELY(((D.m & 2) | (uint8_t)(*p ^ '0')) == 0)) return false; // leading zero
        uint64_t m = dig_value(&D, len - 1, n1), sign = (uint64_t)neg << 63;
        *out = UNLIKELY(m == 0) ? sign : el_neg_small(len - n1 - 1, m, sign);
        return true;
#else
        uint64_t bad = _bzhi_u64(D.m & ~dig_dots(&D), len) | (nd & (nd - 1));
        if (UNLIKELY(bad)) return false;
        if (UNLIKELY(n1 - 1 > 13)) return false;                    // 1..14 integer digits
        if (UNLIKELY(len > 20)) return false;                       // <= 19 digits
        if (UNLIKELY(n1 + 1 == len)) return false;                  // '.' last
        if (UNLIKELY(((D.m & 2) | (uint8_t)(*p ^ '0')) == 0)) return false; // leading zero
        unsigned isf = nd != 0;
        *out = num_word(dig_value(&D, len - isf, n1), len - n1 - isf, neg);
        return true;
#endif
    }
#elif PJ_DIGCNT
    // Same checks from a count of the extent's non-digits (no second scan for the fraction).
    // ccmp makes the conjunctions branch-free.
    {
        // Integers (n1 == len) need no count; the branch is the one the digit scan's fraction
        // length always took, so it costs mixed int/float data nothing new.
        unsigned isf = n1 < len;
        if (UNLIKELY(n1 - 1 > 13)) return false;                    // 1..14 integer digits
        if (UNLIKELY(len > 20)) return false;                       // <= 19 digits
        if (isf && UNLIKELY(!dig_nd_one(&D, len) | (p[n1] != '.') | (n1 + 1 == len))) return false;
        // Leading zero: '0' then a digit (byte 1's nibble of m0 is clear). One OR, one test: as
        // (*p == '0') & (n1 != 1), clang 18 branched on n1 == 1 first (mispredicts on 1-digit ints).
        if (UNLIKELY(((D.m0 & 0xF0) | (uint8_t)(*p ^ '0')) == 0)) return false;
#if PJ_INTLOAD
        if (!isf) { // integers: the low 16 lanes right-aligned by one TBL (no index table, no high half)
            uint64_t m = int_value(&D, len);
            *out = W(PJ_TAG_INT, (uint64_t)(neg ? -(int64_t)m : (int64_t)m) & 0xFFFFFFFFFFFFull);
            return true;
        }
#endif
        *out = num_word(dig_value(&D, len - isf, n1), len - n1 - isf, neg);
        return true;
    }
#endif
    if (UNLIKELY(n1 > 19 || n1 == 0)) return false;
    // The extent is known, so the fraction length comes from it (n1 -> n2 is one subtract and
    // the digit-index load does not wait on a second scan); the scan only validates, off the chain.
    // (x86 only: on Neoverse V2 it measured 2-4% slower than taking n2 from the digit scan.)
#if defined(__x86_64__)
    unsigned isf = n1 != len;
    unsigned n2 = (len - n1 - 1) & -isf;
#else
    unsigned n2 = n1 != len ? dig_n2(&D, n1) : 0;
#endif
    unsigned n = n1 + n2;
    // Non-short-circuit (& |): n1 and n2 vary per number, so && / ?: here compile to
    // data-dependent branches (a 1-digit integer part mispredicts ~5% of numbers).
    bool lead = (*p != '0') | (n1 == 1);
#if defined(__x86_64__)
    bool frac = (p[n1] == '.') & (n <= 19) & (dig_n2(&D, n1) == n2);
#else
    bool frac = (p[n1] == '.') & (n1 + 1 + n2 == len) & (n <= 19);
#endif
    bool intg = (n1 == len) & (n1 <= 14);
    bool ok = lead & ((frac & (n2 != 0)) | (intg & (n2 == 0)));
    if (UNLIKELY(!ok)) return false;
    *out = num_word(dig_value(&D, n, n1), n2, neg);
    return true;
}

// Number at p with unknown extent. Returns new p or NULL.
INLINE const uint8_t *parse_num(const uint8_t *p, uint64_t *out, uint8_t **S) {
    const uint8_t *start = p;
    bool neg = *p == '-';
    p += neg;
    dig32 D;
    unsigned n1 = dig_scan(p, &D);
    if (UNLIKELY(n1 == 0 || n1 > 19 || (*p == '0' && n1 > 1))) return parse_num_slow(start, out, S);
    unsigned n2 = 0, n;
    if (LIKELY(p[n1] == '.')) {
        n2 = dig_n2(&D, n1);
        n = n1 + n2;
        if (UNLIKELY(n2 == 0 || n > 19 || (p[n1 + 1 + n2] | 0x20) == 'e')) return parse_num_slow(start, out, S);
        p += n1 + 1 + n2;
    } else {
        if (UNLIKELY((p[n1] | 0x20) == 'e')) return parse_num_slow(start, out, S);
        n = n1;
        p += n1;
        if (n1 > 14) { // may not fit the 48-bit inline int
            uint64_t m = dig_value(&D, n, n1);
            if (m >= (1ull << 47)) {
                if (neg && m > (1ull << 63)) return parse_num_slow(start, out, S);
                pj_big *b = (pj_big *)(((uintptr_t)*S + 7) & ~(uintptr_t)7);
                *S = (uint8_t *)(b + 1);
                b->neg = neg; b->is_u64 = !neg && m > INT64_MAX; b->u = m;
                *out = W(PJ_TAG_BIG, (uintptr_t)b);
                return p;
            }
            *out = num_word(m, 0, neg);
            return p;
        }
    }
    *out = num_word(dig_value(&D, n, n1), n2, neg);
    return p;
}

// Terminator bitmap: bit i set iff byte (base + i) is ',', '}' or ']'. A number's
// end is the next terminator; no string state is needed because only number bytes
// can sit between a number's first byte and its terminator.
// Builds the bitmap over [base, lim) and validates UTF-8 over [beg, end).
// The bitmaps are built in windows of BM_WIN input bytes just ahead of the parser: each window
// is one tight SIMD pass, then the parser runs on input that pass just pulled into cache, with
// the bitmap in L1. The window end is a soft chunk end: the parser stops there at a token
// boundary (the same p >= e checks as at chunk ends), the window is refilled, and parsing
// resumes from the saved state. Per 64-byte block: {terminators, non-whitespace (wsb only)}.
// UTF-8 is validated as windows are built.
// PJ_WINDOWS=1: windows of BM_WIN bytes with soft stops (x86: low per-core DRAM bandwidth, so one
// pass over cache-resident input wins 10-20%). PJ_WINDOWS=0: one window per chunk and none of the
// soft-stop code is compiled (Apple/Neoverse: a separate full bitmap pass streams fast).
#ifndef PJ_WINDOWS
#if defined(__x86_64__)
#define PJ_WINDOWS 1
#else
#define PJ_WINDOWS 0
#endif
#endif
#ifndef BM_WIN
#define BM_WIN (PJ_WINDOWS ? (16u << 10) : (1ull << 40))
#endif
// Bitmap kept past the soft end: the parser only stops between values, so the next few tokens
// (typically < 100 bytes) still read bitmap words; anything farther is clamped (slow path).
#define BM_MARGIN 512
// Window fill prefetches the input PJ_PF bytes ahead (locality PJ_PFL), so the next window's
// lines arrive while the parser runs on this one instead of stalling the next fill.
#ifndef PJ_PF
#define PJ_PF 16384
#endif
#ifndef PJ_PFL
#define PJ_PFL 3
#endif
static size_t g_win; // window bytes (PJ_WIN env overrides BM_WIN; huge = one window per chunk)
static void win_init(void) {
    const char *e = PJ_WINDOWS ? getenv("PJ_WIN") : 0; // without soft stops the window must span the chunk
    g_win = e && atol(e) >= 64 ? ((size_t)atol(e) & ~(size_t)63) : BM_WIN;
}
static pthread_once_t g_win_once = PTHREAD_ONCE_INIT;

typedef struct {
    const uint8_t *buf;
    size_t beg, end, blim; // chunk range; bitmap limit for the last window (64-aligned)
    size_t vdone;          // UTF-8 validated up to here (64-aligned, from beg & ~63)
    u8chk u;
    bool wsb;
    size_t nb;             // words per bitmap: terminators at bm[0..nb), non-whitespace at bm[nb..2nb)
} bmwin;

// Copies the block at b into tmp, zeroing bytes outside [beg, end). The validator state is
// never passed by address, so it stays in registers.
static __attribute__((noinline)) void mask_block(uint8_t *tmp, size_t beg, size_t end, size_t b, const uint8_t *s) {
    size_t lo = b < beg ? beg - b : 0, hi = b + 64 > end ? end - b : 64;
    memset(tmp, 0, 64);
    if (hi > lo) memcpy(tmp + lo, s + lo, hi - lo);
}
INLINE void u8_range(bmwin *W, size_t b, const uint8_t *s) { // block at b, keeping only bytes in [beg, end)
    if (UNLIKELY(b < W->beg || b + 64 > W->end)) {
        uint8_t tmp[64] __attribute__((aligned(64)));
        mask_block(tmp, W->beg, W->end, b, s);
        u8_block(&W->u, blk_load(tmp));
    } else {
        u8_block(&W->u, blk_load(s));
    }
}

// Blocks fully inside the chunk and not yet validated run a loop without per-block range checks
// (Intel twitter / citm -2% to -3%); the edges take the general body.
INLINE void bm_fill_t(bmwin *W, uint64_t *bm, size_t base, size_t lim, bool wsb) {
    u8chk u = W->u;
    const uint8_t *buf = W->buf;
    size_t vb = W->vdone > base ? W->vdone : base, ve = lim < W->end ? lim : W->end; // blocks to validate
    uint64_t *bw = bm + W->nb;
    size_t ib = (W->beg + 63) & ~(size_t)63; if (ib < vb) ib = vb; if (ib > lim) ib = lim;
    size_t ie = W->end & ~(size_t)63; if (ie > ve) ie = ve; if (ie < ib) ie = ib;
#define FILL_GEN(B0, B1) for (size_t b = (B0); b < (B1); b += 64) { \
        const uint8_t *s = buf + b; size_t i = (b - base) >> 6; \
        blk64 v = blk_load(s); \
        bm[i] = blk_term(v); \
        if (wsb) bw[i] = ~blk_ws(v); \
        if (b >= vb && b < ve) { \
            if (UNLIKELY(b < W->beg || b + 64 > W->end)) { \
                uint8_t tmp[64] __attribute__((aligned(64))); \
                mask_block(tmp, W->beg, W->end, b, s); \
                u8_block(&u, blk_load(tmp)); \
            } else u8_block(&u, v); \
        } }
    FILL_GEN(base, ib)
    {
        uint64_t *bi = bm + ((ib - base) >> 6), *bwi = bw + ((ib - base) >> 6);
        const uint8_t *s = buf + ib, *se = buf + ie;
        for (; s < se; s += 64, bi++, bwi++) {
#if PJ_WINDOWS && PJ_PF
            __builtin_prefetch(s + PJ_PF, 0, PJ_PFL);
#endif
            blk64 v = blk_load(s);
            *bi = blk_term(v);
            if (wsb) *bwi = ~blk_ws(v);
            u8_block(&u, v);
        }
    }
    FILL_GEN(ie, lim)
#undef FILL_GEN
    size_t i = (lim - base) >> 6;
    if (ve > vb) W->vdone = (ve + 63) & ~(size_t)63;
    W->u = u;
    bm[i] = bm[i + 1] = 0;
    if (wsb) bw[i] = bw[i + 1] = 0;
}

// Builds the window starting at q's block. Returns {base, soft end, bitmap end} in registers.
typedef struct { const uint8_t *base, *e, *cover; } bmref;
static __attribute__((noinline)) bmref bm_refill(bmwin *W, uint64_t *bm, const uint8_t *q) {
    size_t base = (size_t)(q - W->buf) & ~(size_t)63;
    size_t e = g_win < W->end - base ? base + g_win : W->end, lim = e + BM_MARGIN;
    if (e >= W->end) { e = W->end; lim = W->blim > base + 64 ? W->blim : base + 64; }
    for (size_t b = W->vdone; b < base && b < W->end; b += 64) { u8_range(W, b, W->buf + b); W->vdone = b + 64; } // skipped-over gap
    if (W->wsb) bm_fill_t(W, bm, base, lim, true); else bm_fill_t(W, bm, base, lim, false);
    return (bmref){W->buf + base, W->buf + e, W->buf + lim};
}

// Validates whatever the windows did not reach.
static bool bm_finish(bmwin *W) {
    for (size_t b = W->vdone; b < W->end; b += 64) u8_range(W, b, W->buf + b);
    W->vdone = W->end;
    return u8_ok(&W->u);
}

// Pretty-printed (>= 1 newline per 128 bytes in the first 4 KB)? Then whitespace runs are
// skipped with the bitmap; compact input keeps the cheaper inline checks.
static bool looks_pretty(const uint8_t *p, size_t n) {
    if (n > 4096) n = 4096;
    size_t nl = 0;
    for (size_t i = 0; i + 64 <= n; i += 64) nl += (size_t)__builtin_popcountll(blk_eq(blk_load(p + i), '\n'));
    return n >= 64 && nl * 128 >= n;
}

// 57+ bits of a contiguous bitmap starting at bit idx.
INLINE uint64_t bm_at(const uint64_t *bm, size_t idx) { return load8((const uint8_t *)bm + (idx >> 3)) >> (idx & 7); }

// First non-whitespace byte at or after q, from the non-whitespace bitmap bw (q - bbase within it).
INLINE const uint8_t *skip_ws_bm(const uint64_t *bw, const uint8_t *bbase, const uint8_t *bcover, const uint8_t *q) {
    if (LIKELY(*q > ' ')) return q;
    if (LIKELY(*q == ' ' && q[1] > ' ')) return q + 1;
    if (PJ_WINDOWS && UNLIKELY(q >= bcover)) return skip_ws_slow(q); // past a long token: outside the window
    uint64_t lo = bm_at(bw, (size_t)(q - bbase));
    if (LIKELY(lo)) return q + __builtin_ctzll(lo);
    return skip_ws_slow(q);
}

// Position of the first terminator at or after q (q - base within the bitmap). >= 57 if none within
// 57 bytes (64 when the word is empty: one tzcnt, and num_known rejects any extent that long).
INLINE unsigned next_term(const uint64_t *bm, const uint8_t *bbase, const uint8_t *q) {
    uint64_t lo = bm_at(bm, (size_t)(q - bbase));
    return lo ? (unsigned)__builtin_ctzll(lo) : 64;
}

// next_term for a q that may lie past the window's bitmap: clamps to the zero sentinel
// (result 0: the caller takes the general number path).
INLINE unsigned next_term_c(const uint64_t *bm, const uint8_t *bbase, const uint8_t *bcover, const uint8_t *q) {
    return next_term(bm, bbase, !PJ_WINDOWS || q < bcover ? q : bcover);
}

// Parser state carried across window refills.
typedef struct {
    job_t *J; wctx *w; chunk_t *C; bmwin *bw;
    const uint64_t *bm; const uint8_t *bbase, *bcover; bool wsb;
    const uint8_t *p; uint64_t *t; uint8_t *S;
    lframe *stk; int d; uint8_t state;
} pstate;
enum { EN_FIRST = 100, EN_UNK = 101 };

// Parses until the window's soft end e (returns false: state saved, refill and call again with
// entry = state) or the chunk is done (true). e is constant within a call.
// Inlined into its single caller (as a separate function clang emitted far worse code).
INLINE bool parse_run(pstate *ps, const uint8_t *const e, int entry, const bool wsb) {
    job_t *J = ps->J; wctx *w = ps->w; chunk_t *C = ps->C;
    const uint8_t *buf = J->buf;
    const uint8_t *const eh = buf + C->end; // hard chunk end
    uint64_t *T = J->T;
    const uint64_t *const bm = ps->bm;
    const uint64_t *const bmw = bm + ps->bw->nb; // non-whitespace bitmap (wsb only)
    const uint8_t *const bbase = ps->bbase, *const bcover = ps->bcover;
    const uint8_t *p = ps->p;
    uint64_t *t = ps->t;
    uint8_t *S = ps->S;
    lframe *const stk = ps->stk;
    int d = ps->d;
    uint8_t state = ps->state;
#define SKIP_WS(q) (wsb ? skip_ws_bm(bmw, bbase, bcover, q) : skip_ws(q))
    switch (entry) {
    case EN_FIRST: stk[0].kind = K_DOC; goto value;
    case EN_UNK: break;
#if PJ_WINDOWS
    case ST_AFTER_VALUE: // the only soft-stop point: after a value, before ',' or a closer
        p = SKIP_WS(p);
        if (UNLIKELY(p >= e)) goto stop;
        if (LIKELY(*p == ',')) goto comma;
        if (*p == '}' || *p == ']') goto close;
        FAIL("expected ',' or closer");
#endif
    default: FAIL("bad resume state");
    }
    stk[0].kind = K_UNK;
    if (p >= eh) { C->empty = 1; goto done; }
    C->c0 = *p;
    switch (*p) {
    case '{': case '[': goto value;
    case ',': goto comma;
    case ':': stk[0].utype = K_OBJ; p++; goto value;
    case '}': case ']': goto close;
    default: FAIL("bad chunk start");
    }

value:
    state = ST_EXPECT_VALUE;
value_st:
    p = SKIP_WS(p);
    if (UNLIKELY(p >= eh)) goto stop;
    {
        uint8_t c = *p;
        if (LIKELY(c == '-' || is_digit(c))) {
            unsigned L2 = next_term_c(bm, bbase, bcover, p); // p may be past the window: clamped
            if (LIKELY(num_known(p, L2, t))) {
                p += L2;
            } else {
                const uint8_t *q = parse_num(p, t, &S);
                if (UNLIKELY(!q)) FAIL("bad number");
                p = q;
            }
            t++;
            goto value_done;
        }
        if (c == '"') {
            const uint8_t *sp; uint32_t sl;
            const uint8_t *q = scan_str(p + 1, sdst(S), &sp, &sl);
            if (UNLIKELY(!q)) FAIL("bad string");
            p = q;
            *t++ = str_word(sp, sl, &S, buf, J->len);
            goto value_done;
        }
        if (c == '{') {
            if (UNLIKELY(++d >= MAX_DEPTH)) FAIL("too deep");
            stk[d - 1].deep = 1;
            lframe *f = &stk[d];
#if PJ_RPRED
            {
                lframe *pf = &stk[d - 1];
                pj_shape **cx = pf->kind == K_OBJ ? &pf->shape->cfirst : &pf->apred;
                pj_shape *pr = *cx, *r = w->root;
                f->ctx = cx;
                if (pr && UNLIKELY(r->last != pr)) set_last(r, pr);
            }
#endif
            f->kind = K_OBJ; f->hdr = (uint64_t)(t - T); f->count = 0; f->deep = 0; f->extras = 0; f->shape = w->root;
            t++; p++;
            if (LIKELY(*p == '"')) goto fast_obj;
            p = SKIP_WS(p);
            if (UNLIKELY(p >= eh)) { state = ST_OBJ_OPEN; goto stop; }
            if (*p == '}') goto close;
            goto key_st;
        }
        if (c == '[') {
            if (UNLIKELY(++d >= MAX_DEPTH)) FAIL("too deep");
            stk[d - 1].deep = 1;
            lframe *f = &stk[d];
            f->kind = K_ARR; f->hdr = (uint64_t)(t - T); f->count = 0;
#if PJ_RPRED
            f->apred = 0;
#endif
            t += 2; p++;
            p = SKIP_WS(p);
            if (UNLIKELY(p >= eh)) { state = ST_ARR_OPEN; goto stop; }
            if (*p == ']') goto close;
            state = ST_EXPECT_VALUE;
            goto value_st;
        }
        if (c == 't') { if (load8(p) << 32 >> 32 != 0x65757274ull) FAIL("bad literal"); *t++ = W(PJ_TAG_CONST, 2); p += 4; goto value_done; }
        if (c == 'f') { if ((load8(p) & 0xFFFFFFFFFFull) != 0x65736c6166ull) FAIL("bad literal"); *t++ = W(PJ_TAG_CONST, 1); p += 5; goto value_done; }
        if (c == 'n') { if (load8(p) << 32 >> 32 != 0x6c6c756eull) FAIL("bad literal"); *t++ = W(PJ_TAG_CONST, 0); p += 4; goto value_done; }
        FAIL("unexpected character");
    }

value_done:
    stk[d].count++;
    state = ST_AFTER_VALUE;
    p = SKIP_WS(p);
    if (UNLIKELY(p >= e)) goto stop;
    {
        uint8_t c = *p;
        if (LIKELY(c == ',')) goto comma;
        if (c == '}' || c == ']') goto close;
        FAIL("expected ',' or closer");
    }

comma: {
        p++;
        lframe *f = &stk[d];
        uint8_t k = f->kind;
        if (UNLIKELY(k == K_UNK)) {
            if (!f->utype) {
                const uint8_t *q = skip_ws(p);
                if (*q != '"') f->utype = K_ARR;
                else { q = skip_ws(skip_str(q + 1)); f->utype = *q == ':' ? K_OBJ : K_ARR; }
            }
            k = f->utype;
        }
        if (k == K_OBJ) { if (LIKELY(f->kind == K_OBJ && !f->extras)) goto fast_obj; goto key; }
        if (k == K_ARR) goto value;
        FAIL("unexpected ','");
    }

fast_obj: {
        // Fused loop for objects whose next key is predicted by the shape's last
        // transition: "key":number pairs without the generic state machine. Any
        // surprise (whitespace, other value types, new keys) falls back to the
        // generic labels with identical state.
        lframe *f = &stk[d];
        pj_shape *sh = f->shape;
        for (;;) {
            const pj_shape *nx = sh->last;
            if (UNLIKELY(!nx)) break;
            // pretty input: skip the newline and indent here, or every key fails this compare first
            // (pretty files -2.5% to -8%; coordinates +1%, from register allocation in the compact copy)
            if (wsb) p = SKIP_WS(p);
            // '"' key '"' ':' in one compare against the pattern cached in this shape: the loads
            // depend only on sh, not on sh->last->key->s.
            const uint8_t *q = p + sh->plen;
            if (UNLIKELY(!pat_eq(p, sh->pat, sh->pm, sh->plen) || q >= e)) break;
            sh = (pj_shape *)nx;
            p = q;
            if (wsb) p = SKIP_WS(p);
            // Any number, the generic path's fallbacks included, leaves the loop: no calls in here,
            // so nothing clobbers the vector constants. NUM_START may let a few other bytes through;
            // num_known rejects them.
            if (UNLIKELY(!NUM_START(*p))) { f->shape = sh; goto value; }
            unsigned L2 = next_term(bm, bbase, p);
            if (UNLIKELY(!num_known(p, L2, t))) { f->shape = sh; goto value; }
            p += L2;
            t++;
            if (LIKELY(*p == ',' && p < e)) { p++; continue; }
            // '},{"' inside an array: close this object and open the next one in place, without
            // the generic close / comma / value round trip
            // (compact input only: pretty input never matches, and its copy of this loop is left as is)
            if (!wsb && load4(p) == 0x227B2C7Du && p + 3 < e && !f->deep && sh->ht && stk[d - 1].kind == K_ARR) {
                T[f->hdr] = shape_word(sh);
                stk[d - 1].count++;
                f->hdr = (uint64_t)(t - T); f->count = 0;
                sh = w->root;
                t++; p += 3;
                continue;
            }
            f->shape = sh;
            goto value_done;
        }
        f->shape = sh;
    }

key:
    state = ST_EXPECT_KEY;
    p = SKIP_WS(p);
    if (UNLIKELY(p >= eh)) goto stop;
key_st: {
        if (UNLIKELY(*p != '"')) FAIL("expected key");
        {
            // Predicted transition: if the input key equals the key we saw last time
            // at this position in this shape, take it without scanning the string.
            lframe *f = &stk[d];
            const pj_shape *s0 = f->shape;
            const pj_shape *c = (f->kind == K_OBJ && !f->extras) ? s0->last : 0;
            if (LIKELY(c)) {
                uint32_t pl = s0->plen - 1; // the pattern without its ':'
                if (LIKELY(pl > 1)) {
                    if (LIKELY(pat_eq(p, s0->pat, s0->pm >> 1, pl))) {
                        f->shape = (pj_shape *)c;
                        p += pl;
                        goto after_key;
                    }
                } else {
                    const pj_key *k = c->key;
                    uint32_t L = k->len;
                    if (LIKELY(k->plain && key_eq(k->s, p + 1, L) && p[1 + L] == '"')) {
                        f->shape = (pj_shape *)c;
                        p += L + 2;
                        goto after_key;
                    }
                }
            }
        }
        const uint8_t *sp; uint32_t sl;
        const uint8_t *q = scan_str(p + 1, sdst(S), &sp, &sl);
        if (UNLIKELY(!q)) FAIL("bad key");
        p = q;
        lframe *f = &stk[d];
        if (LIKELY(f->kind == K_OBJ && !f->extras)) {
            if (LIKELY(f->shape->n < PJ_SHAPE_MAX)) {
                if (UNLIKELY(sp != q - 1 - sl)) { // unescaped into S: keep a padded copy for 8-byte loads
                    memset((uint8_t *)sp + sl, 0, 8);
                }
#if PJ_RPRED
                pj_shape *s0 = f->shape;
                f->shape = shape_next(w, s0, sp, sl);
                if (s0 == w->root) *f->ctx = f->shape;
#else
                f->shape = shape_next(w, f->shape, sp, sl);
#endif
            } else {
                f->extras = 1; f->deep = 1;
                *t++ = str_word(sp, sl, &S, buf, J->len);
            }
        } else {
            *t++ = str_word(sp, sl, &S, buf, J->len);
        }
    after_key:
        state = ST_AFTER_KEY;
        p = SKIP_WS(p);
        if (UNLIKELY(p >= eh)) goto stop;
        if (UNLIKELY(*p != ':')) FAIL("expected ':'");
        p++;
        goto value;
    }

close: {
        uint8_t c = *p;
        lframe *f = &stk[d];
        switch (f->kind) {
        case K_ARR:
            if (UNLIKELY(c != ']')) FAIL("mismatched ']'");
            T[f->hdr] = W(PJ_TAG_ARR, (uint64_t)(t - T));
            T[f->hdr + 1] = f->count;
            break;
        case K_OBJ:
            if (UNLIKELY(c != '}')) FAIL("mismatched '}'");
            if (!f->deep) {
                SHAPE_FINAL(&w->ar, f->shape);
                T[f->hdr] = shape_word(f->shape);
            } else {
                SHAPE_FINAL(&w->ar, f->shape);
                *t++ = shape_word(f->shape);
                T[f->hdr] = W(PJ_TAG_OBJD, (uint64_t)(t - T));
            }
            break;
        case K_UNK: {
            uint8_t ty = c == ']' ? K_ARR : K_OBJ;
            if (UNLIKELY(f->utype && f->utype != ty)) FAIL("mismatched closer");
            if (C->nclosed == C->ccap) {
                C->ccap = C->ccap ? C->ccap * 2 : 16;
                C->closed = realloc(C->closed, (size_t)C->ccap * sizeof(closed_rec));
            }
            closed_rec *r = &C->closed[C->nclosed++];
            r->type = ty; r->count = f->count;
            r->trailer = (uint64_t)(t - T);
            if (ty == K_OBJ) t++;
            r->end = (uint64_t)(t - T);
            memset(f, 0, sizeof *f);
            f->kind = K_UNK;
            p++;
            goto value_done;
        }
        default:
            FAIL("unexpected closer");
        }
        d--;
        p++;
        goto value_done;
    }

stop:
    if (PJ_WINDOWS && state == ST_AFTER_VALUE && p < eh) { // soft end (window): save state; the caller refills and resumes
        ps->p = p; ps->t = t; ps->S = S; ps->d = d; ps->state = state;
        return false;
    }
    if (UNLIKELY(p > eh)) FAIL("token overruns chunk");
    C->end_state = state;

done:
    if (!C->err && UNLIKELY(!bm_finish(ps->bw))) { C->err = 1; C->msg = "invalid UTF-8"; C->errpos = C->beg; }
    if (!C->err) {
        C->uk_type = stk[0].kind == K_UNK ? stk[0].utype : K_DOC;
        C->uk_count = stk[0].count;
        C->nopen = d;
        if (d) {
            C->open = realloc(C->open, (size_t)d * sizeof(open_rec));
            for (int i = 1; i <= d; i++) {
                open_rec *o = &C->open[i - 1];
                o->type = stk[i].kind; o->count = stk[i].count; o->hdr = stk[i].hdr; o->shape = stk[i].shape;
            }
        }
    }
    *t = W(PJ_TAG_JUMP, C->next_region);
    C->t_end = (uint64_t)(t - T); C->S_end = S;
    if ((uint64_t)(t - T) >= C->t0 + C->tcap) { fprintf(stderr, "pj: tape region overflow\n"); abort(); }
    return true;
}
#undef SKIP_WS
#undef FAIL

static void parse_chunk(job_t *J, wctx *w, chunk_t *C, bool first) {
    const uint8_t *buf = J->buf;
    pthread_once(&g_dig_once, dig_idx_init);
    size_t blim = C->end + 128 < J->len ? C->end + 128 : J->len;
    blim = (blim + 63) & ~(size_t)63;
    pthread_once(&g_win_once, win_init);
    size_t win = g_win < C->end - C->beg + 64 ? g_win : C->end - C->beg + 64;
    size_t nb = (win + BM_MARGIN + 256) / 64 + 8, nbw = 3 * nb;
    if (w->bmcap < nbw) { free(w->bm); w->bmcap = nbw; w->bm = malloc(nbw * 8); }
    const bool wsb = looks_pretty(buf + C->beg, C->end - C->beg);
    bmwin BW = {buf, C->beg, C->end, blim, C->beg & ~(size_t)63, {0}, wsb, nb};
    u8_init(&BW.u);
    lframe stk[MAX_DEPTH];
    C->nclosed = 0; C->nopen = 0; C->err = 0; C->empty = 0;
    memset(&stk[0], 0, sizeof stk[0]);
    pstate ps = {J, w, C, &BW, w->bm, 0, 0, wsb, buf + C->beg, J->T + C->t0, C->S, stk, 0, ST_NONE};
    int entry = first ? EN_FIRST : EN_UNK;
#if !PJ_WINDOWS
    { bmref r = bm_refill(&BW, w->bm, ps.p); ps.bbase = r.base; ps.bcover = r.cover;
      if (wsb) parse_run(&ps, buf + C->end, entry, true); else parse_run(&ps, buf + C->end, entry, false); return; }
#endif
    for (;;) {
        bmref r = bm_refill(&BW, w->bm, ps.p);
        ps.bbase = r.base; ps.bcover = r.cover;
        // two copies of the parser, specialized on wsb (pretty-printed or not)
        if (wsb ? parse_run(&ps, r.e, entry, true) : parse_run(&ps, r.e, entry, false)) break;
        entry = ps.state;
    }
}

// ---- phases ------------------------------------------------------------------

// Parity of unescaped quotes in [b, e).
static int quote_parity(const uint8_t *buf, size_t b, size_t e) {
    size_t k = 0;
    while (b > k && buf[b - 1 - k] == '\\') k++;
    uint64_t carry = k & 1, cnt = 0;
    for (size_t p = b; p < e; p += 64) {
        blk64 v = blk_load(buf + p);
        uint64_t qm = blk_eq(v, '"'), bm = blk_eq(v, '\\');
        if (e - p < 64) { uint64_t m = (1ull << (e - p)) - 1; qm &= m; bm &= m; }
        uint64_t esc = esc_next(bm, &carry);
        cnt += (uint64_t)__builtin_popcountll(qm & ~esc);
    }
    return (int)(cnt & 1);
}

static void phase_a(void *ctx, int wi, int i) {
    (void)wi;
    job_t *J = ctx;
    J->parity[i] = (uint8_t)quote_parity(J->buf, J->bnd[i], J->bnd[i + 1]);
}

static uint64_t g_wbusy[MAX_THREADS], g_wcnt[MAX_THREADS];
static int g_trace = -1;
static void phase_b(void *ctx, int wi, int i) {
    job_t *J = ctx;
    uint64_t t0 = g_trace ? tnow() : 0; // clock reads only for PJ_TRACE: two of them were ~4% of a 1 KB parse
    if (!J->w_init[wi]) { wctx_get(&J->w[wi]); J->w_init[wi] = 1; }
    J->ch[i].wi = wi;
    parse_chunk(J, &J->w[wi], &J->ch[i], i == 0);
    if (g_trace) { g_wbusy[wi] += tnow() - t0; g_wcnt[wi]++; }
}

// First structural char ({}[],:) outside strings at or after b, given string state.
static size_t find_start(const uint8_t *buf, size_t len, size_t b, int in_str) {
    size_t p = b;
    if (in_str) {
        size_t k = 0;
        while (p > k && buf[p - 1 - k] == '\\') k++;
        if (k & 1) p++; // first byte is escaped
        while (p < len) {
            uint8_t c = buf[p];
            if (c == '\\') { p += 2; continue; }
            p++;
            if (c == '"') break;
        }
    }
    while (p < len) {
        uint8_t c = buf[p];
        switch (c) {
        case '{': case '}': case '[': case ']': case ',': case ':': return p;
        case '"':
            p++;
            while (p < len) { uint8_t x = buf[p]; if (x == '\\') { p += 2; continue; } p++; if (x == '"') break; }
            break;
        default: p++;
        }
    }
    return len;
}

// Is byte b inside a string? Tokenizes up to ~512 bytes from b under each hypothesis with a
// stackless grammar check; text inside a string almost never reads as JSON (and vice versa).
// Returns 0 / 1, or -1 if both or neither survive. Only a guess: a wrong one makes the chunk
// before b fail (its last string runs past b) and the parse is redone with exact parity.
enum { H_ANY, H_VAL, H_VAL_OR_CLOSE, H_KEY_OR_CLOSE, H_AFTER_STR, H_AFTER_KEY, H_AFTER_VAL };
// 0: grammar violation; 1: survived; 2: survived but ended inside an unclosed string.
static int hyp_ok(const uint8_t *buf, size_t len, size_t b, int in_str, size_t budget) {
    const uint8_t *p = buf + b, *e = buf + (len - b > budget ? b + budget : len);
    int st = H_ANY;
    if (in_str) {
        size_t k = 0;
        while (b > k && buf[b - 1 - k] == '\\') k++;
        if (k & 1) p++;
        goto str;
    }
    while (p < e) {
        uint8_t c = *p;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { p++; continue; }
        switch (st) {
        case H_ANY: // somewhere outside strings, maybe mid-scalar
            if (c == ',' || c == ':') st = H_VAL;
            else if (c == '[') st = H_VAL_OR_CLOSE;
            else if (c == '{') st = H_KEY_OR_CLOSE;
            else if (c == ']' || c == '}') st = H_AFTER_VAL;
            else if (c == '"') { p++; goto str; }
            else if (!(is_digit(c) || c == '.' || c == '-' || c == '+' || strchr("truefalsnE", c))) return 0; // mid-scalar only
            p++;
            continue;
        case H_VAL: case H_VAL_OR_CLOSE:
            if (c == '"') { p++; goto str; }
            if (c == '{') { st = H_KEY_OR_CLOSE; p++; continue; }
            if (c == '[') { st = H_VAL_OR_CLOSE; p++; continue; }
            if (c == ']' && st == H_VAL_OR_CLOSE) { st = H_AFTER_VAL; p++; continue; }
            if (c == '-' || is_digit(c)) { while (p < e && (is_digit(*p) || *p == '.' || *p == '-' || *p == '+' || (*p | 0x20) == 'e')) p++; st = H_AFTER_VAL; continue; }
            if (c == 't' || c == 'n') { if (p + 4 <= e && memcmp(p, c == 't' ? "true" : "null", 4)) return 0; p += 4; st = H_AFTER_VAL; continue; }
            if (c == 'f') { if (p + 5 <= e && memcmp(p, "false", 5)) return 0; p += 5; st = H_AFTER_VAL; continue; }
            return 0;
        case H_KEY_OR_CLOSE:
            if (c == '"') { p++; st = H_AFTER_KEY; goto str_k; }
            if (c == '}') { st = H_AFTER_VAL; p++; continue; }
            return 0;
        case H_AFTER_KEY:
            if (c != ':') return 0;
            st = H_VAL; p++; continue;
        case H_AFTER_STR:
            if (c == ':' || c == ',') { st = H_VAL; p++; continue; }
            if (c == ']' || c == '}') { st = H_AFTER_VAL; p++; continue; }
            return 0;
        case H_AFTER_VAL:
            if (c == ',') { st = H_VAL; p++; continue; }
            if (c == ']' || c == '}') { p++; continue; }
            return 0;
        }
        continue;
    str:
        st = H_AFTER_STR;
    str_k:
        for (;;) {
            if (p >= e) return 2;
            uint8_t x = *p++;
            if (x == '"') break;
            if (x == '\\') { p++; continue; }
            if (x < 0x20) return 0;
        }
    }
    return 1;
}
static int guess_in_str(const uint8_t *buf, size_t len, size_t b) {
    int o = hyp_ok(buf, len, b, 0, 512), i = hyp_ok(buf, len, b, 1, 512);
    if (o == i) { o = hyp_ok(buf, len, b, 0, 8192); i = hyp_ok(buf, len, b, 1, 8192); }
    if (o == i) return -1;
    if (!o || !i) return o ? 0 : 1;
    return o == 1 ? 0 : 1; // both survived: prefer the one that saw its strings close
}

// Small cache of freed tape/arena mappings so that repeated parses skip mmap/munmap and page
// faults (simdjson's parser keeps its buffers the same way). Only touched pages are resident.
// PJ_MFREE: freed mappings are returned with MADV_FREE (reclaimable under memory pressure, and no
// fault on reuse while they are not reclaimed).
#ifndef CACHE_MAX
#define CACHE_MAX (64ull << 30)
#endif
#ifndef PJ_MFREE
#define PJ_MFREE 1
#endif
static struct { void *p; size_t sz; } g_cache[4];
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static void *map_anon(size_t *szp) {
#ifdef __linux__
    if (*szp >= (4u << 20)) {
        size_t al = 2u << 20, sz = (*szp + al - 1) & ~(al - 1);
        uint8_t *p = mmap(0, sz + al, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return p;
        uint8_t *a = (uint8_t *)(((uintptr_t)p + al - 1) & ~(uintptr_t)(al - 1));
        if (a > p) munmap(p, (size_t)(a - p));
        if (p + sz + al > a + sz) munmap(a + sz, (size_t)(p + sz + al - (a + sz)));
        *szp = sz;
        return a;
    }
#endif
    return mmap(0, *szp, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
}

// Transparent huge pages (Linux) only where they pay: a single-chunk tape (written densely), or
// chunks of >= 1 MB (each region then fills a good part of its 2 MB page; records_1GB is 9% faster
// on Graviton with them). With small chunks every region's 2 MB page is mostly empty: a 4 MB
// document cost 15x its size. Small documents gain nothing and would fault 2 MB pages for a few KB
// (the tape's first and last words): 16 MB minimum. The string arena is mostly untouched: never.
static void map_hugepages(void *p, size_t sz, bool on) {
#ifdef __linux__
    if (sz >= (4u << 20)) madvise(p, sz, on ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#else
    (void)p; (void)sz; (void)on;
#endif
}

static void *map_get(size_t *szp) {
    size_t sz = *szp;
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < 4; i++)
        if (g_cache[i].p && g_cache[i].sz >= sz && g_cache[i].sz <= 4 * sz + (16u << 20)) {
            void *p = g_cache[i].p; *szp = g_cache[i].sz; g_cache[i].p = 0;
            pthread_mutex_unlock(&g_cache_mu);
            return p;
        }
    pthread_mutex_unlock(&g_cache_mu);
    *szp = sz < (8u << 20) ? (8u << 20) : sz;
    return map_anon(szp);
}
static void map_put(void *p, size_t sz) {
    if (sz <= CACHE_MAX) {
#if PJ_MFREE && defined(__linux__)
        if (sz >= (64u << 20)) madvise(p, sz, MADV_FREE);
#endif
        pthread_mutex_lock(&g_cache_mu);
        for (int i = 0; i < 4; i++)
            if (!g_cache[i].p) { g_cache[i].p = p; g_cache[i].sz = sz; pthread_mutex_unlock(&g_cache_mu); return; }
        pthread_mutex_unlock(&g_cache_mu);
    }
    munmap(p, sz);
}

typedef struct {
    pj_doc pub;
    uint64_t *tape; size_t tape_bytes;
    uint8_t *sarena; size_t sarena_bytes;
    wctx *w; int nw;
    void *map; size_t map_bytes;
} doc_priv;

void pj_free(pj_doc *pd) {
    if (!pd) return;
    doc_priv *d = (doc_priv *)pd;
    if (d->tape) map_put(d->tape, d->tape_bytes);
    if (d->sarena) map_put(d->sarena, d->sarena_bytes);
    for (int i = 0; i < d->nw; i++) wctx_put(&d->w[i]);
    free(d->w);
    if (d->map) munmap(d->map, d->map_bytes);
    free(d);
}

static bool state_ok(uint8_t st, uint8_t c) {
    switch (st) {
    case ST_AFTER_VALUE: return c == ',' || c == ']' || c == '}';
    case ST_AFTER_KEY: return c == ':';
    case ST_EXPECT_VALUE: return c == '{' || c == '[';
    case ST_ARR_OPEN: return c == '{' || c == '[' || c == ']';
    case ST_OBJ_OPEN: return c == '}';
    default: return false;
    }
}

// ar: the arena of the worker that created shape (its key table goes there: worker contexts are
// cached and freed separately, so a pointer into another worker's arena could dangle)
typedef struct { uint8_t type; uint32_t count; uint64_t hdr; pj_shape *shape; arena *ar; } gframe;

static pj_doc *parse_impl(const uint8_t *buf, size_t len, int nthreads, pj_err *err, void *map, size_t map_bytes, bool exact) {
    pj_err e0; if (!err) err = &e0;
    memset(err, 0, sizeof *err);
    int pool = 0;
    if (nthreads != 1 && len >= MIN_PARALLEL) {
        pool = pool_size();
        if (pool > 1 && pthread_mutex_trylock(&P.busy) != 0) pool = 0;
        if (pool <= 1) pool = 0;
    }
    static size_t chunk_bytes = 0, min_par = MIN_PARALLEL;
    if (!chunk_bytes) {
        const char *ce = getenv("PJ_CHUNK"); // testing: force tiny chunks
        chunk_bytes = ce && atol(ce) > 0 ? (size_t)atol(ce) : CHUNK_BYTES;
        if (ce) min_par = 1;
    }
    if (nthreads != 1 && len >= min_par && !pool) {
        pool = pool_size();
        if (pool > 1 && pthread_mutex_trylock(&P.busy) != 0) pool = 0;
        if (pool <= 1) pool = 0;
    }
    int nch = pool ? (int)((len + chunk_bytes - 1) / chunk_bytes) : 1;
    if (pool && nch < pool * 4 && chunk_bytes == CHUNK_BYTES) nch = pool * 4;
    if (nch > (int)len) nch = len ? (int)len : 1;
    if (nch < 1) nch = 1;
    int nw = pool ? pool : 1;

    job_t J = {0};
    J.buf = buf; J.len = len; J.nch = nch;
    if (g_trace < 0) g_trace = getenv("PJ_TRACE") != 0;
    uint64_t *T; uint8_t *SA; size_t tape_bytes, sarena_bytes;
    uint64_t T0 = tnow(), T1 = T0, T2;
    size_t *s = 0;
    bool spec = false;
    {
    J.bnd = malloc((size_t)(nch + 1) * sizeof(size_t));
    J.parity = calloc((size_t)nch, 1);
    J.ch = calloc((size_t)nch, sizeof(chunk_t));
    for (int i = 0; i <= nch; i++) J.bnd[i] = (size_t)((unsigned __int128)len * (unsigned)i / (unsigned)nch);

    // String state at each boundary: guessed locally (no pass over the input), or exact from
    // phase A's quote parity when a guess is ambiguous or a guessed parse failed.
    static int nospec = -1, flip = 0;
    if (nospec < 0) { nospec = getenv("PJ_NOSPEC") != 0; flip = getenv("PJ_SPECFLIP") != 0; } // testing
    // (guessing is serial: only worth it when phase A would stream a large input from DRAM)
    spec = nch > 1 && !exact && !nospec && (len >= SPEC_MIN || flip || chunk_bytes != CHUNK_BYTES);
    for (int i = 1; i < nch && spec; i++) {
        int g = guess_in_str(buf, len, J.bnd[i]);
        if (g < 0) spec = false;
        J.parity[i] = (uint8_t)(g ^ flip); // here: the guessed state itself
    }
    if (nch > 1 && !spec) pool_run(phase_a, &J, nch);
    T1 = tnow();

    // chunk start positions
    s = malloc((size_t)(nch + 1) * sizeof(size_t));
    s[0] = 0; s[nch] = len;
    int ins = 0;
    for (int i = 1; i < nch; i++) {
        if (spec) ins = J.parity[i]; else ins ^= J.parity[i - 1];
        s[i] = find_start(buf, len, J.bnd[i], ins);
        if (s[i] < s[i - 1]) s[i] = s[i - 1];
    }
    // regions
    uint64_t tw = 0; size_t sb = 0;
    for (int i = 0; i < nch; i++) {
        chunk_t *C = &J.ch[i];
        size_t n = s[i + 1] - s[i];
        C->beg = s[i]; C->end = s[i + 1];
        C->t0 = tw; C->tcap = 2 * (uint64_t)n + 64;
        tw += C->tcap;
        C->next_region = tw;
        sb += 2 * n + 64;
    }
    tape_bytes = (size_t)(tw + 8) * 8;
    sarena_bytes = sb + 4096;
    T = map_get(&tape_bytes);
    SA = map_get(&sarena_bytes);
    map_hugepages(T, tape_bytes, len >= (16u << 20) && (nch == 1 || len / (size_t)nch >= (1u << 20)));
    map_hugepages(SA, sarena_bytes, false);
#ifdef PJ_POISON
    { // debug: recycled memory full of far JUMPs makes reads of unwritten words crash
        for (size_t i = 0; i < tape_bytes / 8; i++) T[i] = W(PJ_TAG_JUMP, 1ull << 46);
        memset(SA, 0xA5, sarena_bytes);
    }
#endif
    {
        size_t off = 0;
        for (int i = 0; i < nch; i++) { J.ch[i].S = SA + off; off += 2 * (s[i + 1] - s[i]) + 64; }
    }
    T[tw] = W(PJ_TAG_CONST, 0); // terminator: the last region's JUMP points here
    J.T = T;
    J.w = calloc((size_t)nw, sizeof(wctx));
    J.w_init = calloc((size_t)nw, 1);

    T2 = tnow();
    if (nch > 1) pool_run(phase_b, &J, nch);
    else phase_b(&J, 0, 0);
    }
    uint64_t T3 = tnow();
    if (pool) pthread_mutex_unlock(&P.busy);

    doc_priv *D = calloc(1, sizeof *D);
    D->tape = T; D->tape_bytes = tape_bytes;
    D->sarena = SA; D->sarena_bytes = sarena_bytes;
    D->w = J.w; D->nw = nw;
    D->map = map; D->map_bytes = map_bytes;
    for (int i = 0; i < nw; i++) if (!J.w_init[i]) wctx_get(&J.w[i]);
    D->pub.tape = T; D->pub.root = 0; D->pub.priv = D; D->pub.input = (const char *)buf;

    // merge
    bool ok = true;
    gframe *gs = malloc(sizeof(gframe) * (MAX_DEPTH + 2));
    int gd = 0;
    uint8_t prev_state = ST_NONE;
    for (int i = 0; i < nch && ok; i++) {
        chunk_t *C = &J.ch[i];
        if (C->err) { ok = false; err->code = 1; err->pos = C->errpos; err->msg = C->msg; break; }
        if (i == 0) {
            gs[0] = (gframe){K_DOC, C->uk_count, 0, 0, 0};
        } else {
            if (C->empty) continue;
            if (!state_ok(prev_state, C->c0)) { ok = false; err->code = 2; err->pos = C->beg; err->msg = "invalid structure at chunk seam"; break; }
            for (int r = 0; r < C->nclosed; r++) {
                closed_rec *cr = &C->closed[r];
                gframe *g = &gs[gd];
                if (gd == 0 || g->type != cr->type) { ok = false; err->code = 2; err->pos = C->beg; err->msg = "mismatched closer"; break; }
                g->count += cr->count;
                if (g->type == K_ARR) { T[g->hdr] = W(PJ_TAG_ARR, cr->end); T[g->hdr + 1] = g->count; }
                else { SHAPE_FINAL(g->ar, g->shape); T[g->hdr] = W(PJ_TAG_OBJD, cr->end); T[cr->trailer] = shape_word(g->shape); }
                gd--;
            }
            if (!ok) break;
            gframe *g = &gs[gd];
            if (C->uk_type && C->uk_type != g->type) { ok = false; err->code = 2; err->pos = C->beg; err->msg = "container type mismatch"; break; }
            g->count += C->uk_count;
        }
        for (int o = 0; o < C->nopen; o++) {
            if (gd + 1 >= MAX_DEPTH) { ok = false; break; }
            open_rec *orc = &C->open[o];
            gs[++gd] = (gframe){orc->type, orc->count, orc->hdr, orc->shape, &J.w[C->wi].ar};
        }
        prev_state = C->end_state;
    }
    if (ok && (gd != 0 || gs[0].count != 1 || prev_state != ST_AFTER_VALUE)) {
        ok = false; err->code = 3; err->pos = len; err->msg = gs[0].count == 0 ? "empty or truncated document" : "unexpected content";
    }
    free(gs);
    if (g_trace) {
        for (int i = 0; i < nw; i++) { fprintf(stderr, "  w%d: %.2fms %llu chunks\n", i, g_wbusy[i] / 1e6, g_wcnt[i]); g_wbusy[i] = g_wcnt[i] = 0; }
    }
    if (g_trace) fprintf(stderr, "pj: %s nch=%d A=%.2fms prep=%.2fms B=%.2fms merge=%.2fms\n", exact ? "exact-retry" : spec ? "guessed" : "exact", nch, (T1 - T0) / 1e6, (T2 - T1) / 1e6, (T3 - T2) / 1e6, (tnow() - T3) / 1e6);
    for (int i = 0; i < nch; i++) { free(J.ch[i].closed); free(J.ch[i].open); }
    free(J.ch); free(J.bnd); free(J.parity); free(J.w_init); free(s);
    if (!ok) {
        if (spec) D->map = 0; // keep the input mapping for the retry
        pj_free(&D->pub);
        if (spec) return parse_impl(buf, len, nthreads, err, map, map_bytes, true); // a guess was wrong (or the input is invalid)
        return 0;
    }
    D->pub.root = pj__fix(T, 0, UINT64_MAX); // chunk 0 may hold only leading whitespace
    return &D->pub;
}

pj_doc *pj_parse(const char *buf, size_t len, int nthreads, pj_err *err) {
    return parse_impl((const uint8_t *)buf, len, nthreads, err, 0, 0, false);
}

pj_doc *pj_parse_file(const char *path, int nthreads, pj_err *err) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (err) { err->code = 4; err->msg = "open failed"; } return 0; }
    struct stat st;
    fstat(fd, &st);
    size_t len = (size_t)st.st_size;
    size_t pg = (size_t)getpagesize();
    size_t flen = (len + pg - 1) & ~(pg - 1);
    size_t total = flen + pg; // trailing anonymous zero page = padding
    uint8_t *base = mmap(0, total, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (len && mmap(base, len, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        close(fd); munmap(base, total);
        if (err) { err->code = 4; err->msg = "mmap failed"; }
        return 0;
    }
    close(fd);
    pj_doc *d = parse_impl(base, len, nthreads, err, base, total, false);
    if (!d) munmap(base, total);
    return d;
}

// ---- slow accessors ------------------------------------------------------------

uint64_t pj__iter_resync(pj_iter *it, uint64_t p) {
    p = pj__fix(it->t, p, it->end);
    if (p < it->end) it->stride = pj__skip(it->t, p) - p;
    return p;
}

static inline bool key_word_eq(const pj_doc *d, uint64_t w, const pj_qkey *k) {
    if (k->len <= 6) return w == k->inl;
    if (PJ_TAG(w) == PJ_TAG_REF) return (w & 0xFFFF) == k->len && !memcmp(d->input + (PJ_PAYLOAD(w) >> 16), k->s, k->len);
    if (PJ_TAG(w) != PJ_TAG_STR) return false;
    const uint32_t *h = (const uint32_t *)PJ_PAYLOAD(w);
    return *h == k->len && !memcmp(h + 1, k->s, k->len);
}

bool pj__obj_get_deep(const pj_doc *d, pj_val o, pj_qkey *k, pj_val *out) {
    const uint64_t *t = d->tape;
    uint64_t w = t[o];
    if (PJ_TAG(w) != PJ_TAG_OBJD) return false;
    uint64_t end = PJ_PAYLOAD(w);
    uint64_t sw = t[end - 1];
    const pj_shape *sh = pj__shape(sw);
    if (sh != k->cs) { k->cs = sh; k->slot = pj__shape_find(sh, k); }
    uint64_t p = o + 1;
    if (k->slot >= 0) {
        for (int i = 0; i < k->slot; i++) p = pj__skip(t, pj__fix(t, p, end));
        *out = pj__fix(t, p, end);
        return true;
    }
    for (uint32_t i = 0; i < sh->n; i++) p = pj__skip(t, pj__fix(t, p, end));
    for (;;) {
        p = pj__fix(t, p, end - 1);
        if (p >= end - 1) return false;
        uint64_t kw = t[p];
        uint64_t v = pj__fix(t, p + 1, end);
        if (key_word_eq(d, kw, k)) { *out = v; return true; }
        p = pj__skip(t, v);
    }
}

bool pj_obj_iter_init(const pj_doc *d, pj_val o, pj_obj_iter *it) {
    uint64_t w = d->tape[o];
    it->d = d; it->i = 0; it->pos = o + 1;
    if (PJ_TAG(w) == PJ_TAG_OBJF) { it->sh = pj__shape(w); it->end = o + 1 + it->sh->n; return true; }
    if (PJ_TAG(w) == PJ_TAG_OBJD) { it->end = PJ_PAYLOAD(w) - 1; it->sh = pj__shape(d->tape[it->end]); return true; }
    it->sh = 0; it->end = 0;
    return false;
}

bool pj_obj_iter_next(pj_obj_iter *it, pj_str *key, pj_val *val) {
    const uint64_t *t = it->d->tape;
    if (!it->sh) return false;
    uint64_t p = pj__fix(t, it->pos, it->end);
    if (it->i < it->sh->n) {
        const pj_key *k = &it->sh->keys[it->i++];
        key->s = k->s; key->len = k->len;
        *val = p;
        it->pos = pj__skip(t, p);
        return true;
    }
    if (p >= it->end) return false;
    *key = pj_get_str(it->d, p);
    uint64_t v = pj__fix(t, p + 1, it->end + 1);
    *val = v;
    it->pos = pj__skip(t, v);
    return true;
}

size_t pj_obj_size(const pj_doc *d, pj_val o) {
    pj_obj_iter it; pj_str k; pj_val v; size_t n = 0;
    if (!pj_obj_iter_init(d, o, &it)) return 0;
    while (pj_obj_iter_next(&it, &k, &v)) n++;
    return n;
}
