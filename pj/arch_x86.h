// pj SIMD layer: x86-64 AVX2, with AVX-512 (BW/VL, optionally VBMI) paths. See arch.h.
#include <immintrin.h>

#if defined(__AVX512BW__) && defined(__AVX512VL__)
#define PJ_AVX512 1
#define PJ_ARCH_NAME (PJ_HAVE_VBMI ? "avx512vbmi" : "avx512")
#else
#define PJ_AVX512 0
#define PJ_ARCH_NAME "avx2"
#endif
#ifdef __AVX512VBMI__
#define PJ_HAVE_VBMI 1
#else
#define PJ_HAVE_VBMI 0
#endif

#ifndef PJ_SCAN64
#define PJ_SCAN64 0 // 64B scans: Zen 5 twitter_x150 +3%, Intel records +6% (strings are short)
#endif
#if PJ_SCAN64
#define SCANW 64 // bytes per str_mask/ws_mask step
#else
#define SCANW 32
#endif
#define MSHIFT 0 // one bit per byte

INLINE void cpu_relax(void) { _mm_pause(); }

#define LD256(p) _mm256_loadu_si256((const __m256i *)(p))
#define B8(c) _mm256_set1_epi8((char)(c))
INLINE uint32_t mm(__m256i v) { return (uint32_t)_mm256_movemask_epi8(v); }

// ---- 64-byte blocks ----------------------------------------------------------

#if PJ_AVX512
typedef struct { __m512i v; } blk64;
INLINE blk64 blk_load(const uint8_t *s) { return (blk64){_mm512_loadu_si512((const void *)s)}; }
INLINE uint64_t blk_eq(blk64 b, uint8_t c) { return _mm512_cmpeq_epi8_mask(b.v, _mm512_set1_epi8((char)c)); }
INLINE uint64_t blk_term(blk64 b) {
    return _mm512_cmpeq_epi8_mask(b.v, _mm512_set1_epi8(',')) |
           _mm512_cmpeq_epi8_mask(_mm512_or_si512(b.v, _mm512_set1_epi8(0x20)), _mm512_set1_epi8('}'));
}
INLINE uint64_t blk_ws(blk64 b) { // table[c & 15] == c only for ws; bytes >= 0x80 shuffle to 0
    const __m512i t = _mm512_broadcast_i32x4(_mm_setr_epi8(' ', 1, 1, 1, 1, 1, 1, 1, 1, '\t', '\n', 1, 1, '\r', 1, 1));
    return _mm512_cmpeq_epi8_mask(_mm512_shuffle_epi8(t, b.v), b.v);
}
#else
typedef struct { __m256i a, b; } blk64;
INLINE blk64 blk_load(const uint8_t *s) { return (blk64){LD256(s), LD256(s + 32)}; }
INLINE uint64_t blk_eq(blk64 b, uint8_t c) {
    __m256i k = B8(c);
    return mm(_mm256_cmpeq_epi8(b.a, k)) | ((uint64_t)mm(_mm256_cmpeq_epi8(b.b, k)) << 32);
}
INLINE uint64_t blk_term(blk64 b) {
    const __m256i kc = B8(','), kb = B8('}'), k20 = B8(0x20);
#define TM(v) mm(_mm256_or_si256(_mm256_cmpeq_epi8(v, kc), _mm256_cmpeq_epi8(_mm256_or_si256(v, k20), kb)))
    return TM(b.a) | ((uint64_t)TM(b.b) << 32);
#undef TM
}
INLINE uint64_t blk_ws(blk64 b) { // table[c & 15] == c only for ws; bytes >= 0x80 shuffle to 0
    const __m256i t = _mm256_setr_epi8(' ', 1, 1, 1, 1, 1, 1, 1, 1, '\t', '\n', 1, 1, '\r', 1, 1, ' ', 1, 1, 1, 1, 1, 1, 1, 1, '\t', '\n', 1, 1, '\r', 1, 1);
    return mm(_mm256_cmpeq_epi8(_mm256_shuffle_epi8(t, b.a), b.a)) | ((uint64_t)mm(_mm256_cmpeq_epi8(_mm256_shuffle_epi8(t, b.b), b.b)) << 32);
}
#endif

// ---- short scans ---------------------------------------------------------------

#if PJ_SCAN64
INLINE uint64_t str_mask(const uint8_t *p) { // '"', '\\' or control char in p[0..64)
    __m512i v = _mm512_loadu_si512((const void *)p);
    return _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('"')) | _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\\')) |
           _mm512_cmple_epu8_mask(v, _mm512_set1_epi8(0x1F));
}
INLINE uint64_t nonws_mask(const uint8_t *p) { return ~blk_ws(blk_load(p)); }
#else
INLINE uint64_t str_mask(const uint8_t *p) {
    __m256i v = LD256(p);
    __m256i ctl = _mm256_cmpeq_epi8(_mm256_max_epu8(v, B8(0x1F)), B8(0x1F));
    return mm(_mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, B8('"')), _mm256_cmpeq_epi8(v, B8('\\'))), ctl));
}
INLINE uint64_t nonws_mask(const uint8_t *p) { // table[c & 15] == c only for ws (as blk_ws): 2 ops
    const __m256i t = _mm256_setr_epi8(' ', 1, 1, 1, 1, 1, 1, 1, 1, '\t', '\n', 1, 1, '\r', 1, 1, ' ', 1, 1, 1, 1, 1, 1, 1, 1, '\t', '\n', 1, 1, '\r', 1, 1);
    __m256i v = LD256(p);
    return (uint32_t)~mm(_mm256_cmpeq_epi8(_mm256_shuffle_epi8(t, v), v));
}
#endif

