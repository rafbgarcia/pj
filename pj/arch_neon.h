// pj SIMD layer: AArch64 NEON. See arch.h for the contract.
#include <arm_neon.h>

#define PJ_ARCH_NAME "neon"
#define SCANW 16  // bytes per str_mask/ws_mask step
#define MSHIFT 2  // str_mask/ws_mask: 4 bits per byte (ctz >> MSHIFT = byte index)

INLINE void cpu_relax(void) { __asm__ volatile("yield"); }

INLINE uint64_t nib_mask(uint8x16_t m) { return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0); }

INLINE uint64_t movemask64(uint8x16_t a, uint8x16_t b, uint8x16_t c, uint8x16_t d) {
    const uint8x16_t bit = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    a = vandq_u8(a, bit); b = vandq_u8(b, bit); c = vandq_u8(c, bit); d = vandq_u8(d, bit);
    uint8x16_t s0 = vpaddq_u8(a, b), s1 = vpaddq_u8(c, d);
    s0 = vpaddq_u8(s0, s1);
    s0 = vpaddq_u8(s0, s0);
    return vgetq_lane_u64(vreinterpretq_u64_u8(s0), 0);
}

// ---- 64-byte blocks ----------------------------------------------------------

typedef struct { uint8x16_t v[4]; } blk64;

INLINE blk64 blk_load(const uint8_t *s) { return (blk64){{vld1q_u8(s), vld1q_u8(s + 16), vld1q_u8(s + 32), vld1q_u8(s + 48)}}; }

INLINE uint64_t blk_eq(blk64 b, uint8_t c) {
    uint8x16_t k = vdupq_n_u8(c);
    return movemask64(vceqq_u8(b.v[0], k), vceqq_u8(b.v[1], k), vceqq_u8(b.v[2], k), vceqq_u8(b.v[3], k));
}

// ',' '}' ']' ('}' == ']' | 0x20)
INLINE uint64_t blk_term(blk64 b) {
    const uint8x16_t k20 = vdupq_n_u8(0x20), kc = vdupq_n_u8(','), kb = vdupq_n_u8('}');
#define TM(v) vorrq_u8(vceqq_u8(v, kc), vceqq_u8(vorrq_u8(v, k20), kb))
    return movemask64(TM(b.v[0]), TM(b.v[1]), TM(b.v[2]), TM(b.v[3]));
#undef TM
}

// whitespace: table[c & 15] == c only for ' ', '\t', '\n', '\r'
INLINE uint64_t blk_ws(blk64 b) {
    const uint8x16_t t = {' ', 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, '\t', '\n', 0x80, 0x80, '\r', 0x80, 0x80};
    const uint8x16_t lo = vdupq_n_u8(15);
#define WS(v) vceqq_u8(vqtbl1q_u8(t, vandq_u8(v, lo)), v)
    return movemask64(WS(b.v[0]), WS(b.v[1]), WS(b.v[2]), WS(b.v[3]));
#undef WS
}

// ---- short scans ---------------------------------------------------------------

// '"', '\\' or control char in p[0..SCANW)
INLINE uint64_t str_mask(const uint8_t *p) {
    uint8x16_t v = vld1q_u8(p);
    return nib_mask(vorrq_u8(vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))), vcltq_u8(v, vdupq_n_u8(0x20))));
}
// non-whitespace in p[0..SCANW)
INLINE uint64_t nonws_mask(const uint8_t *p) {
    uint8x16_t v = vld1q_u8(p);
    uint8x16_t ws = vorrq_u8(vorrq_u8(vceqq_u8(v, vdupq_n_u8(' ')), vceqq_u8(v, vdupq_n_u8('\n'))),
                             vorrq_u8(vceqq_u8(v, vdupq_n_u8('\r')), vceqq_u8(v, vdupq_n_u8('\t'))));
    return ~nib_mask(ws);
}

