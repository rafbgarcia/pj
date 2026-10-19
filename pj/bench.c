// Benchmark: parse + extract {x0,y0,x1,y1} from every item, yyjson vs simdjson vs pj.
// Usage: bench <file.json> [reps] [filter]
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "pj.h"
#include "yyjson.h"

int sjd_parse(const char *buf, size_t len);
double sjd_sum(size_t *n);
int sjo_iterate(const char *buf, size_t len);
double sjo_sum(size_t *n);
int sjd_parse_file(const char *path);

uint64_t tnow(void);
static uint64_t now(void) { return tnow(); }

static char *g_buf, *g_copy;
static size_t g_len;
static const char *g_path;

typedef struct { const char *name; void (*run)(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n); } parser;

static double yy_sum(yyjson_doc *doc, size_t *n) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_arr_iter it; yyjson_arr_iter_init(root, &it);
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0; size_t c = 0;
    for (yyjson_val *o; (o = yyjson_arr_iter_next(&it));) {
        s0 += yyjson_get_num(yyjson_obj_get(o, "x0"));
        s1 += yyjson_get_num(yyjson_obj_get(o, "y0"));
        s2 += yyjson_get_num(yyjson_obj_get(o, "x1"));
        s3 += yyjson_get_num(yyjson_obj_get(o, "y1"));
        c++;
    }
    *n = c;
    return (s0 + s1) + (s2 + s3);
}

static void run_yy(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    uint64_t a = now();
    yyjson_doc *doc = yyjson_read_opts(g_buf, g_len, 0, 0, 0);
    uint64_t b = now();
    if (!doc) { fprintf(stderr, "yyjson failed\n"); exit(1); }
    *sum = yy_sum(doc, n);
    uint64_t c = now();
    yyjson_doc_free(doc);
    *tp = b - a; *te = c - b; *tf = now() - c;
}

static void run_yy_insitu(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    memcpy(g_copy, g_buf, g_len + 64); // not timed
    uint64_t a = now();
    yyjson_doc *doc = yyjson_read_opts(g_copy, g_len, YYJSON_READ_INSITU, 0, 0);
    uint64_t b = now();
    if (!doc) { fprintf(stderr, "yyjson insitu failed\n"); exit(1); }
    *sum = yy_sum(doc, n);
    uint64_t c = now();
    yyjson_doc_free(doc);
    *tf = now() - c;
    *tp = b - a; *te = c - b;
}

static void run_sjd(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    uint64_t a = now();
    if (!sjd_parse(g_buf, g_len)) { fprintf(stderr, "simdjson failed\n"); exit(1); }
    uint64_t b = now();
    *sum = sjd_sum(n);
    uint64_t c = now();
    *tp = b - a; *te = c - b; *tf = 0;
}

static void run_sjo(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    uint64_t a = now();
    if (!sjo_iterate(g_buf, g_len)) { fprintf(stderr, "simdjson ondemand failed\n"); exit(1); }
    uint64_t b = now();
    *sum = sjo_sum(n);
    uint64_t c = now();
    *tp = b - a; *te = c - b; *tf = 0;
}

static double pj_sum_doc(pj_doc *d, size_t *n) {
    pj_iter it; pj_arr_iter(d, pj_root(d), &it);
    pj_qkey kx0 = pj_qkey_make("x0"), ky0 = pj_qkey_make("y0"), kx1 = pj_qkey_make("x1"), ky1 = pj_qkey_make("y1");
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0; size_t c = 0;
    for (pj_val o, v; pj_iter_next(&it, &o);) {
        pj_obj_get_k(d, o, &kx0, &v); s0 += pj_get_num(d, v);
        pj_obj_get_k(d, o, &ky0, &v); s1 += pj_get_num(d, v);
        pj_obj_get_k(d, o, &kx1, &v); s2 += pj_get_num(d, v);
        pj_obj_get_k(d, o, &ky1, &v); s3 += pj_get_num(d, v);
        c++;
    }
    *n = c;
    return (s0 + s1) + (s2 + s3);
}

