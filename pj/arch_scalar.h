// pj SIMD layer: portable scalar fallback (any 64-bit little-endian target). See arch.h.

#define PJ_ARCH_NAME "scalar"
#define SCANW 8
#define MSHIFT 3 // one byte (8 bits) per input byte

INLINE void cpu_relax(void) {}

typedef struct { const uint8_t *s; } blk64;
INLINE blk64 blk_load(const uint8_t *s) { return (blk64){s}; }
INLINE uint64_t blk_eq(blk64 b, uint8_t c) {
    uint64_t m = 0;
    for (int i = 0; i < 64; i++) m |= (uint64_t)(b.s[i] == c) << i;
    return m;
}
INLINE uint64_t blk_term(blk64 b) {
    uint64_t m = 0;
    for (int i = 0; i < 64; i++) { uint8_t c = b.s[i]; m |= (uint64_t)(c == ',' || (c | 0x20) == '}') << i; }
    return m;
}

INLINE uint64_t blk_ws(blk64 b) {
    uint64_t m = 0;
    for (int i = 0; i < 64; i++) { uint8_t c = b.s[i]; m |= (uint64_t)(c == ' ' || c == '\n' || c == '\r' || c == '\t') << i; }
    return m;
}

// SWAR: high bit of each byte set where the byte matches.
INLINE uint64_t swar_eq(uint64_t v, uint8_t c) {
    uint64_t x = v ^ (0x0101010101010101ull * c);
    return ~(((x & 0x7F7F7F7F7F7F7F7Full) + 0x7F7F7F7F7F7F7F7Full) | x) & 0x8080808080808080ull;
}
INLINE uint64_t swar_lt(uint64_t v, uint8_t c) { // bytes < c (c <= 128)
    return ~(((v & 0x7F7F7F7F7F7F7F7Full) + 0x0101010101010101ull * (uint8_t)(0x80 - c)) | v) & 0x8080808080808080ull;
}
INLINE uint64_t str_mask(const uint8_t *p) { uint64_t v = load8(p); return swar_eq(v, '"') | swar_eq(v, '\\') | swar_lt(v, 0x20); }
INLINE uint64_t nonws_mask(const uint8_t *p) {
    uint64_t v = load8(p);
    return ~(swar_eq(v, ' ') | swar_eq(v, '\n') | swar_eq(v, '\r') | swar_eq(v, '\t')) & 0x8080808080808080ull;
}

INLINE bool key_eq32(const uint8_t *a, const uint8_t *b, uint32_t len) { return !memcmp(a, b, len); }
INLINE bool pat_eq(const uint8_t *p, const uint8_t *pat, uint32_t pm, uint32_t plen) { (void)pm; return key_eq32(pat, p, plen); }

typedef struct { const uint8_t *p; } dig32;
INLINE unsigned dig_scan(const uint8_t *p, dig32 *D) {
    D->p = p;
    unsigned n = 0;
    while (n < 32 && is_digit(p[n])) n++;
    return n;
}
INLINE unsigned dig_n2(const dig32 *D, unsigned n1) {
    unsigned n = 0;
    while (n1 + 1 + n < 32 && is_digit(D->p[n1 + 1 + n])) n++;
    return n;
}
INLINE uint64_t dig_value(const dig32 *D, unsigned n, unsigned n1) {
    uint64_t m = 0;
    for (unsigned i = 0; i < n1; i++) m = m * 10 + (uint64_t)(D->p[i] - '0');
    for (unsigned i = 0; i < n - n1; i++) m = m * 10 + (uint64_t)(D->p[n1 + 1 + i] - '0');
    return m;
}

typedef struct { unsigned need; uint8_t lo, hi; bool err; } u8chk;
INLINE void u8_init(u8chk *u) { u->need = 0; u->lo = 0x80; u->hi = 0xBF; u->err = false; }
static void u8_block(u8chk *u, blk64 b) {
    for (int i = 0; i < 64; i++) {
        uint8_t c = b.s[i];
        if (u->need) {
            if (c < u->lo || c > u->hi) u->err = true;
            u->lo = 0x80; u->hi = 0xBF; u->need--;
            continue;
        }
        if (c < 0x80) continue;
        if (c >= 0xC2 && c <= 0xDF) u->need = 1;
        else if (c == 0xE0) { u->need = 2; u->lo = 0xA0; }
        else if (c == 0xED) { u->need = 2; u->hi = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) u->need = 2;
        else if (c == 0xF0) { u->need = 3; u->lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) u->need = 3;
        else if (c == 0xF4) { u->need = 3; u->hi = 0x8F; }
        else u->err = true;
    }
}
INLINE bool u8_ok(u8chk *u) { return !u->err && !u->need; }