// len-byte equality, 9 <= len <= 32, both sides readable for 32 bytes
INLINE bool key_eq32(const uint8_t *a, const uint8_t *b, uint32_t len) {
    const uint8x16_t iota0 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8x16_t iota1 = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    uint8x16_t L = vdupq_n_u8((uint8_t)len);
    uint8x16_t x0 = vandq_u8(veorq_u8(vld1q_u8(a), vld1q_u8(b)), vcltq_u8(iota0, L));
    uint8x16_t x1 = vandq_u8(veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16)), vcltq_u8(iota1, L));
    return vmaxvq_u8(vorrq_u8(x0, x1)) == 0;
}

INLINE bool pat_eq(const uint8_t *p, const uint8_t *pat, uint32_t pm, uint32_t plen) { (void)pm; return key_eq32(pat, p, plen); }

// ---- digits ----------------------------------------------------------------------

typedef struct { uint8x16_t d0, d1; uint64_t m0, m1; } dig32;

INLINE unsigned ctz128_nib(uint64_t lo, uint64_t hi) { // index of first set nibble in hi:lo (32 if none)
    return lo ? (unsigned)__builtin_ctzll(lo) >> 2 : hi ? 16 + ((unsigned)__builtin_ctzll(hi) >> 2) : 32;
}

// Loads p[0..32) as digit values; returns the number of leading digits (0..32).
INLINE unsigned dig_scan(const uint8_t *p, dig32 *D) {
    const uint8x16_t z = vdupq_n_u8('0'), ten = vdupq_n_u8(10);
    D->d0 = vsubq_u8(vld1q_u8(p), z); D->d1 = vsubq_u8(vld1q_u8(p + 16), z);
    D->m0 = nib_mask(vcgeq_u8(D->d0, ten)); D->m1 = nib_mask(vcgeq_u8(D->d1, ten));
    return ctz128_nib(D->m0, D->m1);
}
// dig_scan for callers that reject more than 14 leading digits: the count comes from p[0..16)
// only, and is 15 when those are all digits (the stop bit in the last nibble), so no second
// ctz and no selects.
INLINE unsigned dig_scan_lo(const uint8_t *p, dig32 *D) {
    dig_scan(p, D);
    return (unsigned)__builtin_ctzll(D->m0 | (1ull << 63)) >> 2;
}
// Non-digit bytes in [0, len): compare lanes are 0 or 0xFF (-1), so their sum is -count.
#define PJ_DIGCNT 1
INLINE unsigned dig_nd_count(const dig32 *D, unsigned len) {
    const uint8x16_t iota0 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8x16_t iota1 = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    const uint8x16_t ten = vdupq_n_u8(10), L = vdupq_n_u8((uint8_t)len);
    uint8x16_t x = vaddq_u8(vandq_u8(vcgeq_u8(D->d0, ten), vcltq_u8(iota0, L)), vandq_u8(vcgeq_u8(D->d1, ten), vcltq_u8(iota1, L)));
    return (uint8_t)-vaddvq_u8(x);
}
// dig_nd_count(D, len) == 1, compared as the raw lane sum (-1 = 0xFF): no neg/and.
INLINE bool dig_nd_one(const dig32 *D, unsigned len) {
    const uint8x16_t iota0 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8x16_t iota1 = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    const uint8x16_t ten = vdupq_n_u8(10), L = vdupq_n_u8((uint8_t)len);
    uint8x16_t x = vaddq_u8(vandq_u8(vcgeq_u8(D->d0, ten), vcltq_u8(iota0, L)), vandq_u8(vcgeq_u8(D->d1, ten), vcltq_u8(iota1, L)));
    return vaddvq_u8(x) == 0xFF;
}
// Digits in a row starting at n1 + 1 (n1 <= 19).
INLINE unsigned dig_n2(const dig32 *D, unsigned n1) {
    __uint128_t M = (((__uint128_t)D->m1 << 64) | D->m0) >> (4 * (n1 + 1));
    return ctz128_nib((uint64_t)M, (uint64_t)(M >> 64));
}

