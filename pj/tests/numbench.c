// Microbench: number conversion alone (num_known with known extents), over all numbers after ':'.
#include "../pj.c"
#include <time.h>
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = calloc(1, n + 64); fread(b, 1, n, f); fclose(f);
    size_t cap = n / 8, k = 0; uint32_t *pos = malloc(cap * 4); uint8_t *len = malloc(cap);
    for (size_t i = 0; i < n; i++) if (b[i] == ':') { size_t j = i + 1; while (b[j] != ',' && b[j] != '}') j++; pos[k] = (uint32_t)(i + 1); len[k++] = (uint8_t)(j - i - 1); }
    uint64_t *out = malloc(k * 8);
    pthread_once(&g_dig_once, dig_idx_init);
    for (int r = 0; r < 5; r++) {
        uint64_t a = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        size_t fails = 0;
        for (size_t i = 0; i < k; i++) fails += !num_known(b + pos[i], len[i], &out[i]);
        uint64_t z = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (r >= 3) printf("num_known: %zu numbers, %.2f ms, %.2f ns/num, fails %zu\n", k, (z - a) / 1e6, (z - a) / (double)k, fails);
    }
    size_t bad = 0;
    for (size_t i = 0; i < k; i++) { double d; memcpy(&d, &out[i], 8); double e = strtod((char *)b + pos[i], 0); if (d != e) { if (bad < 5) printf("mismatch at %u: %.17g vs %.17g\n", pos[i], d, e); bad++; } }
    printf("mismatches: %zu\n", bad);
}
