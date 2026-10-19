// Benchmark runner: one parser, one file, one fresh process (so cold and memory numbers are clean).
// Usage: runner <yyjson|simdjson|simdjson-od|pj|pj-1t> <file.json> [budget_seconds]
// Prints one JSON object. All times are wall-clock ns per document.
//   cold   first parse in a fresh process: includes page faults and first-touch allocation
//   parse  steady-state parse (document ready to query); free timed separately as "free"
//   total  steady-state parse + full traversal + free (every value visited: numbers summed,
//          string and key bytes counted), i.e. the whole cost of using a document once
// CPU time (all threads) per document shows how many cores a parser used. Peak memory is the
// growth of max RSS over the cold parse (and after the traversal, for lazy parsers).
// Hardware counters (Linux perf_event, macOS proc_pid_rusage) are per document.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <pthread.h>
#endif
#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

#include "pj.h"
#include "yyjson.h"

uint64_t tnow(void); // pj.c: monotonic ns
typedef struct { double num; uint64_t vals, bytes; } walk_t;

int sjd_parse(const char *buf, size_t len);
void sjd_walk(walk_t *w);
int sjo_parse(const char *buf, size_t len);
int sjo_walk(walk_t *w);
const char *sj_version(void);
const char *sj_kernel(void);

static char *g_buf;
static size_t g_len;
static yyjson_doc *g_yy;
static pj_doc *g_pj;

// ---- traversals ------------------------------------------------------------

static void yy_walk(yyjson_val *v, walk_t *w) {
    w->vals++;
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NUM: w->num += yyjson_get_num(v); break;
    case YYJSON_TYPE_STR: w->bytes += yyjson_get_len(v); break;
    case YYJSON_TYPE_ARR: {
        yyjson_arr_iter it; yyjson_arr_iter_init(v, &it);
        for (yyjson_val *e; (e = yyjson_arr_iter_next(&it));) yy_walk(e, w);
        break;
    }
    case YYJSON_TYPE_OBJ: {
        yyjson_obj_iter it; yyjson_obj_iter_init(v, &it);
        for (yyjson_val *k; (k = yyjson_obj_iter_next(&it));) { w->bytes += yyjson_get_len(k); yy_walk(yyjson_obj_iter_get_val(k), w); }
        break;
    }
    default: break;
    }
}

static void pj_walk(const pj_doc *d, pj_val v, walk_t *w) {
    w->vals++;
    switch (pj_typeof(d, v)) {
    case PJ_NUM: w->num += pj_get_num(d, v); break;
    case PJ_STR: w->bytes += pj_get_str(d, v).len; break;
    case PJ_ARR: {
        pj_iter it; pj_arr_iter(d, v, &it);
        for (pj_val e; pj_iter_next(&it, &e);) pj_walk(d, e, w);
        break;
    }
    case PJ_OBJ: {
        pj_obj_iter it; pj_obj_iter_init(d, v, &it);
        pj_str k; pj_val e;
        while (pj_obj_iter_next(&it, &k, &e)) { w->bytes += k.len; pj_walk(d, e, w); }
        break;
    }
    default: break;
    }
}

// ---- parsers -----------------------------------------------------------------

typedef struct {
    const char *name;
    int threads;           // 0 = all cores
    int (*parse)(void);    // returns 0 on failure
    void (*walk)(walk_t *);
    void (*release)(void);
} parser;

static int p_yy(void) { return (g_yy = yyjson_read_opts(g_buf, g_len, 0, 0, 0)) != 0; }
static void w_yy(walk_t *w) { yy_walk(yyjson_doc_get_root(g_yy), w); }
static void f_yy(void) { yyjson_doc_free(g_yy); g_yy = 0; }

static int p_pj(void) { return (g_pj = pj_parse(g_buf, g_len, 0, 0)) != 0; }
static int p_pj1(void) { return (g_pj = pj_parse(g_buf, g_len, 1, 0)) != 0; }
static void w_pj(walk_t *w) { pj_walk(g_pj, pj_root(g_pj), w); }
static void f_pj(void) { pj_free(g_pj); g_pj = 0; }

// simdjson parsers own their buffers and reuse them across documents (its documented usage):
// "free" is nothing.
static int p_sjd(void) { return sjd_parse(g_buf, g_len); }
static int p_sjo(void) { return sjo_parse(g_buf, g_len); }
static void w_sjo(walk_t *w) { if (!sjo_walk(w)) { fprintf(stderr, "simdjson-od: traversal error\n"); exit(2); } }
static void f_none(void) {}

static const parser parsers[] = {
    {"yyjson", 1, p_yy, w_yy, f_yy},
    {"simdjson", 1, p_sjd, sjd_walk, f_none},
    {"simdjson-od", 1, p_sjo, w_sjo, f_none},
    {"pj", 0, p_pj, w_pj, f_pj},
    {"pj-1t", 1, p_pj1, w_pj, f_pj},
};

// ---- process resources -------------------------------------------------------