INLINE uint64_t lanes16_value(uint8x16_t a) {
    const uint8x16_t w10 = {10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1};
    const uint16x8_t w100 = {100, 1, 100, 1, 100, 1, 100, 1};
    const uint32x4_t w1e4 = {10000, 1, 10000, 1};
    uint16x8_t a16 = vpaddlq_u8(vmulq_u8(a, w10));
    uint32x4_t a32 = vpaddlq_u16(vmulq_u16(a16, w100));
    uint64x2_t a64 = vpaddlq_u32(vmulq_u32(a32, w1e4));
    return vgetq_lane_u64(a64, 0) * 100000000ull + vgetq_lane_u64(a64, 1);
}

// n digits (n1 before the '.', the rest after it) -> integer, via the TBL index table.
INLINE uint64_t dig_value(const dig32 *D, unsigned n, unsigned n1) {
    const uint8_t *ix = g_dig_idx[n1][n];
    uint8x16x2_t src = {{D->d0, D->d1}};
    uint64_t hi = lanes16_value(vqtbl2q_u8(src, vld1q_u8(ix))), lo = lanes16_value(vqtbl2q_u8(src, vld1q_u8(ix + 16)));
    // Opaque lo keeps the madd tree 2 deep: clang 18 reassociated it into a chain of 3.
    __asm__("" : "+r"(lo));
    return hi * 10000000000000000ull + lo;
}

#define PJ_INTLOAD 1
// Integer of len (1..14) digits starting at D's first lane: one TBL right-aligns them in 16 lanes
// (out-of-range indices give 0), so no index-table load and no high half.
INLINE uint64_t int_value(const dig32 *D, unsigned len) {
    const uint8x16_t iota = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    return lanes16_value(vqtbl1q_u8(D->d0, vaddq_u8(iota, vdupq_n_u8((uint8_t)(len - 16)))));
}

// ---- UTF-8 validation (Keiser & Lemire lookup, as in simdjson) ------------------------

typedef struct { uint8x16_t err, prev, prev_incomplete; } u8chk;

INLINE void u8_init(u8chk *u) { u->err = u->prev = u->prev_incomplete = vdupq_n_u8(0); }

INLINE void u8_check16(u8chk *u, uint8x16_t in) {
    uint8x16_t prev1 = vextq_u8(u->prev, in, 15), prev2 = vextq_u8(u->prev, in, 14), prev3 = vextq_u8(u->prev, in, 13);
    uint8x16_t sc = vandq_u8(vandq_u8(vqtbl1q_u8(vld1q_u8(U8_B1H), vshrq_n_u8(prev1, 4)), vqtbl1q_u8(vld1q_u8(U8_B1L), vandq_u8(prev1, vdupq_n_u8(15)))),
                             vqtbl1q_u8(vld1q_u8(U8_B2H), vshrq_n_u8(in, 4)));
    uint8x16_t must23 = vorrq_u8(vqsubq_u8(prev2, vdupq_n_u8(0xE0 - 0x80)), vqsubq_u8(prev3, vdupq_n_u8(0xF0 - 0x80)));
    u->err = vorrq_u8(u->err, veorq_u8(vandq_u8(must23, vdupq_n_u8(0x80)), sc));
    u->prev = in;
}

INLINE void u8_block(u8chk *u, blk64 b) {
    uint8x16_t any = vorrq_u8(vorrq_u8(b.v[0], b.v[1]), vorrq_u8(b.v[2], b.v[3]));
    if (LIKELY(vmaxvq_u8(any) < 0x80)) { u->err = vorrq_u8(u->err, u->prev_incomplete); u->prev = b.v[3]; u->prev_incomplete = vdupq_n_u8(0); return; }
    u8_check16(u, b.v[0]); u8_check16(u, b.v[1]); u8_check16(u, b.v[2]); u8_check16(u, b.v[3]);
    const uint8x16_t maxv = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0xF0 - 1, 0xE0 - 1, 0xC0 - 1};
    u->prev_incomplete = vcgtq_u8(b.v[3], maxv);
}

INLINE bool u8_ok(u8chk *u) { return vmaxvq_u8(vorrq_u8(u->err, u->prev_incomplete)) == 0; }