INLINE bool key_eq32(const uint8_t *a, const uint8_t *b, uint32_t len) {
    uint32_t ne = ~mm(_mm256_cmpeq_epi8(LD256(a), LD256(b)));
    uint32_t lm = len >= 32 ? ~0u : (1u << len) - 1;
    return (ne & lm) == 0;
}

INLINE bool pat_eq(const uint8_t *p, const uint8_t *pat, uint32_t pm, uint32_t plen) { // bytes in mask pm equal
    (void)plen;
#if PJ_AVX512
    return _mm256_mask_cmpneq_epi8_mask(pm, LD256(p), LD256(pat)) == 0;
#else
    return (~mm(_mm256_cmpeq_epi8(LD256(p), LD256(pat))) & pm) == 0;
#endif
}

// ---- digits ----------------------------------------------------------------------

typedef struct { __m256i d; uint64_t m; } dig32;

INLINE unsigned dig_scan(const uint8_t *p, dig32 *D) {
    D->d = _mm256_sub_epi8(LD256(p), B8('0'));
    uint32_t isd = mm(_mm256_cmpeq_epi8(_mm256_min_epu8(D->d, B8(9)), D->d));
    D->m = (uint64_t)(uint32_t)~isd | (3ull << 32);
    return (unsigned)__builtin_ctzll(D->m);
}
#ifdef __BMI2__
#define PJ_DIGMASK 1 // D->m is a bit-per-byte non-digit mask (bits 32, 33 set); num_known validates from masks
INLINE uint64_t dig_dots(const dig32 *D) { return mm(_mm256_cmpeq_epi8(D->d, B8('.' - '0'))); }
#endif
INLINE unsigned dig_n2(const dig32 *D, unsigned n1) { return (unsigned)__builtin_ctzll(D->m >> (n1 + 1)); }

// 32 digit lanes (most significant first) -> hi16 * 1e16 + lo16
INLINE uint64_t lanes32_value(__m256i v) {
    __m256i a = _mm256_maddubs_epi16(v, _mm256_set1_epi16(0x010A));   // d0*10 + d1
    __m256i b = _mm256_madd_epi16(a, _mm256_set1_epi32(0x00010064));  // *100 + next
    __m256i c = _mm256_packus_epi32(b, b);
    __m256i d = _mm256_madd_epi16(c, _mm256_set1_epi32(0x00012710));  // *10000 + next: 8-digit groups
    // vpmuludq by hand: with AVX512DQ clang rewrites _mm256_mul_epu32 as vpblendd + vpmullq
    // (3 uops, ~15 cycles on Intel vs 5), right on the number's dependency chain.
    __m256i pr, k8 = _mm256_set1_epi64x(100000000);
    __asm__("vpmuludq %2, %1, %0" : "=x"(pr) : "x"(d), "x"(k8));
    __m256i e = _mm256_add_epi64(pr, _mm256_srli_epi64(d, 32));
    uint64_t hi = (uint64_t)_mm256_extract_epi64(e, 0), lo = (uint64_t)_mm256_extract_epi64(e, 2);
    return hi * 10000000000000000ull + lo;
}

INLINE uint64_t dig_value(const dig32 *D, unsigned n, unsigned n1) {
    __m256i ix = LD256(g_dig_idx[n1][n]);
#if PJ_HAVE_VBMI
    __m256i v = _mm256_maskz_permutexvar_epi8(~_mm256_movepi8_mask(ix), ix, D->d);
#else
    __m256i lo = _mm256_permute2x128_si256(D->d, D->d, 0x00), hi = _mm256_permute2x128_si256(D->d, D->d, 0x11);
    __m256i v = _mm256_or_si256(_mm256_shuffle_epi8(lo, _mm256_adds_epu8(ix, B8(0x70))), _mm256_shuffle_epi8(hi, _mm256_sub_epi8(ix, B8(16))));
#endif
    return lanes32_value(v);
}

#if PJ_AVX512 && PJ_HAVE_VBMI && defined(__BMI2__)
#define PJ_INTLOAD 1
// Integer of len (1..14) digits from dig_scan's first lane: one masked vpermb right-aligns them in
// 16 lanes, so the value waits on no gather index (n1) and has no high half.
INLINE uint64_t int_value(const dig32 *D, unsigned len) {
    __mmask16 kv = (__mmask16)(0xFFFFu << (16 - len));
    __m128i ix = _mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15), _mm_set1_epi8((char)(len - 16)));
    __m128i v = _mm_maskz_permutexvar_epi8(kv, ix, _mm256_castsi256_si128(D->d));
    __m128i a = _mm_maddubs_epi16(v, _mm_set1_epi16(0x010A));
    __m128i b = _mm_madd_epi16(a, _mm_set1_epi32(0x00010064));
    __m128i c = _mm_packus_epi32(b, b);
    __m128i d = _mm_madd_epi16(c, _mm_set1_epi32(0x00012710));
    uint64_t x = (uint64_t)_mm_cvtsi128_si64(d);
    return (uint32_t)x * 100000000ull + (x >> 32);
}
#endif