// Peak memory over an interval: reset the high-water mark to the current footprint, run, read it.
// Linux: clear_refs 5 resets VmHWM to VmRSS. macOS: the footprint interval max (phys_footprint
// is what Activity Monitor shows: anonymous + compressed, not shared library pages).
#ifdef __linux__
static uint64_t status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256]; uint64_t v = 0; size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, key, kl)) { v = strtoull(line + kl, 0, 10); break; }
    if (f) fclose(f);
    return v * 1024;
}
static uint64_t mem_reset(void) {
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (f) { fputs("5", f); fclose(f); }
    return status_kb("VmRSS:");
}
static uint64_t mem_peak(void) { return status_kb("VmHWM:"); }
#elif defined(__APPLE__)
int proc_reset_footprint_interval(int pid); // libsystem_kernel (proc_info), not in public headers
static uint64_t mem_reset(void) {
    proc_reset_footprint_interval(getpid());
    struct rusage_info_v4 ri; proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri);
    return ri.ri_phys_footprint;
}
static uint64_t mem_peak(void) {
    struct rusage_info_v4 ri; proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri);
    return ri.ri_interval_max_phys_footprint;
}
#else
static uint64_t mem_reset(void) { return 0; }
static uint64_t mem_peak(void) { return 0; }
#endif
static uint64_t minflt(void) { struct rusage ru; getrusage(RUSAGE_SELF, &ru); return (uint64_t)ru.ru_minflt; }
static uint64_t cpu_ns(void) { // all threads, user + system
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
}

// ---- hardware counters -------------------------------------------------------
// Linux: this thread only (so pj with all cores reports none). macOS: the whole process.

enum { HW_CYCLES, HW_INSTR, HW_BRMISS, HW_N };
static const char *hw_names[HW_N] = {"cycles", "instructions", "branch_misses"};
static int hw_ok;
#ifdef __linux__
static int hw_fd[HW_N] = {-1, -1, -1};
static void hw_init(int single_thread) {
    if (!single_thread) return;
    static const uint64_t cfg[HW_N] = {PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES};
    hw_ok = 1;
    for (int i = 0; i < HW_N; i++) {
        struct perf_event_attr a; memset(&a, 0, sizeof a);
        a.type = PERF_TYPE_HARDWARE; a.size = sizeof a; a.config = cfg[i];
        a.exclude_kernel = 1; a.exclude_hv = 1;
        hw_fd[i] = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
        if (hw_fd[i] < 0) hw_ok = 0;
    }
}
static void hw_read(uint64_t *v) {
    for (int i = 0; i < HW_N; i++) { v[i] = 0; if (hw_fd[i] >= 0 && read(hw_fd[i], &v[i], 8) != 8) v[i] = 0; }
}
#elif defined(__APPLE__)
static void hw_init(int single_thread) { (void)single_thread; hw_ok = 1; }
static void hw_read(uint64_t *v) {
    struct rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri)) { hw_ok = 0; return; }
    v[HW_CYCLES] = ri.ri_cycles; v[HW_INSTR] = ri.ri_instructions; v[HW_BRMISS] = 0;
}
#else
static void hw_init(int single_thread) { (void)single_thread; }
static void hw_read(uint64_t *v) { memset(v, 0, HW_N * 8); }
#endif

// ---- measurement ---------------------------------------------------------------

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

typedef struct { double med, min, p25, p75, mean, sd; int n; } stats;
static stats summarize(double *s, int n) {
    double sum = 0, sq = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    double mean = sum / n;
    for (int i = 0; i < n; i++) sq += (s[i] - mean) * (s[i] - mean);
    qsort(s, n, sizeof *s, cmpd);
    return (stats){s[n / 2], s[0], s[n / 4], s[(3 * n) / 4], mean, n > 1 ? sqrt(sq / (n - 1)) : 0, n};
}

#define MAXS 1000
typedef struct { stats parse, free, total; double cpu; uint64_t hw[HW_N]; uint64_t iters; } loop_t;

// Each sample is the mean of k back-to-back documents, with k chosen so that a sample lasts
// >= 200 us (timer overhead and resolution stay < 0.1%). Phases are timed separately.
// Runs until the time budget is spent, with at least 5 and at most MAXS samples.
static loop_t run_loop(const parser *p, int with_walk, double budget_ns, uint64_t one_ns, walk_t *wout) {
    static double sp[MAXS], sf[MAXS], st[MAXS];
    loop_t L; memset(&L, 0, sizeof L);
    int k = one_ns >= 200000 ? 1 : (int)(200000 / (one_ns ? one_ns : 1)) + 1;
    int n = 0;
    uint64_t hw0[HW_N], hw1[HW_N];
    uint64_t start = tnow(), c0 = cpu_ns();
    hw_read(hw0);
    while (n < MAXS && (n < 5 || (double)(tnow() - start) < budget_ns)) {
        uint64_t ap = 0, af = 0, at = 0;
        for (int i = 0; i < k; i++) {
            walk_t w = {0};
            uint64_t a = tnow();
            if (!p->parse()) { fprintf(stderr, "%s: parse failed\n", p->name); exit(2); }
            uint64_t b = tnow();
            if (with_walk) p->walk(&w);
            uint64_t c = tnow();
            p->release();
            uint64_t e = tnow();
            ap += b - a; af += e - c; at += e - a;
            if (wout) *wout = w;
        }
        L.iters += k;
        sp[n] = (double)ap / k; sf[n] = (double)af / k; st[n] = (double)at / k;
        n++;
        if (n >= 5 && one_ns > 2e9) break; // huge documents: 5 samples
    }
    hw_read(hw1);
    L.cpu = (double)(cpu_ns() - c0) / L.iters;
    for (int i = 0; i < HW_N; i++) L.hw[i] = (hw1[i] - hw0[i]) / L.iters;
    L.parse = summarize(sp, n); L.free = summarize(sf, n); L.total = summarize(st, n);
    return L;
}

