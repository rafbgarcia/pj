// simdjson (DOM and On-Demand) wrappers for runner.c
#include "simdjson.h"
#include <string>
using namespace simdjson;

struct walk_t { double num; uint64_t vals, bytes; };

static dom::parser dp;
static dom::element droot;
static ondemand::parser op;
static ondemand::document odoc;

extern "C" int sjd_parse(const char *buf, size_t len) { return dp.parse((const uint8_t *)buf, len, false).get(droot) ? 0 : 1; }

static void dwalk(dom::element e, walk_t *w) {
    w->vals++;
    switch (e.type()) {
    case dom::element_type::DOUBLE: w->num += e.get_double().value_unsafe(); break;
    case dom::element_type::INT64: w->num += (double)e.get_int64().value_unsafe(); break;
    case dom::element_type::UINT64: w->num += (double)e.get_uint64().value_unsafe(); break;
    case dom::element_type::STRING: w->bytes += e.get_string_length().value_unsafe(); break;
    case dom::element_type::ARRAY:
        for (dom::element c : e.get_array().value_unsafe()) dwalk(c, w);
        break;
    case dom::element_type::OBJECT:
        for (dom::key_value_pair kv : e.get_object().value_unsafe()) { w->bytes += kv.key.size(); dwalk(kv.value, w); }
        break;
    default: break;
    }
}
extern "C" void sjd_walk(walk_t *w) { dwalk(droot, w); }

extern "C" int sjo_parse(const char *buf, size_t len) {
    return op.iterate(padded_string_view(buf, len, len + SIMDJSON_PADDING)).get(odoc) ? 0 : 1;
}

static bool owalk(ondemand::value v, walk_t *w) {
    w->vals++;
    ondemand::json_type t;
    if (v.type().get(t)) return false;
    switch (t) {
    case ondemand::json_type::number: { double d; if (v.get_double().get(d)) return false; w->num += d; break; }
    case ondemand::json_type::string: { std::string_view s; if (v.get_string().get(s)) return false; w->bytes += s.size(); break; }
    case ondemand::json_type::boolean: { bool b; if (v.get_bool().get(b)) return false; break; }
    case ondemand::json_type::null: { if (!v.is_null()) return false; break; }
    case ondemand::json_type::array: {
        ondemand::array a; if (v.get_array().get(a)) return false;
        for (auto c : a) { ondemand::value cv; if (c.get(cv) || !owalk(cv, w)) return false; }
        break;
    }
    case ondemand::json_type::object: {
        ondemand::object o; if (v.get_object().get(o)) return false;
        for (auto f : o) {
            std::string_view k; ondemand::value fv;
            if (f.unescaped_key().get(k)) return false;
            w->bytes += k.size();
            if (f.value().get(fv) || !owalk(fv, w)) return false;
        }
        break;
    }
    default: return false;
    }
    return true;
}
extern "C" int sjo_walk(walk_t *w) {
    ondemand::value v;
    if (odoc.get_value().get(v)) return 0;
    return owalk(v, w) ? 1 : 0;
}

extern "C" const char *sj_version(void) { return SIMDJSON_VERSION; }
extern "C" const char *sj_kernel(void) {
    static std::string k = std::string(simdjson::get_active_implementation()->name());
    return k.c_str();
}