// ---- UTF-8 validation (Keiser & Lemire lookup, as in simdjson) ------------------------

#if PJ_AVX512
typedef __m512i u8v;
#define U8_W 64
#define U8_SET1(c) _mm512_set1_epi8((char)(c))
#define U8_TBL(t) _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)(t)))
#define U8_LOOKUP(t, i) _mm512_shuffle_epi8(t, i)
#define U8_SHR4(v) _mm512_and_si512(_mm512_srli_epi16(v, 4), U8_SET1(15))
#define U8_AND _mm512_and_si512
#define U8_OR _mm512_or_si512
#define U8_XOR _mm512_xor_si512
#define U8_SUBS _mm512_subs_epu8
INLINE u8v u8_prev(u8v in, u8v prev, int n) {
    u8v sh = _mm512_permutex2var_epi64(prev, _mm512_set_epi64(13, 12, 11, 10, 9, 8, 7, 6), in);
    switch (n) {
    case 1: return _mm512_alignr_epi8(in, sh, 15);
    case 2: return _mm512_alignr_epi8(in, sh, 14);
    default: return _mm512_alignr_epi8(in, sh, 13);
    }
}
INLINE bool u8_nonzero(u8v v) { return _mm512_test_epi8_mask(v, v) != 0; }
#else
typedef __m256i u8v;
#define U8_W 32
#define U8_SET1(c) B8(c)
#define U8_TBL(t) _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(t)))
#define U8_LOOKUP(t, i) _mm256_shuffle_epi8(t, i)
#define U8_SHR4(v) _mm256_and_si256(_mm256_srli_epi16(v, 4), B8(15))
#define U8_AND _mm256_and_si256
#define U8_OR _mm256_or_si256
#define U8_XOR _mm256_xor_si256
#define U8_SUBS _mm256_subs_epu8
INLINE u8v u8_prev(u8v in, u8v prev, int n) {
    u8v sh = _mm256_permute2x128_si256(prev, in, 0x21);
    switch (n) {
    case 1: return _mm256_alignr_epi8(in, sh, 15);
    case 2: return _mm256_alignr_epi8(in, sh, 14);
    default: return _mm256_alignr_epi8(in, sh, 13);
    }
}
INLINE bool u8_nonzero(u8v v) { return !_mm256_testz_si256(v, v); }
#endif

typedef struct { u8v err, prev, prev_incomplete; } u8chk;

INLINE void u8_init(u8chk *u) { u->err = u->prev = u->prev_incomplete = U8_SET1(0); }

INLINE void u8_check(u8chk *u, u8v in) {
    u8v prev1 = u8_prev(in, u->prev, 1), prev2 = u8_prev(in, u->prev, 2), prev3 = u8_prev(in, u->prev, 3);
    u8v sc = U8_AND(U8_AND(U8_LOOKUP(U8_TBL(U8_B1H), U8_SHR4(prev1)), U8_LOOKUP(U8_TBL(U8_B1L), U8_AND(prev1, U8_SET1(15)))),
                    U8_LOOKUP(U8_TBL(U8_B2H), U8_SHR4(in)));
    u8v must23 = U8_OR(U8_SUBS(prev2, U8_SET1(0xE0 - 0x80)), U8_SUBS(prev3, U8_SET1(0xF0 - 0x80)));
    u->err = U8_OR(u->err, U8_XOR(U8_AND(must23, U8_SET1(0x80)), sc));
    u->prev = in;
}

INLINE u8v u8_incomplete(u8v last) {
    static const uint8_t maxv[64] __attribute__((aligned(64))) = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0xF0 - 1, 0xE0 - 1, 0xC0 - 1};
    return U8_SUBS(last, *(const u8v *)(maxv + 64 - U8_W));
}

INLINE void u8_block(u8chk *u, blk64 b) {
#if PJ_AVX512
    if (LIKELY(_mm512_movepi8_mask(b.v) == 0)) { u->err = U8_OR(u->err, u->prev_incomplete); u->prev = b.v; u->prev_incomplete = U8_SET1(0); return; }
    u8_check(u, b.v);
    u->prev_incomplete = u8_incomplete(b.v);
#else
    if (LIKELY(mm(_mm256_or_si256(b.a, b.b)) == 0)) { u->err = U8_OR(u->err, u->prev_incomplete); u->prev = b.b; u->prev_incomplete = U8_SET1(0); return; }
    u8_check(u, b.a); u8_check(u, b.b);
    u->prev_incomplete = u8_incomplete(b.b);
#endif
}

INLINE bool u8_ok(u8chk *u) { return !u8_nonzero(U8_OR(u->err, u->prev_incomplete)); }