static char *load(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(1); }
    struct stat st; fstat(fd, &st);
    *len = (size_t)st.st_size;
    char *b = calloc(1, *len + 64); // 64 bytes of padding: what pj and simdjson require
    for (size_t off = 0; off < *len;) { ssize_t r = read(fd, b + off, *len - off); if (r <= 0) exit(1); off += (size_t)r; }
    close(fd);
    return b;
}

static void pstats(const char *name, stats s) {
    printf("\"%s\":{\"med\":%.1f,\"min\":%.1f,\"p25\":%.1f,\"p75\":%.1f,\"mean\":%.1f,\"sd\":%.1f,\"n\":%d}", name, s.med, s.min, s.p25, s.p75, s.mean, s.sd, s.n);
}
static void phw(const char *name, const loop_t *L) {
    printf("\"%s\":", name);
    if (!hw_ok) { printf("null"); return; }
    printf("{");
    for (int i = 0; i < HW_N; i++) {
#ifdef __APPLE__
        if (i == HW_BRMISS) { printf(",\"%s\":null", hw_names[i]); continue; } // not available without root
#endif
        printf("%s\"%s\":%llu", i ? "," : "", hw_names[i], (unsigned long long)L->hw[i]);
    }
    printf("}");
}

int main(int argc, char **argv) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    if (argc == 2 && !strcmp(argv[1], "--versions")) {
        printf("{\"yyjson\":\"%s\",\"simdjson\":\"%s\",\"simdjson_kernel\":\"%s\",\"pj_arch\":\"%s\"}\n",
               YYJSON_VERSION_STRING, sj_version(), sj_kernel(), pj_arch());
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: runner <parser> <file> [budget_s] | runner --versions\n"); return 1; }
    const parser *p = 0;
    for (size_t i = 0; i < sizeof parsers / sizeof *parsers; i++) if (!strcmp(parsers[i].name, argv[1])) p = &parsers[i];
    if (!p) { fprintf(stderr, "unknown parser %s\n", argv[1]); return 1; }
    double budget = (argc > 3 ? atof(argv[3]) : 1.0) * 1e9;
    g_buf = load(argv[2], &g_len);
    hw_init(p->threads == 1);

    // Cold: the first parse in this process (input already resident).
    uint64_t rss0 = mem_reset(), flt0 = minflt();
    uint64_t t0 = tnow();
    if (!p->parse()) { fprintf(stderr, "%s: parse failed\n", p->name); return 2; }
    uint64_t cold = tnow() - t0;
    uint64_t pk = mem_peak(), mem_parse = pk > rss0 ? pk - rss0 : 0, faults = minflt() - flt0;
    walk_t w = {0};
    p->walk(&w);
    pk = mem_peak(); // lazy parsers (simdjson On-Demand) materialize during the walk
    uint64_t mem_total = pk > rss0 ? pk - rss0 : 0;
    p->release();

    walk_t wt;
    loop_t lp = run_loop(p, 0, budget, cold, 0);
    loop_t lt = run_loop(p, 1, budget, cold, &wt);

    printf("{\"parser\":\"%s\",\"threads\":%d,\"bytes\":%zu,\"cold\":%llu,\"cold_faults\":%llu,", p->name, p->threads, g_len,
           (unsigned long long)cold, (unsigned long long)faults);
    pstats("parse", lp.parse); putchar(',');
    pstats("free", lp.free); putchar(',');
    pstats("total", lt.total); putchar(',');
    printf("\"cpu_parse\":%.1f,\"cpu_total\":%.1f,", lp.cpu, lt.cpu);
    phw("hw_parse", &lp); putchar(',');
    phw("hw_total", &lt); putchar(',');
    printf("\"mem_parse\":%llu,\"mem_total\":%llu,\"rss_base\":%llu,"
           "\"check\":{\"vals\":%llu,\"bytes\":%llu,\"num\":%.17g}}\n",
           (unsigned long long)mem_parse, (unsigned long long)mem_total, (unsigned long long)rss0,
           (unsigned long long)wt.vals, (unsigned long long)wt.bytes, wt.num);
    return 0;
}