static double pj_sum_doc_str(pj_doc *d, size_t *n) {
    pj_iter it; pj_arr_iter(d, pj_root(d), &it);
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0; size_t c = 0;
    for (pj_val o, v; pj_iter_next(&it, &o);) {
        pj_obj_get(d, o, "x0", &v); s0 += pj_get_num(d, v);
        pj_obj_get(d, o, "y0", &v); s1 += pj_get_num(d, v);
        pj_obj_get(d, o, "x1", &v); s2 += pj_get_num(d, v);
        pj_obj_get(d, o, "y1", &v); s3 += pj_get_num(d, v);
        c++;
    }
    *n = c;
    return (s0 + s1) + (s2 + s3);
}

static void run_pj_n(int nt, bool strkeys, uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    pj_err err;
    uint64_t a = now();
    pj_doc *d = pj_parse(g_buf, g_len, nt, &err);
    uint64_t b = now();
    if (!d) { fprintf(stderr, "pj failed: %s at %zu\n", err.msg, err.pos); exit(1); }
    *sum = strkeys ? pj_sum_doc_str(d, n) : pj_sum_doc(d, n);
    uint64_t c = now();
    pj_free(d);
    *tf = now() - c;
    *tp = b - a; *te = c - b;
}
static void run_pj(uint64_t *tp, uint64_t *te, uint64_t *tf, double *s, size_t *n) { run_pj_n(0, false, tp, te, tf, s, n); }
static void run_pj_str(uint64_t *tp, uint64_t *te, uint64_t *tf, double *s, size_t *n) { run_pj_n(0, true, tp, te, tf, s, n); }
static void run_pj1(uint64_t *tp, uint64_t *te, uint64_t *tf, double *s, size_t *n) { run_pj_n(1, false, tp, te, tf, s, n); }

// end-to-end from file path
static void run_yy_file(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    uint64_t a = now();
    yyjson_doc *doc = yyjson_read_file(g_path, 0, 0, 0);
    uint64_t b = now();
    *sum = yy_sum(doc, n);
    uint64_t c = now();
    yyjson_doc_free(doc);
    *tf = now() - c; *tp = b - a; *te = c - b;
}
static void run_sjd_file(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    uint64_t a = now();
    if (!sjd_parse_file(g_path)) exit(1);
    uint64_t b = now();
    *sum = sjd_sum(n);
    uint64_t c = now();
    *tf = 0; *tp = b - a; *te = c - b;
}
static void run_pj_file(uint64_t *tp, uint64_t *te, uint64_t *tf, double *sum, size_t *n) {
    pj_err err;
    uint64_t a = now();
    pj_doc *d = pj_parse_file(g_path, 0, &err);
    uint64_t b = now();
    if (!d) { fprintf(stderr, "pj failed: %s at %zu\n", err.msg, err.pos); exit(1); }
    *sum = pj_sum_doc(d, n);
    uint64_t c = now();
    pj_free(d);
    *tf = now() - c; *tp = b - a; *te = c - b;
}

static int cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

int sjd_validate(const char *buf, size_t len);

static char *load(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    struct stat st; fstat(fd, &st);
    *len = (size_t)st.st_size;
    char *b = calloc(1, *len + 64);
    for (size_t off = 0; off < *len;) { ssize_t r = read(fd, b + off, *len - off); if (r <= 0) exit(1); off += (size_t)r; }
    close(fd);
    return b;
}

