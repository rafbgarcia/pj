// simdjson wrappers for bench.c
#include "simdjson.h"
using namespace simdjson;

static dom::parser dp;
static dom::element droot;
static ondemand::parser op;
static ondemand::document odoc;
static padded_string pstr;

extern "C" int sjd_parse(const char *buf, size_t len) { return dp.parse((const uint8_t *)buf, len, false).get(droot) ? 0 : 1; }

extern "C" double sjd_sum(size_t *n) {
    dom::array a = droot.get_array().value_unsafe();
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0; size_t c = 0;
    for (dom::element e : a) {
        dom::object o = e.get_object().value_unsafe();
        s0 += o["x0"].get_double().value_unsafe();
        s1 += o["y0"].get_double().value_unsafe();
        s2 += o["x1"].get_double().value_unsafe();
        s3 += o["y1"].get_double().value_unsafe();
        c++;
    }
    *n = c;
    return (s0 + s1) + (s2 + s3);
}

extern "C" int sjo_iterate(const char *buf, size_t len) {
    return op.iterate(padded_string_view(buf, len, len + SIMDJSON_PADDING)).get(odoc) ? 0 : 1;
}

extern "C" double sjo_sum(size_t *n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0; size_t c = 0;
    ondemand::array a = odoc.get_array().value_unsafe();
    for (auto v : a) {
        ondemand::object o = v.get_object().value_unsafe();
        s0 += o["x0"].get_double().value_unsafe();
        s1 += o["y0"].get_double().value_unsafe();
        s2 += o["x1"].get_double().value_unsafe();
        s3 += o["y1"].get_double().value_unsafe();
        c++;
    }
    *n = c;
    return (s0 + s1) + (s2 + s3);
}

extern "C" int sjd_parse_file(const char *path) {
    if (padded_string::load(path).get(pstr)) return 0;
    return dp.parse(pstr).get(droot) ? 0 : 1;
}

extern "C" int sjd_validate(const char *buf, size_t len) {
    dom::element e;
    return dp.parse((const uint8_t *)buf, len, false).get(e) ? 0 : 1;
}
