// The ActionScript 2 classes and global functions the game's movies use. Movie clips, text
// fields, Color and the flash.geom classes come from the player (swf/player.cpp).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "swf/as.h"
#include "swf/player.h"

namespace tdkr::swf {

namespace {

std::u32string decode(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        char32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) cp = (c & 7u) << 18 | (s[i + 1] & 63u) << 12 | (s[i + 2] & 63u) << 6 | (s[i + 3] & 63u), n = 4;
        else if (c >= 0xE0 && i + 2 < s.size()) cp = (c & 15u) << 12 | (s[i + 1] & 63u) << 6 | (s[i + 2] & 63u), n = 3;
        else if (c >= 0xC0 && i + 1 < s.size()) cp = (c & 31u) << 6 | (s[i + 1] & 63u), n = 2;
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string encode(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 63));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 63)), out += char(0x80 | (c & 63));
        else out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 63)), out += char(0x80 | (c >> 6 & 63)), out += char(0x80 | (c & 63));
    }
    return out;
}

char32_t lower(char32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
char32_t upper(char32_t c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

double clamp_index(double i, double len) {
    if (std::isnan(i)) return 0;
    if (i < 0) i = std::max(0.0, len + i);
    return std::min(i, len);
}

}  // namespace

std::string utf8_from_code_points(const std::u32string& s) { return encode(s); }
std::u32string code_points(const std::string& s) { return decode(s); }

void VM::install_builtins() {
    object_proto = std::make_shared<Object>(nullptr);
    function_proto = std::make_shared<Object>(object_proto);
    global = std::make_shared<Object>(object_proto);
    array_proto = std::make_shared<Object>(object_proto);
    string_proto = std::make_shared<Object>(object_proto);
    number_proto = std::make_shared<Object>(object_proto);
    boolean_proto = std::make_shared<Object>(object_proto);
    movieclip_proto = std::make_shared<Object>(object_proto);
    textfield_proto = std::make_shared<Object>(object_proto);
    textformat_proto = std::make_shared<Object>(object_proto);

    auto klass = [&](const std::string& name, const ObjectPtr& proto, NativeFunction::Fn fn, NativeFunction::Ctor ctor = nullptr) {
        auto f = native(std::move(fn), std::move(ctor));
        f->set_own("prototype", Value(proto), 1);
        proto->set_own("constructor", Value(ObjectPtr(f)), 1);
        global->set_own(name, Value(ObjectPtr(f)), 1);
        return f;
    };

    // --- Object
    auto object_ctor = klass("Object", object_proto,
        [](VM& vm, CallArgs& a) { return a[0].is_object() ? a[0] : Value(vm.new_object()); },
        [](VM& vm, CallArgs& a) -> ObjectPtr { return a[0].is_object() ? a[0].o : vm.new_object(); });
    method(object_proto, "toString", [](VM&, CallArgs& a) {
        return Value(std::string(a.self.function() ? "[type Function]" : "[object Object]"));
    });
    method(object_proto, "valueOf", [](VM&, CallArgs& a) { return a.self; });
    method(object_proto, "hasOwnProperty", [](VM& vm, CallArgs& a) {
        Object* o = a.self.object();
        if (!o) return Value(false);
        Value tmp;
        const std::string n = vm.to_string(a[0]);
        return Value(o->find_own(n) != nullptr || o->get_special(vm, n, tmp));
    });
    method(object_proto, "isPropertyEnumerable", [](VM& vm, CallArgs& a) {
        Object* o = a.self.object();
        const Property* p = o ? o->find_own(vm.to_string(a[0])) : nullptr;
        return Value(p && !(p->flags & 1));
    });
    method(object_proto, "isPrototypeOf", [](VM&, CallArgs& a) {
        Object* o = a.self.object();
        int n = 0;
        for (Object* p = a[0].object() ? a[0].o->proto.get() : nullptr; p && n < 64; p = p->proto.get(), ++n)
            if (p == o) return Value(true);
        return Value(false);
    });
    method(object_proto, "addProperty", [](VM& vm, CallArgs& a) {
        Object* o = a.self.object();
        if (!o || !a[1].function()) return Value(false);
        Property& p = o->own(vm.to_string(a[0]));
        p.getter = std::static_pointer_cast<Function>(a[1].o);
        p.setter = a[2].function() ? std::static_pointer_cast<Function>(a[2].o) : nullptr;
        return Value(true);
    });
    method(object_proto, "watch", [](VM&, CallArgs&) { return Value(false); });
    method(object_proto, "unwatch", [](VM&, CallArgs&) { return Value(false); });
    method(ObjectPtr(object_ctor), "registerClass", [](VM& vm, CallArgs& a) {
        const std::string name = vm.to_string(a[0]);
        if (a[1].function()) vm.registered[name] = a[1].o;
        else vm.registered.erase(name);
        return Value(true);
    });

    // --- Function
    auto function_ctor = klass("Function", function_proto, [](VM&, CallArgs&) { return Value(); });
    (void)function_ctor;
    method(function_proto, "call", [](VM& vm, CallArgs& a) {
        std::vector<Value> rest(a.args.size() > 1 ? a.args.begin() + 1 : a.args.end(), a.args.end());
        return vm.call(a.self, a[0], std::move(rest));
    });
    method(function_proto, "apply", [](VM& vm, CallArgs& a) {
        std::vector<Value> rest;
        if (ArrayObject* arr = a[1].object() ? a[1].o->as_array() : nullptr) rest = arr->items;
        return vm.call(a.self, a[0], std::move(rest));
    });

    // --- Array
    auto array_ctor = klass("Array", array_proto,
        [](VM& vm, CallArgs& a) {
            if (a.size() == 1 && a[0].type == Value::Number) {
                auto arr = vm.new_array();
                arr->items.resize(size_t(std::max(0.0, std::min(a[0].n, 1e6))));
                return Value(ObjectPtr(arr));
            }
            return Value(ObjectPtr(vm.new_array(a.args)));
        },
        [](VM& vm, CallArgs& a) -> ObjectPtr {
            if (a.size() == 1 && a[0].type == Value::Number) {
                auto arr = vm.new_array();
                arr->items.resize(size_t(std::max(0.0, std::min(a[0].n, 1e6))));
                return arr;
            }
            return vm.new_array(a.args);
        });
    for (auto [k, v] : {std::pair{"CASEINSENSITIVE", 1}, {"DESCENDING", 2}, {"UNIQUESORT", 4}, {"RETURNINDEXEDARRAY", 8}, {"NUMERIC", 16}})
        array_ctor->set_own(k, Value(v), 1);
    auto items = [](CallArgs& a) -> std::vector<Value>* {
        ArrayObject* arr = a.self.object() ? a.self.o->as_array() : nullptr;
        return arr ? &arr->items : nullptr;
    };
    method(array_proto, "push", [items](VM&, CallArgs& a) {
        auto* v = items(a);
        if (!v) return Value();
        v->insert(v->end(), a.args.begin(), a.args.end());
        return Value(double(v->size()));
    });
    method(array_proto, "pop", [items](VM&, CallArgs& a) {
        auto* v = items(a);
        if (!v || v->empty()) return Value();
        Value r = v->back();
        v->pop_back();
        return r;
    });
    method(array_proto, "shift", [items](VM&, CallArgs& a) {
        auto* v = items(a);
        if (!v || v->empty()) return Value();
        Value r = v->front();
        v->erase(v->begin());
        return r;
    });
    method(array_proto, "unshift", [items](VM&, CallArgs& a) {
        auto* v = items(a);
        if (!v) return Value();
        v->insert(v->begin(), a.args.begin(), a.args.end());
        return Value(double(v->size()));
    });
    method(array_proto, "join", [items](VM& vm, CallArgs& a) {
        auto* v = items(a);
        if (!v) return Value(std::string());
        const std::string sep = a[0].undefined() ? "," : vm.to_string(a[0]);
        std::string out;
        for (size_t i = 0; i < v->size(); ++i) {
            if (i) out += sep;
            if (!(*v)[i].nullish()) out += vm.to_string((*v)[i]);
        }
        return Value(out);
    });
    method(array_proto, "toString", [](VM& vm, CallArgs& a) { return vm.call_method(a.self, "join", {}); });
    method(array_proto, "concat", [items](VM& vm, CallArgs& a) {
        auto* v = items(a);
        auto out = vm.new_array(v ? *v : std::vector<Value>{});
        for (const Value& x : a.args) {
            if (ArrayObject* arr = x.object() ? x.o->as_array() : nullptr)
                out->items.insert(out->items.end(), arr->items.begin(), arr->items.end());
            else
                out->items.push_back(x);
        }
        return Value(ObjectPtr(out));
    });
    method(array_proto, "slice", [items](VM& vm, CallArgs& a) {
        auto* v = items(a);
        auto out = vm.new_array();
        if (!v) return Value(ObjectPtr(out));
        const double len = double(v->size());
        const double s = clamp_index(a[0].undefined() ? 0 : vm.to_number(a[0]), len);
        const double e = clamp_index(a[1].undefined() ? len : vm.to_number(a[1]), len);
        for (size_t i = size_t(s); i < size_t(e); ++i) out->items.push_back((*v)[i]);
        return Value(ObjectPtr(out));
    });
    method(array_proto, "splice", [items](VM& vm, CallArgs& a) {
        auto* v = items(a);
        auto removed = vm.new_array();
        if (!v) return Value(ObjectPtr(removed));
        const double len = double(v->size());
        const size_t s = size_t(clamp_index(vm.to_number(a[0]), len));
        size_t n = a.size() < 2 ? v->size() - s : size_t(std::clamp(vm.to_number(a[1]), 0.0, double(v->size() - s)));
        if (std::isnan(vm.to_number(a[1])) && a.size() >= 2) n = 0;
        removed->items.assign(v->begin() + long(s), v->begin() + long(s + n));
        v->erase(v->begin() + long(s), v->begin() + long(s + n));
        if (a.size() > 2) v->insert(v->begin() + long(s), a.args.begin() + 2, a.args.end());
        return Value(ObjectPtr(removed));
    });
    method(array_proto, "reverse", [items](VM&, CallArgs& a) {
        if (auto* v = items(a)) std::reverse(v->begin(), v->end());
        return a.self;
    });
    method(array_proto, "sort", [items](VM& vm, CallArgs& a) {
        auto* v = items(a);
        if (!v) return a.self;
        Value cmp = a[0].function() ? a[0] : Value();
        const int opts = int(vm.to_number(a[0].function() ? a[1] : a[0])) & 31;
        std::vector<Value> copy = *v;
        std::stable_sort(copy.begin(), copy.end(), [&](const Value& x, const Value& y) {
            bool less;
            if (cmp.function()) less = vm.to_number(vm.call(cmp, Value(), {x, y})) < 0;
            else if (opts & 16) less = vm.to_number(x) < vm.to_number(y);
            else if (opts & 1) less = to_lower(vm.to_string(x)) < to_lower(vm.to_string(y));
            else less = vm.to_string(x) < vm.to_string(y);
            return less;
        });
        if (opts & 2) std::reverse(copy.begin(), copy.end());
        *v = std::move(copy);
        return a.self;
    });
    method(array_proto, "sortOn", [items](VM& vm, CallArgs& a) {
        auto* v = items(a);
        if (!v) return a.self;
        const std::string field = vm.to_string(a[0]);
        const int opts = int(vm.to_number(a[1])) & 31;
        std::stable_sort(v->begin(), v->end(), [&](const Value& x, const Value& y) {
            const Value fx = vm.get_member(x, field), fy = vm.get_member(y, field);
            return (opts & 16) ? vm.to_number(fx) < vm.to_number(fy) : vm.to_string(fx) < vm.to_string(fy);
        });
        if (opts & 2) std::reverse(v->begin(), v->end());
        return a.self;
    });

    // --- String
    auto string_ctor = klass("String", string_proto,
        [](VM& vm, CallArgs& a) { return Value(a.size() ? vm.to_string(a[0]) : std::string()); },
        [](VM& vm, CallArgs& a) -> ObjectPtr {
            auto o = std::make_shared<Object>(vm.string_proto);
            o->primitive = Value(a.size() ? vm.to_string(a[0]) : std::string());
            return o;
        });
    method(ObjectPtr(string_ctor), "fromCharCode", [](VM& vm, CallArgs& a) {
        std::u32string s;
        for (const Value& v : a.args) s.push_back(char32_t(uint32_t(vm.to_number(v))));
        return Value(encode(s));
    });
    auto str = [](VM& vm, CallArgs& a) { return decode(vm.to_string(a.self)); };
    method(string_proto, "toString", [](VM& vm, CallArgs& a) { return Value(vm.to_string(a.self)); });
    method(string_proto, "valueOf", [](VM& vm, CallArgs& a) { return Value(vm.to_string(a.self)); });
    method(string_proto, "charAt", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a);
        const double i = vm.to_number(a[0]);
        return Value(i >= 0 && i < double(s.size()) ? encode(s.substr(size_t(i), 1)) : std::string());
    });
    method(string_proto, "charCodeAt", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a);
        const double i = vm.to_number(a[0]);
        return Value(i >= 0 && i < double(s.size()) ? double(s[size_t(i)]) : std::numeric_limits<double>::quiet_NaN());
    });
    method(string_proto, "concat", [](VM& vm, CallArgs& a) {
        std::string s = vm.to_string(a.self);
        for (const Value& v : a.args) s += vm.to_string(v);
        return Value(s);
    });
    method(string_proto, "indexOf", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a), f = decode(vm.to_string(a[0]));
        const size_t from = a.size() > 1 ? size_t(std::max(0.0, vm.to_number(a[1]))) : 0;
        const size_t r = s.find(f, from);
        return Value(r == std::u32string::npos ? -1.0 : double(r));
    });
    method(string_proto, "lastIndexOf", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a), f = decode(vm.to_string(a[0]));
        const size_t from = a.size() > 1 ? size_t(std::max(0.0, vm.to_number(a[1]))) : std::u32string::npos;
        const size_t r = s.rfind(f, from);
        return Value(r == std::u32string::npos ? -1.0 : double(r));
    });
    method(string_proto, "substr", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a);
        const double len = double(s.size());
        const double st = clamp_index(vm.to_number(a[0]), len);
        double n = a[1].undefined() ? len - st : vm.to_number(a[1]);
        if (std::isnan(n) || n < 0) n = 0;
        return Value(encode(s.substr(size_t(st), size_t(std::min(n, len - st)))));
    });
    method(string_proto, "substring", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a);
        const double len = double(s.size());
        double x = std::clamp(std::isnan(vm.to_number(a[0])) ? 0 : vm.to_number(a[0]), 0.0, len);
        double y = a[1].undefined() ? len : std::clamp(std::isnan(vm.to_number(a[1])) ? 0 : vm.to_number(a[1]), 0.0, len);
        if (x > y) std::swap(x, y);
        return Value(encode(s.substr(size_t(x), size_t(y - x))));
    });
    method(string_proto, "slice", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a);
        const double len = double(s.size());
        const double x = clamp_index(vm.to_number(a[0]), len);
        const double y = a[1].undefined() ? len : clamp_index(vm.to_number(a[1]), len);
        return Value(y > x ? encode(s.substr(size_t(x), size_t(y - x))) : std::string());
    });
    method(string_proto, "split", [str](VM& vm, CallArgs& a) {
        const auto s = str(vm, a);
        auto out = vm.new_array();
        if (a[0].undefined()) {
            out->items.emplace_back(encode(s));
            return Value(ObjectPtr(out));
        }
        const auto sep = decode(vm.to_string(a[0]));
        if (sep.empty()) {
            for (char32_t c : s) out->items.emplace_back(encode(std::u32string(1, c)));
        } else {
            size_t p = 0;
            for (;;) {
                const size_t q = s.find(sep, p);
                out->items.emplace_back(encode(s.substr(p, q == std::u32string::npos ? std::u32string::npos : q - p)));
                if (q == std::u32string::npos) break;
                p = q + sep.size();
            }
        }
        return Value(ObjectPtr(out));
    });
    method(string_proto, "toLowerCase", [str](VM& vm, CallArgs& a) {
        auto s = str(vm, a);
        for (auto& c : s) c = lower(c);
        return Value(encode(s));
    });
    method(string_proto, "toUpperCase", [str](VM& vm, CallArgs& a) {
        auto s = str(vm, a);
        for (auto& c : s) c = upper(c);
        return Value(encode(s));
    });

    // --- Number, Boolean
    auto number_ctor = klass("Number", number_proto,
        [](VM& vm, CallArgs& a) { return Value(a.size() ? vm.to_number(a[0]) : 0.0); },
        [](VM& vm, CallArgs& a) -> ObjectPtr {
            auto o = std::make_shared<Object>(vm.number_proto);
            o->primitive = Value(a.size() ? vm.to_number(a[0]) : 0.0);
            return o;
        });
    number_ctor->set_own("MAX_VALUE", Value(std::numeric_limits<double>::max()), 1);
    number_ctor->set_own("MIN_VALUE", Value(std::numeric_limits<double>::denorm_min()), 1);
    number_ctor->set_own("NaN", Value(std::numeric_limits<double>::quiet_NaN()), 1);
    number_ctor->set_own("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()), 1);
    number_ctor->set_own("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()), 1);
    method(number_proto, "toString", [](VM& vm, CallArgs& a) {
        const double n = vm.to_number(a.self);
        const int radix = a[0].undefined() ? 10 : int(vm.to_number(a[0]));
        if (radix == 10 || radix < 2 || radix > 36 || std::isnan(n) || std::isinf(n)) return Value(VM::number_to_string(n));
        long long v = (long long)std::trunc(n);
        const bool neg = v < 0;
        if (neg) v = -v;
        std::string s;
        do {
            s.insert(s.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[v % radix]);
            v /= radix;
        } while (v);
        return Value(neg ? "-" + s : s);
    });
    method(number_proto, "valueOf", [](VM& vm, CallArgs& a) { return Value(vm.to_number(a.self)); });
    klass("Boolean", boolean_proto, [](VM&, CallArgs& a) { return Value(VM::to_boolean(a[0])); },
          [](VM& vm, CallArgs& a) -> ObjectPtr {
              auto o = std::make_shared<Object>(vm.boolean_proto);
              o->primitive = Value(VM::to_boolean(a[0]));
              return o;
          });
    method(boolean_proto, "toString", [](VM& vm, CallArgs& a) { return Value(vm.to_string(a.self)); });
    method(boolean_proto, "valueOf", [](VM&, CallArgs& a) { return Value(VM::to_boolean(a.self)); });

    // --- Math
    auto math = new_object();
    global->set_own("Math", Value(math), 1);
    const std::pair<const char*, double> constants[] = {{"E", M_E}, {"LN10", M_LN10}, {"LN2", M_LN2}, {"LOG10E", M_LOG10E},
                                                        {"LOG2E", M_LOG2E}, {"PI", M_PI}, {"SQRT1_2", M_SQRT1_2}, {"SQRT2", M_SQRT2}};
    for (auto [k, v] : constants) math->set_own(k, Value(v), 1);
    auto math1 = [&](const char* name, double (*f)(double)) {
        method(math, name, [f](VM& vm, CallArgs& a) { return Value(f(vm.to_number(a[0]))); });
    };
    math1("abs", [](double x) { return std::abs(x); });
    math1("acos", [](double x) { return std::acos(x); });
    math1("asin", [](double x) { return std::asin(x); });
    math1("atan", [](double x) { return std::atan(x); });
    math1("ceil", [](double x) { return std::ceil(x); });
    math1("cos", [](double x) { return std::cos(x); });
    math1("exp", [](double x) { return std::exp(x); });
    math1("floor", [](double x) { return std::floor(x); });
    math1("log", [](double x) { return std::log(x); });
    math1("round", [](double x) { return std::floor(x + 0.5); });
    math1("sin", [](double x) { return std::sin(x); });
    math1("sqrt", [](double x) { return std::sqrt(x); });
    math1("tan", [](double x) { return std::tan(x); });
    method(math, "atan2", [](VM& vm, CallArgs& a) { return Value(std::atan2(vm.to_number(a[0]), vm.to_number(a[1]))); });
    method(math, "pow", [](VM& vm, CallArgs& a) { return Value(std::pow(vm.to_number(a[0]), vm.to_number(a[1]))); });
    method(math, "random", [](VM&, CallArgs&) { return Value(double(std::rand()) / (double(RAND_MAX) + 1.0)); });
    method(math, "max", [](VM& vm, CallArgs& a) {
        double r = -std::numeric_limits<double>::infinity();
        for (const Value& v : a.args) {
            const double x = vm.to_number(v);
            if (std::isnan(x)) return Value(x);
            r = std::max(r, x);
        }
        return Value(r);
    });
    method(math, "min", [](VM& vm, CallArgs& a) {
        double r = std::numeric_limits<double>::infinity();
        for (const Value& v : a.args) {
            const double x = vm.to_number(v);
            if (std::isnan(x)) return Value(x);
            r = std::min(r, x);
        }
        return Value(r);
    });

    // --- global functions
    method(global, "parseInt", [](VM& vm, CallArgs& a) {
        const std::string s = vm.to_string(a[0]);
        int radix = a[1].undefined() ? 0 : int(vm.to_number(a[1]));
        const char* p = s.c_str();
        while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
        if (radix == 0) radix = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? 16 : 10;
        char* end = nullptr;
        const long long v = std::strtoll(p, &end, radix);
        return end == p ? Value(std::numeric_limits<double>::quiet_NaN()) : Value(double(v));
    });
    method(global, "parseFloat", [](VM& vm, CallArgs& a) {
        const std::string s = vm.to_string(a[0]);
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        return end == s.c_str() ? Value(std::numeric_limits<double>::quiet_NaN()) : Value(v);
    });
    method(global, "isNaN", [](VM& vm, CallArgs& a) { return Value(std::isnan(vm.to_number(a[0]))); });
    method(global, "isFinite", [](VM& vm, CallArgs& a) { return Value(std::isfinite(vm.to_number(a[0]))); });
    method(global, "int", [](VM& vm, CallArgs& a) { return Value(std::trunc(vm.to_number(a[0]))); });
    method(global, "getTimer", [](VM& vm, CallArgs&) { return Value(std::floor(vm.time_ms)); });
    method(global, "getVersion", [](VM&, CallArgs&) { return Value("AND 8,0,0,0"); });
    method(global, "trace", [](VM& vm, CallArgs& a) {
        if (vm.trace) vm.trace(vm.to_string(a[0]));
        return Value();
    });
    method(global, "escape", [](VM& vm, CallArgs& a) {
        const std::string s = vm.to_string(a[0]);
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c)) out += char(c);
            else {
                char b[4];
                std::snprintf(b, sizeof b, "%%%02X", c);
                out += b;
            }
        }
        return Value(out);
    });
    method(global, "unescape", [](VM& vm, CallArgs& a) {
        const std::string s = vm.to_string(a[0]);
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            } else {
                out += s[i] == '+' ? ' ' : s[i];
            }
        }
        return Value(out);
    });
    method(global, "ASSetPropFlags", [](VM& vm, CallArgs& a) {
        Object* o = a[0].object();
        if (!o) return Value();
        std::vector<std::string> names;
        if (a[1].is_null()) {
            names = o->own_names();
        } else if (ArrayObject* arr = a[1].object() ? a[1].o->as_array() : nullptr) {
            for (const Value& v : arr->items) names.push_back(vm.to_string(v));
        } else {
            const std::string list = vm.to_string(a[1]);
            size_t p = 0;
            for (;;) {
                const size_t q = list.find(',', p);
                names.push_back(list.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        const int set = int(vm.to_number(a[2])), clear = a[3].undefined() ? 0 : int(vm.to_number(a[3]));
        for (const std::string& n : names)
            if (Property* p = o->find_own(n)) p->flags = uint8_t((p->flags & ~clear) | set);
        return Value();
    });

    // Timers: setInterval(fn, ms, ...) / setInterval(obj, "method", ms, ...), setTimeout.
    auto add_timer = [](VM& vm, CallArgs& a, bool once) {
        VM::Timer t;
        size_t i = 0;
        if (a[0].function()) {
            t.fn = a[0];
            i = 1;
        } else {
            t.obj = a[0];
            t.method = vm.to_string(a[1]);
            i = 2;
        }
        t.period = std::max(1.0, vm.to_number(a[i]));
        t.next = vm.time_ms + t.period;
        t.args.assign(a.args.size() > i + 1 ? a.args.begin() + long(i + 1) : a.args.end(), a.args.end());
        t.once = once;
        t.id = ++vm.next_timer;
        vm.timers.push_back(std::move(t));
        return Value(double(vm.next_timer));
    };
    method(global, "setInterval", [add_timer](VM& vm, CallArgs& a) { return add_timer(vm, a, false); });
    method(global, "setTimeout", [add_timer](VM& vm, CallArgs& a) { return add_timer(vm, a, true); });
    auto clear_timer = [](VM& vm, CallArgs& a) {
        const int id = int(vm.to_number(a[0]));
        for (auto& t : vm.timers)
            if (t.id == id) t.dead = true;
        return Value();
    };
    method(global, "clearInterval", clear_timer);
    method(global, "clearTimeout", clear_timer);

    // --- TextFormat: a plain record of text attributes.
    klass("TextFormat", textformat_proto, [](VM&, CallArgs&) { return Value(); },
          [](VM& vm, CallArgs& a) -> ObjectPtr {
              auto o = std::make_shared<Object>(vm.textformat_proto);
              const char* fields[] = {"font", "size", "color", "bold", "italic", "underline", "url", "target",
                                      "align", "leftMargin", "rightMargin", "indent", "leading"};
              for (size_t i = 0; i < std::size(fields); ++i) o->set_own(fields[i], i < a.size() ? a[i] : Value::null());
              return o;
          });

    // --- Key, Mouse, Stage, Selection, System, Sound (the host fills in what it needs)
    auto key = new_object();
    global->set_own("Key", Value(key), 1);
    const std::pair<const char*, int> keys[] = {{"BACKSPACE", 8}, {"CAPSLOCK", 20}, {"CONTROL", 17}, {"DELETEKEY", 46},
                                                {"DOWN", 40}, {"END", 35}, {"ENTER", 13}, {"ESCAPE", 27}, {"HOME", 36},
                                                {"INSERT", 45}, {"LEFT", 37}, {"PGDN", 34}, {"PGUP", 33}, {"RIGHT", 39},
                                                {"SHIFT", 16}, {"SPACE", 32}, {"TAB", 9}, {"UP", 38}};
    for (auto [k, v] : keys) key->set_own(k, Value(v), 1);
    method(key, "isDown", [](VM& vm, CallArgs& a) { return Value(vm.key_down && vm.key_down(int(vm.to_number(a[0])))); });
    method(key, "getCode", [](VM& vm, CallArgs&) { return Value(vm.last_key); });
    method(key, "getAscii", [](VM& vm, CallArgs&) { return Value(vm.last_key); });
    auto listeners = [](const char* field) {
        return [field](VM& vm, CallArgs& a) {
            Object* o = a.self.object();
            if (!o) return Value();
            Value list = vm.get_member(a.self, field);
            if (!list.is_object()) {
                list = Value(ObjectPtr(vm.new_array()));
                o->set_own(field, list, 1);
            }
            if (ArrayObject* arr = list.o->as_array()) arr->items.push_back(a[0]);
            return Value(true);
        };
    };
    auto unlisten = [](const char* field) {
        return [field](VM& vm, CallArgs& a) {
            const Value list = vm.get_member(a.self, field);
            if (ArrayObject* arr = list.object() ? list.o->as_array() : nullptr) {
                auto& v = arr->items;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const Value& x) { return VM::strict_equals(x, a[0]); }), v.end());
            }
            return Value(true);
        };
    };
    method(key, "addListener", listeners("_listeners"));
    method(key, "removeListener", unlisten("_listeners"));
    auto mouse = new_object();
    global->set_own("Mouse", Value(mouse), 1);
    method(mouse, "show", [](VM&, CallArgs&) { return Value(); });
    method(mouse, "hide", [](VM&, CallArgs&) { return Value(); });
    method(mouse, "addListener", listeners("_listeners"));
    method(mouse, "removeListener", unlisten("_listeners"));
    auto stage = new_object();
    global->set_own("Stage", Value(stage), 1);
    stage->set_own("scaleMode", Value("noScale"));
    stage->set_own("align", Value(""));
    method(stage, "addListener", listeners("_listeners"));
    method(stage, "removeListener", unlisten("_listeners"));
    auto selection = new_object();
    global->set_own("Selection", Value(selection), 1);
    method(selection, "getFocus", [](VM&, CallArgs&) { return Value::null(); });
    method(selection, "setFocus", [](VM&, CallArgs&) { return Value(true); });
    method(selection, "addListener", listeners("_listeners"));
    method(selection, "removeListener", unlisten("_listeners"));
    auto system = new_object();
    global->set_own("System", Value(system), 1);
    auto caps = new_object();
    caps->set_own("language", Value("en"));
    caps->set_own("os", Value("TDKR"));
    system->set_own("capabilities", Value(caps));
    auto sound_proto = new_object();
    klass("Sound", sound_proto, [](VM&, CallArgs&) { return Value(); });
    method(sound_proto, "attachSound", [](VM& vm, CallArgs& a) {
        if (Object* o = a.self.object()) o->set_own("_linkage", Value(vm.to_string(a[0])), 1);
        return Value();
    });
    method(sound_proto, "start", [](VM& vm, CallArgs& a) {
        if (vm.play_sound) vm.play_sound(vm.to_string(vm.get_member(a.self, "_linkage")));
        return Value();
    });
    for (const char* n : {"stop", "setVolume", "setPan", "setTransform", "loadSound"})
        method(sound_proto, n, [](VM&, CallArgs&) { return Value(); });
    method(sound_proto, "getVolume", [](VM&, CallArgs&) { return Value(100); });
    method(sound_proto, "getPan", [](VM&, CallArgs&) { return Value(0); });
}

void VM::update_timers(double now_ms) {
    time_ms = now_ms;
    for (size_t i = 0; i < timers.size(); ++i) {
        if (timers[i].dead || timers[i].next > now_ms) continue;
        Timer t = timers[i];
        timers[i].next = now_ms + t.period;
        if (t.once) timers[i].dead = true;
        if (t.fn.function()) call(t.fn, Value(), t.args);
        else call_method(t.obj, t.method, t.args);
    }
    timers.erase(std::remove_if(timers.begin(), timers.end(), [](const Timer& t) { return t.dead; }), timers.end());
}

}  // namespace tdkr::swf
