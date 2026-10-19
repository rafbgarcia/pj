#include "../pj.c"
int main(int argc, char **argv) {
    size_t k = 20000000; uint64_t *m = malloc(k * 8); uint8_t *e = malloc(k); uint64_t *o = malloc(k * 8);
    uint64_t x = 88172645463325252ull;
    for (size_t i = 0; i < k; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; m[i] = 1000000000000000ull + x % 90000000000000000ull; e[i] = 14 + (x >> 60) % 3; }
    for (int r = 0; r < 4; r++) {
        uint64_t a = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (size_t i = 0; i < k; i++) { double d; el_neg_small(e[i], m[i], false, &d); memcpy(&o[i], &d, 8); }
        uint64_t z = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t a2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (size_t i = 0; i < k; i++) { double d = (double)m[i] / P10[e[i]]; memcpy(&o[i], &d, 8); }
        uint64_t z2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        printf("EL %.2f ns/num   clinger-div %.2f ns/num\n", (z - a) / (double)k, (z2 - a2) / (double)k);
    }
}
