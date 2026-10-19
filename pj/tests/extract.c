// Microbench: consumer-loop variants over one parsed coordinates doc.
#include "../pj.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
int main(int c, char **v) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    pj_err e; pj_doc *d = pj_parse_file(v[1], 0, &e);
    for (int r = 0; r < 3; r++) {
        uint64_t t0 = now();
        pj_iter it; pj_arr_iter(d, pj_root(d), &it); size_t n = 0;
        for (pj_val o; pj_iter_next(&it, &o);) n += o;
        uint64_t t1 = now();
        pj_arr_iter(d, pj_root(d), &it);
        pj_qkey k0 = pj_qkey_make("x0"), k1 = pj_qkey_make("y0"), k2 = pj_qkey_make("x1"), k3 = pj_qkey_make("y1");
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (pj_val o, x; pj_iter_next(&it, &o);) {
            pj_obj_get_k(d, o, &k0, &x); s0 += pj_get_num(d, x);
            pj_obj_get_k(d, o, &k1, &x); s1 += pj_get_num(d, x);
            pj_obj_get_k(d, o, &k2, &x); s2 += pj_get_num(d, x);
            pj_obj_get_k(d, o, &k3, &x); s3 += pj_get_num(d, x);
        }
        uint64_t t2 = now();
        // raw: sum every unboxed word of the root array region(s)
        const uint64_t *T = d->tape; uint64_t end = PJ_PAYLOAD(T[pj_root(d)]); double s4 = 0, s5 = 0;
        for (uint64_t i = 0; i < end; i += 2) { double a, b; uint64_t wa = T[i], wb = T[i + 1]; memcpy(&a, &wa, 8); memcpy(&b, &wb, 8); if (!PJ_BOXED(wa)) s4 += a; if (!PJ_BOXED(wb)) s5 += b; }
        uint64_t t3 = now();
        printf("iterate-only %.2f ms | iterate+4 gets %.2f ms | raw tape scan (%.0f MB) %.2f ms   [%zu %.3f %.3f]\n", (t1 - t0) / 1e6, (t2 - t1) / 1e6, end * 8 / 1e6, (t3 - t2) / 1e6, n, s0 + s1 + s2 + s3, s4 + s5);
    }
}
