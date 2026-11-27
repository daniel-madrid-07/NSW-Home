// Bencoding (BEP 3): what the DHT speaks and what the friends module keeps
// its own file in. Header-only; nothing here depends on the system.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ben {

struct Val {
    enum Type { None, Int, Str, List, Dict } type = None;
    int64_t i = 0;
    std::string s;
    std::vector<Val> list;
    std::vector<std::pair<std::string, Val>> dict;
    // Where this value stood in the text it was decoded from: a signature
    // over a value is over those exact bytes, not over a re-encoding.
    size_t rawBegin = 0, rawEnd = 0;

    Val() = default;
    Val(int64_t n) : type(Int), i(n) {}
    Val(int n) : type(Int), i(n) {}
    Val(const std::string& str) : type(Str), s(str) {}
    Val(const char* str) : type(Str), s(str) {}

    static Val MakeDict() { Val v; v.type = Dict; return v; }
    static Val MakeList() { Val v; v.type = List; return v; }

    Val& Set(const std::string& key, Val v) {
        for (auto& kv : dict)
            if (kv.first == key) {
                kv.second = std::move(v);
                return *this;
            }
        dict.emplace_back(key, std::move(v));
        return *this;
    }
    const Val* Get(const std::string& key) const {
        if (type != Dict) return nullptr;
        for (const auto& kv : dict)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // Typed reads with a fallback, for values that came from outside.
    std::string Str_(const std::string& key) const {
        const Val* v = Get(key);
        return v && v->type == Str ? v->s : std::string();
    }
    int64_t Int_(const std::string& key, int64_t def = 0) const {
        const Val* v = Get(key);
        return v && v->type == Int ? v->i : def;
    }
    bool Has(const std::string& key, Type t) const {
        const Val* v = Get(key);
        return v && v->type == t;
    }
};

inline void EncodeTo(const Val& v, std::string& out) {
    switch (v.type) {
    case Val::Int: out += "i" + std::to_string(v.i) + "e"; break;
    case Val::Str: out += std::to_string(v.s.size()) + ":" + v.s; break;
    case Val::List:
        out += "l";
        for (const auto& e : v.list) EncodeTo(e, out);
        out += "e";
        break;
    case Val::Dict: {
        // Keys go out sorted as raw bytes, as the format requires.
        std::vector<const std::pair<std::string, Val>*> order;
        for (const auto& kv : v.dict) order.push_back(&kv);
        std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->first < b->first; });
        out += "d";
        for (auto* kv : order) {
            out += std::to_string(kv->first.size()) + ":" + kv->first;
            EncodeTo(kv->second, out);
        }
        out += "e";
        break;
    }
    case Val::None: break;
    }
}

inline std::string Encode(const Val& v) {
    std::string out;
    EncodeTo(v, out);
    return out;
}

namespace detail {
inline bool Parse(const std::string& in, size_t& p, Val& out, int depth) {
    if (p >= in.size() || depth > 32) return false;
    out.rawBegin = p;
    char c = in[p];
    if (c == 'i') {
        size_t e = in.find('e', p);
        if (e == std::string::npos || e == p + 1 || e - p > 21) return false;
        bool neg = in[p + 1] == '-';
        size_t d = p + 1 + (neg ? 1 : 0);
        if (d == e) return false;
        uint64_t n = 0;
        for (; d < e; d++) {
            if (in[d] < '0' || in[d] > '9') return false;
            if (n > (UINT64_MAX - 9) / 10) return false;
            n = n * 10 + static_cast<uint64_t>(in[d] - '0');
        }
        if (n > static_cast<uint64_t>(INT64_MAX)) return false;
        out.type = Val::Int;
        out.i = neg ? -static_cast<int64_t>(n) : static_cast<int64_t>(n);
        p = e + 1;
    } else if (c >= '0' && c <= '9') {
        size_t n = 0;
        while (p < in.size() && in[p] >= '0' && in[p] <= '9') {
            n = n * 10 + static_cast<size_t>(in[p] - '0');
            if (n > in.size()) return false;
            p++;
        }
        if (p >= in.size() || in[p] != ':' || in.size() - p - 1 < n) return false;
        out.type = Val::Str;
        out.s = in.substr(p + 1, n);
        p += 1 + n;
    } else if (c == 'l') {
        out.type = Val::List;
        p++;
        while (p < in.size() && in[p] != 'e') {
            Val e;
            if (!Parse(in, p, e, depth + 1)) return false;
            out.list.push_back(std::move(e));
        }
        if (p >= in.size()) return false;
        p++;
    } else if (c == 'd') {
        out.type = Val::Dict;
        p++;
        while (p < in.size() && in[p] != 'e') {
            Val k, e;
            if (!Parse(in, p, k, depth + 1) || k.type != Val::Str) return false;
            if (!Parse(in, p, e, depth + 1)) return false;
            out.dict.emplace_back(std::move(k.s), std::move(e));
        }
        if (p >= in.size()) return false;
        p++;
    } else {
        return false;
    }
    out.rawEnd = p;
    return true;
}
} // namespace detail

// The whole text must be one value.
inline bool Decode(const std::string& in, Val& out) {
    size_t p = 0;
    out = Val();
    return detail::Parse(in, p, out, 0) && p == in.size();
}

} // namespace ben