// Parse-only (+free) timing on arbitrary files.
static int parse_only(int argc, char **argv) {
    int reps = atoi(argv[2]);
    printf("%-28s %8s | %9s %9s %9s %9s | %7s %7s\n", "file", "MB", "yyjson", "simdjson", "pj-1t", "pj", "vs yy", "vs sj");
    for (int a = 3; a < argc; a++) {
        size_t len; char *b = load(argv[a], &len);
        uint64_t best[4] = {~0ull, ~0ull, ~0ull, ~0ull};
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = now();
            yyjson_doc *y = yyjson_read_opts(b, len, 0, 0, 0);
            yyjson_doc_free(y);
            uint64_t t1 = now();
            sjd_validate(b, len);
            uint64_t t2 = now();
            pj_doc *d1 = pj_parse(b, len, 1, 0);
            pj_free(d1);
            uint64_t t3 = now();
            pj_doc *d = pj_parse(b, len, 0, 0);
            pj_free(d);
            uint64_t t4 = now();
            if (!y || !d || !d1) { fprintf(stderr, "%s: parse failed\n", argv[a]); }
            uint64_t ts[4] = {t1 - t0, t2 - t1, t3 - t2, t4 - t3};
            for (int i = 0; i < 4; i++) if (ts[i] < best[i]) best[i] = ts[i];
        }
        const char *nm = strrchr(argv[a], '/'); nm = nm ? nm + 1 : argv[a];
        printf("%-28s %8.1f | %9.3f %9.3f %9.3f %9.3f | %6.1fx %6.1fx\n", nm, len / 1e6, best[0] / 1e6, best[1] / 1e6, best[2] / 1e6, best[3] / 1e6,
               (double)best[0] / best[3], (double)best[1] / best[3]);
        free(b);
    }
    return 0;
}

int main(int argc, char **argv) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    if (argc > 3 && !strcmp(argv[1], "-p")) return parse_only(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: bench <file.json> [reps] [filter] | bench -p <reps> <files...>\n"); return 1; }
    g_path = argv[1];
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    const char *filter = argc > 3 ? argv[3] : 0;
    int fd = open(g_path, O_RDONLY);
    struct stat st; fstat(fd, &st);
    g_len = (size_t)st.st_size;
    g_buf = calloc(1, g_len + 64);
    g_copy = calloc(1, g_len + 64);
    for (size_t off = 0; off < g_len;) { ssize_t r = read(fd, g_buf + off, g_len - off); if (r <= 0) return 1; off += (size_t)r; }
    close(fd);

    parser ps[] = {
        {"yyjson", run_yy},
        {"yyjson-insitu", run_yy_insitu},
        {"simdjson-dom", run_sjd},
        {"simdjson-od", run_sjo},
        {"pj-1thread", run_pj1},
        {"pj", run_pj},
        {"pj-strkeys", run_pj_str},
        {"yyjson-file", run_yy_file},
        {"simdjson-file", run_sjd_file},
        {"pj-file", run_pj_file},
    };
    printf("%s: %.1f MB, %d reps (median ms)\n", g_path, g_len / 1e6, reps);
    printf("%-15s %9s %9s %9s %9s %9s %8s  %s\n", "parser", "parse", "parse-min", "extract", "total", "total-min", "GB/s", "checksum");
    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++) {
        if (filter && !strstr(ps[i].name, filter)) continue;
        uint64_t tp[64], te[64], tt[64], tf[64];
        double sum = 0; size_t n = 0;
        for (int r = 0; r < reps && r < 64; r++) {
            ps[i].run(&tp[r], &te[r], &tf[r], &sum, &n);
            tt[r] = tp[r] + te[r];
        }
        qsort(tp, reps, 8, cmp); qsort(te, reps, 8, cmp); qsort(tt, reps, 8, cmp); qsort(tf, reps, 8, cmp);
        int m = reps / 2;
        printf("%-15s %9.2f %9.2f %9.2f %9.2f %9.2f %8.2f  %.6f (n=%zu)\n", ps[i].name, tp[m] / 1e6, tp[0] / 1e6, te[m] / 1e6, tt[m] / 1e6, tt[0] / 1e6,
               g_len / (double)tp[m], sum, n);
    }
    return 0;
}
