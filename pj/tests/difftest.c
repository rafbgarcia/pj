// Differential test: parse with yyjson and pj, compare full trees.
// Usage: difftest <file.json>...   (set PJ_CHUNK=n to force tiny chunks)
// Exit 0 if every file agrees (same validity; identical trees when valid).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pj.h"
#include "yyjson.h"

static int fails;

static int cmp(const pj_doc *d, pj_val v, yyjson_val *y, const char *path) {
    pj_type t = pj_typeof(d, v);
    switch (yyjson_get_type(y)) {
    case YYJSON_TYPE_NULL: if (t != PJ_NULL) goto bad; return 0;
    case YYJSON_TYPE_BOOL: if (t != (yyjson_get_bool(y) ? PJ_TRUE : PJ_FALSE)) goto bad; return 0;
    case YYJSON_TYPE_NUM: {
        if (t != PJ_NUM) goto bad;
        uint64_t w = d->tape[v];
        if (yyjson_is_real(y)) {
            double a = yyjson_get_real(y), b = pj_get_num(d, v);
            if (memcmp(&a, &b, 8) != 0) { fprintf(stderr, "%s: real %.17g vs %.17g\n", path, a, b); return 1; }
        } else if (yyjson_is_sint(y)) {
            int64_t a = yyjson_get_sint(y), b;
            if (PJ_TAG(w) == PJ_TAG_INT) b = (int64_t)(w << 16) >> 16;
            else if (PJ_TAG(w) == PJ_TAG_BIG) { const pj_big *g = (const pj_big *)PJ_PAYLOAD(w); b = g->neg ? -(int64_t)g->u : (int64_t)g->u; }
            else goto bad;
            if (a != b) { fprintf(stderr, "%s: sint %lld vs %lld\n", path, a, b); return 1; }
        } else {
            uint64_t a = yyjson_get_uint(y), b;
            if (PJ_TAG(w) == PJ_TAG_INT) b = (uint64_t)((int64_t)(w << 16) >> 16);
            else if (PJ_TAG(w) == PJ_TAG_BIG) b = ((const pj_big *)PJ_PAYLOAD(w))->u;
            else goto bad;
            if (a != b) { fprintf(stderr, "%s: uint %llu vs %llu\n", path, a, b); return 1; }
        }
        return 0;
    }
    case YYJSON_TYPE_STR: {
        if (t != PJ_STR) goto bad;
        pj_str s = pj_get_str(d, v);
        if (s.len != yyjson_get_len(y) || memcmp(s.s, yyjson_get_str(y), s.len)) { fprintf(stderr, "%s: string mismatch\n yy[%zu]=%.80s\n pj[%zu]=%.*s\n", path, yyjson_get_len(y), yyjson_get_str(y), s.len, (int)(s.len > 80 ? 80 : s.len), s.s); return 1; }
        return 0;
    }
    case YYJSON_TYPE_ARR: {
        if (t != PJ_ARR) goto bad;
        if (pj_arr_size(d, v) != yyjson_arr_size(y)) { fprintf(stderr, "%s: arr size %zu vs %zu\n", path, yyjson_arr_size(y), pj_arr_size(d, v)); return 1; }
        pj_iter it; pj_arr_iter(d, v, &it);
        yyjson_arr_iter yi; yyjson_arr_iter_init(y, &yi);
        size_t i = 0; pj_val pv;
        for (yyjson_val *yv; (yv = yyjson_arr_iter_next(&yi)); i++) {
            if (!pj_iter_next(&it, &pv)) { fprintf(stderr, "%s: arr ended early at %zu\n", path, i); return 1; }
            char p2[256]; snprintf(p2, sizeof p2, "%.200s[%zu]", path, i);
            if (cmp(d, pv, yv, p2)) return 1;
        }
        if (pj_iter_next(&it, &pv)) {
            { pj_iter i2; pj_val q; pj_arr_iter(d, v, &i2); int z = 0;
              while (pj_iter_next(&i2, &q) && z < 60) { fprintf(stderr, "  el %d at %llu tag %x skip %llu stride %llu\n", z++, q, PJ_TAG(d->tape[q]), pj__skip(d->tape, q), i2.stride); } }
            fprintf(stderr, "%s: arr has extra items: extra at %llu tag %x word %016llx, end %llu stride %llu, arr at %llu\n", path, pv, PJ_TAG(d->tape[pv]), d->tape[pv], it.end, it.stride, v);
            return 1;
        }
        return 0;
    }
    case YYJSON_TYPE_OBJ: {
        if (t != PJ_OBJ) goto bad;
        if (pj_obj_size(d, v) != yyjson_obj_size(y)) { fprintf(stderr, "%s: obj size %zu vs %zu\n", path, yyjson_obj_size(y), pj_obj_size(d, v)); return 1; }
        pj_obj_iter it; pj_obj_iter_init(d, v, &it);
        yyjson_obj_iter yi; yyjson_obj_iter_init(y, &yi);
        pj_str k; pj_val pv;
        for (yyjson_val *yk; (yk = yyjson_obj_iter_next(&yi));) {
            if (!pj_obj_iter_next(&it, &k, &pv)) { fprintf(stderr, "%s: obj ended early\n", path); return 1; }
            if (k.len != yyjson_get_len(yk) || memcmp(k.s, yyjson_get_str(yk), k.len)) { fprintf(stderr, "%s: key mismatch '%.*s' vs '%s'\n", path, (int)k.len, k.s, yyjson_get_str(yk)); return 1; }
            char p2[256]; snprintf(p2, sizeof p2, "%.200s.%.40s", path, yyjson_get_str(yk));
            if (cmp(d, pv, yyjson_obj_iter_get_val(yk), p2)) return 1;
            // keyed lookup must find the first occurrence; only check unique keys
            pj_val lv;
            if (strlen(yyjson_get_str(yk)) == yyjson_get_len(yk) && !pj_obj_get(d, v, yyjson_get_str(yk), &lv)) {
                fprintf(stderr, "%s: lookup failed for key len %zu:", p2, yyjson_get_len(yk));
                for (size_t q = 0; q < yyjson_get_len(yk); q++) fprintf(stderr, " %02x", (unsigned char)yyjson_get_str(yk)[q]);
                fprintf(stderr, "\n");
                return 1;
            }
        }
        return 0;
    }
    default: break;
    }
bad:
    fprintf(stderr, "%s: type mismatch (yy %s, pj %d)\n", path, yyjson_get_type_desc(y), t);
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--arch")) { printf("%s\n", pj_arch()); return 0; }
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); return 2; }
        fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        char *b = calloc(1, n + 64);
        if (fread(b, 1, n, f) != n) return 2;
        fclose(f);
        yyjson_read_err ye;
        yyjson_doc *yd = yyjson_read_opts(b, n, 0, 0, &ye);
        pj_err pe;
        pj_doc *pd = pj_parse(b, n, 0, &pe);
        int bad = 0;
        if (!yd != !pd) {
            fprintf(stderr, "%s: validity differs: yyjson %s (%s @%zu), pj %s (%s @%zu)\n", argv[a], yd ? "ok" : "err", yd ? "" : ye.msg, ye.pos,
                    pd ? "ok" : "err", pd ? "" : pe.msg, pe.pos);
            bad = 1;
        } else if (yd) {
            bad = cmp(pd, pj_root(pd), yyjson_doc_get_root(yd), argv[a]);
        }
        if (bad) { fails++; fprintf(stderr, "FAIL %s\n", argv[a]); }
        yyjson_doc_free(yd);
        pj_free(pd);
        free(b);
    }
    printf("%d files, %d failures\n", argc - 1, fails);
    return fails != 0;
}
