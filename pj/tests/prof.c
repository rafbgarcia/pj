// Profiling driver: parse <file> <reps> times with <threads> (1 = single-threaded).
#include <stdio.h>
#include <stdlib.h>
#include "../pj.h"
#ifdef __APPLE__
#include <libproc.h>
#include <unistd.h>
static void counters(uint64_t *cyc, uint64_t *ins) { // per-process, no root needed
    struct rusage_info_v4 ri;
    proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri);
    *cyc = ri.ri_cycles; *ins = ri.ri_instructions;
}
#else
static void counters(uint64_t *cyc, uint64_t *ins) { *cyc = *ins = 0; }
#endif
uint64_t tnow(void);
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: prof <file> <reps> [threads]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    char *b = calloc(1, n + 64); if (fread(b, 1, n, f) != n) return 1; fclose(f);
    int reps = atoi(argv[2]), nt = argc > 3 ? atoi(argv[3]) : 1;
    uint64_t best = ~0ull, c0, i0, c1, i1;
    counters(&c0, &i0);
    for (int r = 0; r < reps; r++) {
        uint64_t t0 = tnow();
        pj_doc *d = pj_parse(b, n, nt, 0);
        uint64_t t = tnow() - t0;
        if (!d) { fprintf(stderr, "parse failed\n"); return 1; }
        pj_free(d);
        if (t < best) best = t;
    }
    counters(&c1, &i1);
    if (c1 > c0) printf("%s: best %.5f ms, %.2f GB/s, %.3g cycles/parse, %.3g instr/parse, IPC %.2f\n", argv[1], best / 1e6, n / (double)best,
                        (double)(c1 - c0) / reps, (double)(i1 - i0) / reps, (double)(i1 - i0) / (double)(c1 - c0));
    else printf("%s: best %.5f ms, %.2f GB/s\n", argv[1], best / 1e6, n / (double)best);
    return 0;
}
