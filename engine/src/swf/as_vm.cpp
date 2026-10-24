#include "swf/as.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "swf/player.h"

namespace tdkr::swf {

namespace {
constexpr size_t kStepBudget = 20'000'000;   // instructions per top-level run (runaway scripts)
constexpr int kMaxDepth = 120;

bool is_index(const std::string& s) {
    if (s.empty() || s.size() > 9) return false;
    if (s.size() > 1 && s[0] == '0') return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}
}  // namespace

// --- objects -------------------------------------------------------------------------------

Property* Object::find_own(const std::string& name) {
    auto it = props_.find(name);
    return it == props_.end() ? nullptr : &it->second;
}

const Property* Object::find_own(const std::string& name) const {
    auto it = props_.find(name);
    return it == props_.end() ? nullptr : &it->second;
}

Property& Object::own(const std::string& name) {
    auto [it, added] = props_.try_emplace(name);
    if (added) order_.push_back(name);
    return it->second;
}

void Object::set_own(const std::string& name, Value v, uint8_t flags) {
    Property& p = own(name);
    p.value = std::move(v);
    p.flags = flags;
}

bool Object::remove_own(const std::string& name) {
    auto it = props_.find(name);
    if (it == props_.end() || (it->second.flags & 2)) return false;
    props_.erase(it);
    for (size_t i = 0; i < order_.size(); ++i)
        if (order_[i] == name) {
            order_.erase(order_.begin() + long(i));
            break;
        }
    return true;
}

bool ArrayObject::get_special(VM&, const std::string& name, Value& out) {
    if (name == "length") {
        out = Value(double(items.size()));
        return true;
    }
    if (is_index(name)) {
        const size_t i = size_t(std::stoul(name));
        out = i < items.size() ? items[i] : Value();
        return i < items.size();
    }
    return false;
}

bool ArrayObject::set_special(VM& vm, const std::string& name, const Value& v) {
    if (name == "length") {
        const double n = vm.to_number(v);
        if (n >= 0 && n < 1e7) items.resize(size_t(n));
        return true;
    }
    if (is_index(name)) {
        const size_t i = size_t(std::stoul(name));
        if (i >= 10'000'000) return true;
        if (i >= items.size()) items.resize(i + 1);
        items[i] = v;
        return true;
    }
    return false;
}

void ArrayObject::special_names(std::vector<std::string>& out) const {
    for (size_t i = 0; i < items.size(); ++i) out.push_back(std::to_string(i));
}

// --- VM: builders, conversions -----------------------------------------------------------------

VM::VM() { install_builtins(); }
VM::~VM() = default;

ObjectPtr VM::new_object() { return std::make_shared<Object>(object_proto); }

std::shared_ptr<ArrayObject> VM::new_array(std::vector<Value> items) {
    auto a = std::make_shared<ArrayObject>(array_proto);
    a->items = std::move(items);
    return a;
}

std::shared_ptr<NativeFunction> VM::native(NativeFunction::Fn fn, NativeFunction::Ctor ctor) {
    return std::make_shared<NativeFunction>(function_proto, std::move(fn), std::move(ctor));
}

void VM::method(const ObjectPtr& on, const std::string& name, NativeFunction::Fn fn) {
    on->set_own(name, Value(ObjectPtr(native(std::move(fn)))), 1);
}

std::shared_ptr<Function> VM::function_with_prototype(std::shared_ptr<Function> f) {
    auto proto = new_object();
    proto->set_own("constructor", Value(ObjectPtr(f)), 1);
    f->set_own("prototype", Value(proto), 1);
    return f;
}

ObjectPtr VM::current_target() const {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it)
        if ((*it)->target) return (*it)->target;
    return root ? root() : nullptr;
}

std::string VM::number_to_string(double n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
    if (n == 0) return "0";
    if (std::abs(n) < 1e15 && n == std::floor(n)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", n);
        return buf;
    }
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.15g", n);
    std::string s = buf;
    // 1e-07 -> 1e-7 (Flash drops the exponent's leading zeros)
    if (auto e = s.find('e'); e != std::string::npos) {
        size_t d = e + 2;
        while (d < s.size() - 1 && s[d] == '0') s.erase(d, 1);
    }
    return s;
}

Value VM::to_primitive(const Value& v, bool prefer_string) {
    if (!v.is_object()) return v;
    Object* o = v.o.get();
    if (!o->primitive.undefined()) return o->primitive;
    if (o->as_instance()) return Value(to_string(v));
    const char* first = prefer_string ? "toString" : "valueOf";
    const char* second = prefer_string ? "valueOf" : "toString";
    for (const char* name : {first, second}) {
        Value fn;
        if (lookup(o, name, fn, v) && fn.function()) {
            Value r = call(fn, v, {});
            if (!r.is_object()) return r;
        }
    }
    return Value(std::string(o->as_function() ? "[type Function]" : "[object Object]"));
}

double VM::to_number(const Value& v) {
    switch (v.type) {
        case Value::Undefined:
        case Value::Null: return std::numeric_limits<double>::quiet_NaN();
        case Value::Boolean: return v.b ? 1 : 0;
        case Value::Number: return v.n;
        case Value::String: {
            const char* s = v.s.c_str();
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
            if (!*s) return std::numeric_limits<double>::quiet_NaN();
            char* end = nullptr;
            double d;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                d = double(std::strtoll(s + 2, &end, 16));
            else
                d = std::strtod(s, &end);
            while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) ++end;
            return end && !*end ? d : std::numeric_limits<double>::quiet_NaN();
        }
        case Value::Obj: {
            const Value p = to_primitive(v);
            return p.is_object() ? std::numeric_limits<double>::quiet_NaN() : to_number(p);
        }
    }
    return 0;
}

std::string VM::to_string(const Value& v) {
    switch (v.type) {
        case Value::Undefined: return "undefined";
        case Value::Null: return "null";
        case Value::Boolean: return v.b ? "true" : "false";
        case Value::Number: return number_to_string(v.n);
        case Value::String: return v.s;
        case Value::Obj: {
            if (Instance* i = v.o->as_instance()) return i->target_path();
            const Value p = to_primitive(v, true);
            return p.is_object() ? "[object Object]" : to_string(p);
        }
    }
    return {};
}

bool VM::to_boolean(const Value& v) {
    switch (v.type) {
        case Value::Undefined:
        case Value::Null: return false;
        case Value::Boolean: return v.b;
        case Value::Number: return v.n != 0 && !std::isnan(v.n);
        case Value::String: return !v.s.empty();
        case Value::Obj: return true;
    }
    return false;
}

bool VM::strict_equals(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case Value::Undefined:
        case Value::Null: return true;
        case Value::Boolean: return a.b == b.b;
        case Value::Number: return a.n == b.n;
        case Value::String: return a.s == b.s;
        case Value::Obj: return a.o == b.o;
    }
    return false;
}

bool VM::equals(const Value& a, const Value& b) {
    if (a.type == b.type) return strict_equals(a, b);
    if (a.nullish() && b.nullish()) return true;
    if (a.nullish() || b.nullish()) return false;
    if (a.type == Value::Number && b.type == Value::String) return a.n == to_number(b);
    if (a.type == Value::String && b.type == Value::Number) return to_number(a) == b.n;
    if (a.type == Value::Boolean) return equals(Value(a.b ? 1.0 : 0.0), b);
    if (b.type == Value::Boolean) return equals(a, Value(b.b ? 1.0 : 0.0));
    if (a.is_object() && !b.is_object()) {
        const Value p = to_primitive(a);
        return p.is_object() ? false : equals(p, b);
    }
    if (b.is_object() && !a.is_object()) {
        const Value p = to_primitive(b);
        return p.is_object() ? false : equals(a, p);
    }
    return false;
}

bool VM::instance_of(const Value& v, const Value& ctor) {
    if (!v.is_object() || !ctor.is_object()) return false;
    const Value proto = get_member(ctor, "prototype");
    if (!proto.is_object()) return false;
    std::function<bool(Object*, int)> implements = [&](Object* p, int depth) -> bool {
        if (!p || depth > 32) return false;
        for (const ObjectPtr& i : p->interfaces) {
            if (i == proto.o || implements(i.get(), depth + 1)) return true;
        }
        return false;
    };
    int n = 0;
    for (Object* p = v.o->proto.get(); p && n < 64; p = p->proto.get(), ++n) {
        if (p == proto.o.get() || implements(p, 0)) return true;
    }
    return false;
}

// --- members -------------------------------------------------------------------------------------

bool VM::lookup(Object* obj, const std::string& name, Value& out, const Value& self, ObjectPtr* owner) {
    int n = 0;
    for (Object* o = obj; o && n < 64; o = o->proto.get(), ++n) {
        if (o->get_special(*this, name, out)) {
            if (owner) *owner = o->shared_from_this();
            return true;
        }
        if (Property* p = o->find_own(name)) {
            if (p->getter) {
                std::shared_ptr<Function> g = p->getter;
                out = call(Value(ObjectPtr(g)), self, {});
            } else {
                out = p->value;
            }
            if (owner) *owner = o->shared_from_this();
            return true;
        }
    }
    return false;
}

Value VM::get_member(const Value& obj, const std::string& name) {
    Value out;
    switch (obj.type) {
        case Value::Undefined:
        case Value::Null: return out;
        case Value::String:
            if (name == "length") {
                size_t len = 0;
                for (unsigned char c : obj.s) len += (c & 0xC0) != 0x80;
                return Value(double(len));
            }
            lookup(string_proto.get(), name, out, obj);
            return out;
        case Value::Number: lookup(number_proto.get(), name, out, obj); return out;
        case Value::Boolean: lookup(boolean_proto.get(), name, out, obj); return out;
        case Value::Obj: break;
    }
    if (auto* s = dynamic_cast<SuperObject*>(obj.o.get())) {
        lookup(s->from.get(), name, out, s->self);
        return out;
    }
    if (name == "__proto__") return Value(obj.o->proto);
    lookup(obj.o.get(), name, out, obj);
    return out;
}

bool VM::has_member(const Value& obj, const std::string& name) {
    if (!obj.is_object()) return false;
    Value out;
    return lookup(obj.o.get(), name, out, obj);
}

void VM::set_member(const Value& obj, const std::string& name, const Value& v) {
    if (!obj.is_object()) return;
    Object* o = obj.o.get();
    if (auto* s = dynamic_cast<SuperObject*>(o)) {
        set_member(s->self, name, v);
        return;
    }
    if (name == "__proto__") {
        o->proto = v.is_object() ? v.o : nullptr;
        return;
    }
    if (o->set_special(*this, name, v)) return;
    int n = 0;
    for (Object* p = o; p && n < 64; p = p->proto.get(), ++n) {
        if (Property* prop = p->find_own(name)) {
            if (prop->setter) {
                std::shared_ptr<Function> f = prop->setter;
                call(Value(ObjectPtr(f)), obj, {v});
                return;
            }
            if (prop->getter) return;       // read-only property
            if (p == o) {
                prop->value = v;
                return;
            }
            break;
        }
    }
    o->own(name).value = v;
}

Value VM::call(const Value& fn, const Value& self, std::vector<Value> args, ObjectPtr home) {
    Function* f = fn.function();
    if (!f) return {};
    if (depth > kMaxDepth) return {};
    if (stack_.empty() && depth == 0) steps = 0;   // a call from the game
    CallArgs a{self, std::move(args), std::move(home)};
    ++depth;
    Value r = f->call(*this, a);
    --depth;
    return r;
}

Value VM::call_method(const Value& obj, const std::string& name, std::vector<Value> args) {
    Value fn;
    ObjectPtr owner;
    if (obj.is_object()) {
        if (!lookup(obj.o.get(), name, fn, obj, &owner)) return {};
    } else {
        fn = get_member(obj, name);
    }
    return call(fn, obj, std::move(args), owner);
}

Value VM::construct(const Value& ctor, std::vector<Value> args) {
    Function* f = ctor.function();
    if (!f) return {};
    const Value proto = get_member(ctor, "prototype");
    CallArgs a{Value(), std::move(args), proto.is_object() ? proto.o : nullptr};
    if (ObjectPtr made = f->construct(*this, a)) return Value(made);
    auto obj = std::make_shared<Object>(proto.is_object() ? proto.o : object_proto);
    obj->set_own("__constructor__", ctor, 1);
    a.self = Value(ObjectPtr(obj));
    ++depth;
    f->call(*this, a);
    --depth;
    return Value(ObjectPtr(obj));
}

// --- script functions -------------------------------------------------------------------------

Value ScriptFunction::call(VM& vm, CallArgs& a) {
    VM::Context ctx;
    ctx.hold = code;
    ctx.code = code.get();
    ctx.pc = start;
    ctx.end = std::min(start + length, code->size());
    ctx.pool = pool;
    ctx.scope = scope;
    ctx.locals = std::make_shared<Object>(nullptr);
    ctx.self = a.self.undefined() && target ? Value(target) : a.self;
    ctx.target = target;
    ctx.original_target = target;
    ctx.home = a.home;
    if (!ctx.home && ctx.self.is_object()) ctx.home = ctx.self.o->proto;
    auto args = vm.new_array(a.args);
    args->set_own("callee", Value(shared_from_this()), 1);
    ctx.arguments = args;
    ctx.regs.assign(v2 ? std::max<size_t>(registers, 1) : 4, Value());
    auto super_object = [&]() -> Value {
        if (!ctx.home) return {};
        Value ctor;
        if (Property* p = ctx.home->find_own("__constructor__")) ctor = p->value;
        if (!ctor.function() && ctx.home->proto) vm.lookup(ctx.home->proto.get(), "constructor", ctor, Value());
        return Value(ObjectPtr(std::make_shared<SuperObject>(ctx.self, ctx.home->proto, ctor.is_object() ? ctor.o : nullptr)));
    };
    if (v2) {
        size_t r = 1;
        auto put = [&](Value v) {
            if (r < ctx.regs.size()) ctx.regs[r] = std::move(v);
            ++r;
        };
        if (flags & 0x01) put(ctx.self);
        if (flags & 0x04) put(Value(ObjectPtr(args)));
        if (flags & 0x10) put(super_object());
        if (flags & 0x40) put(vm.root ? Value(vm.root()) : Value());
        if (flags & 0x80) {
            Instance* t = target ? target->as_instance() : nullptr;
            put(t && t->parent() ? Value(t->parent()->shared_from_this()) : Value());
        }
        if (flags & 0x100) put(Value(vm.global));
    }
    for (size_t i = 0; i < params.size(); ++i) {
        const Value v = i < a.args.size() ? a.args[i] : Value();
        if (v2 && params[i].first) {
            if (params[i].first < ctx.regs.size()) ctx.regs[params[i].first] = v;
        } else {
            ctx.locals->set_own(params[i].second, v);
        }
    }
    return vm.execute(ctx);
}

void VM::run_actions(const ActionBuffer& code, const ObjectPtr& target, size_t start, size_t length) {
    if (!code) return;
    Context ctx;
    ctx.hold = code;
    ctx.code = code.get();
    ctx.pc = start;
    ctx.end = length == size_t(-1) ? code->size() : std::min(code->size(), start + length);
    ctx.self = Value(target);
    ctx.target = target;
    ctx.original_target = target;
    if (target) ctx.scope.push_back(target);
    if (stack_.empty()) steps = 0;
    execute(ctx);
}

// --- variables -----------------------------------------------------------------------------

ObjectPtr VM::target_from(Context& ctx, const Value& v) {
    if (v.is_object()) return v.o;
    const std::string path = to_string(v);
    if (path.empty()) return ctx.original_target;
    const Value r = resolve_path(ctx, path);
    return r.is_object() ? r.o : nullptr;
}

Value VM::resolve_path(Context& ctx, const std::string& path) {
    Value cur = ctx.target ? Value(ctx.target) : Value();
    auto parent_of = [&](const Value& v) {
        Instance* inst = v.instance();
        return inst && inst->parent() ? Value(inst->parent()->shared_from_this()) : Value();
    };
    size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        cur = root ? Value(root()) : Value();
        i = 1;
    }
    bool first = true;
    while (i < path.size()) {
        if (path.compare(i, 2, "..") == 0 && (i + 2 == path.size() || path[i + 2] == '/')) {
            cur = parent_of(cur);
            i += 3;
            first = false;
            continue;
        }
        size_t j = path.find_first_of("/.:", i);
        if (j == std::string::npos) j = path.size();
        const std::string part = path.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == "this") {
        } else if (part == "_root" || part == "_level0") {
            cur = root ? Value(root()) : Value();
        } else if (part == "_global") {
            cur = Value(global);
        } else if (part == "_parent") {
            cur = parent_of(cur);
        } else {
            Value next = get_member(cur, part);
            if (first && next.undefined()) next = get_variable(ctx, part);
            cur = next;
        }
        first = false;
        if (cur.undefined()) return cur;
    }
    return cur;
}

Value VM::get_variable(Context& ctx, const std::string& name, ObjectPtr* holder) {
    if (name == "this") return ctx.self.undefined() && ctx.target ? Value(ctx.target) : ctx.self;
    if (name == "_global") return Value(global);
    if (name == "_root" || name == "_level0") return root ? Value(root()) : Value();
    if (name == "arguments" && ctx.arguments) return Value(ctx.arguments);
    if (name == "_parent") {
        Instance* t = ctx.target ? ctx.target->as_instance() : nullptr;
        return t && t->parent() ? Value(t->parent()->shared_from_this()) : Value();
    }
    if (name == "super" && ctx.home) {
        Value ctor;
        if (Property* p = ctx.home->find_own("__constructor__")) ctor = p->value;
        return Value(ObjectPtr(std::make_shared<SuperObject>(ctx.self, ctx.home->proto, ctor.is_object() ? ctor.o : nullptr)));
    }
    if (name.find_first_of("/:.") != std::string::npos && name != "." ) {
        // "a.b.c" / "/a/b:var"
        if (auto colon = name.rfind(':'); colon != std::string::npos) {
            const Value t = resolve_path(ctx, name.substr(0, colon));
            return get_member(t, name.substr(colon + 1));
        }
        return resolve_path(ctx, name);
    }
    Value out;
    for (auto it = ctx.with.rbegin(); it != ctx.with.rend(); ++it)
        if (it->second && lookup(it->second.get(), name, out, Value(it->second))) {
            if (holder) *holder = it->second;
            return out;
        }
    if (ctx.locals) {
        if (Property* p = ctx.locals->find_own(name)) return p->value;
    }
    for (auto it = ctx.scope.rbegin(); it != ctx.scope.rend(); ++it) {
        if (*it && lookup(it->get(), name, out, Value(*it))) {
            if (holder && (*it)->as_instance()) *holder = *it;
            return out;
        }
    }
    if (ctx.target && (ctx.scope.empty() || ctx.scope.back() != ctx.target) && lookup(ctx.target.get(), name, out, Value(ctx.target))) {
        if (holder) *holder = ctx.target;
        return out;
    }
    if (lookup(global.get(), name, out, Value(global))) return out;
    return {};
}

void VM::set_variable(Context& ctx, const std::string& name, const Value& v) {
    if (name.find_first_of("/:.") != std::string::npos) {
        size_t cut = name.rfind(':');
        if (cut == std::string::npos) cut = name.rfind('.');
        if (cut == std::string::npos) cut = name.rfind('/');
        const Value t = resolve_path(ctx, name.substr(0, cut));
        set_member(t, name.substr(cut + 1), v);
        return;
    }
    for (auto it = ctx.with.rbegin(); it != ctx.with.rend(); ++it)
        if (it->second && has_member(Value(it->second), name)) {
            set_member(Value(it->second), name, v);
            return;
        }
    if (ctx.locals) {
        if (Property* p = ctx.locals->find_own(name)) {
            p->value = v;
            return;
        }
    }
    for (auto it = ctx.scope.rbegin(); it != ctx.scope.rend(); ++it) {
        if (*it && (*it)->find_own(name) && !(*it)->as_instance()) {
            set_member(Value(*it), name, v);
            return;
        }
    }
    // Undeclared: the timeline the code belongs to.
    ObjectPtr t = ctx.target;
    for (auto it = ctx.scope.rbegin(); it != ctx.scope.rend() && !t; ++it) t = *it;
    if (t) set_member(Value(t), name, v);
}

// --- the interpreter ---------------------------------------------------------------------------

namespace {

std::string read_cstr(const Bytes& c, size_t& p, size_t end) {
    size_t e = p;
    while (e < end && c[e]) ++e;
    std::string s(reinterpret_cast<const char*>(c.data() + p), e - p);
    p = e + 1;
    return s;
}

const char* const kPropertyNames[] = {"_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha",
                                      "_visible", "_width", "_height", "_rotation", "_target", "_framesloaded",
                                      "_name", "_droptarget", "_url", "_highquality", "_focusrect", "_soundbuftime",
                                      "_quality", "_xmouse", "_ymouse"};

int32_t to_int32(double d) {
    if (std::isnan(d) || std::isinf(d)) return 0;
    return int32_t(uint32_t(int64_t(std::fmod(std::trunc(d), 4294967296.0))));
}

}  // namespace

Value VM::execute(Context& ctx) {
    const Bytes& c = *ctx.code;
    std::vector<Value> st;
    st.reserve(32);
    auto pop = [&]() -> Value {
        if (st.empty()) return {};
        Value v = std::move(st.back());
        st.pop_back();
        return v;
    };
    auto pop_args = [&](std::vector<Value>& args) {
        const double n = to_number(pop());
        const size_t count = std::isnan(n) || n < 0 ? 0 : std::min<size_t>(size_t(n), st.size());
        args.clear();
        for (size_t i = 0; i < count; ++i) args.push_back(pop());
    };
    stack_.push_back(&ctx);
    struct Guard {
        std::vector<Context*>& s;
        ~Guard() { s.pop_back(); }
    } guard{stack_};
    Value result;
    std::vector<Value> args;

    while (ctx.pc < ctx.end) {
        while (!ctx.with.empty() && ctx.pc >= ctx.with.back().first) ctx.with.pop_back();
        if (++steps > kStepBudget) {
            if (trace) trace("swf: script step budget exhausted");
            break;
        }
        const uint8_t op = c[ctx.pc++];
        if (op == 0) break;
        size_t data = ctx.pc, len = 0;
        if (op >= 0x80) {
            if (ctx.pc + 2 > c.size()) break;
            len = read_le<uint16_t>(c, ctx.pc);
            data = ctx.pc + 2;
            ctx.pc = data + len;
            if (ctx.pc > c.size()) break;
        }
        const size_t dend = data + len;
        Instance* tgt = ctx.target ? ctx.target->as_instance() : nullptr;

        switch (op) {
            // --- timeline
            case 0x04: if (tgt) tgt->goto_frame(tgt->current_frame() + 1, false); break;
            case 0x05: if (tgt) tgt->goto_frame(std::max(0, tgt->current_frame() - 1), false); break;
            case 0x06: if (tgt) tgt->set_playing(true); break;
            case 0x07: if (tgt) tgt->set_playing(false); break;
            case 0x08: case 0x09: break;
            case 0x81: if (tgt) tgt->goto_frame(read_le<uint16_t>(c, data), false); break;
            case 0x8C: {
                size_t p = data;
                const std::string label = read_cstr(c, p, dend);
                if (tgt) {
                    const int f = tgt->find_label(label);
                    if (f >= 0) tgt->goto_frame(f, false);
                }
                break;
            }
            case 0x9F: {
                const uint8_t flags = c[data];
                const int bias = (flags & 2) && len >= 3 ? read_le<uint16_t>(c, data + 1) : 0;
                const Value frame = pop();
                Instance* t = tgt;
                int f = -1;
                if (frame.type == Value::String) {
                    std::string s = frame.s;
                    if (auto colon = s.rfind(':'); colon != std::string::npos) {
                        const ObjectPtr o = target_from(ctx, Value(s.substr(0, colon)));
                        t = o ? o->as_instance() : nullptr;
                        s = s.substr(colon + 1);
                    }
                    if (t) {
                        f = t->find_label(s);
                        if (f < 0) {
                            const double n = to_number(Value(s));
                            if (!std::isnan(n)) f = int(n) - 1;
                        }
                    }
                } else {
                    f = int(to_number(frame)) - 1 + bias;
                }
                if (t && f >= 0) t->goto_frame(f, flags & 1);
                break;
            }
            case 0x8B: {
                size_t p = data;
                const std::string path = read_cstr(c, p, dend);
                ctx.target = path.empty() ? ctx.original_target : target_from(ctx, Value(path));
                break;
            }
            case 0x20: {
                const Value v = pop();
                ctx.target = v.is_object() ? v.o : (to_string(v).empty() ? ctx.original_target : target_from(ctx, v));
                break;
            }
            case 0x8A: break;                               // WaitForFrame: always loaded
            case 0x8D: pop(); break;
            case 0x9E: pop(); break;                        // Call (frame actions): unused
            case 0x24: {                                    // CloneSprite
                const double d = to_number(pop());
                const std::string name = to_string(pop());
                const ObjectPtr src = target_from(ctx, pop());
                if (src && src->as_instance()) src->as_instance()->duplicate(name, int(d));
                break;
            }
            case 0x25: {
                const ObjectPtr t = target_from(ctx, pop());
                if (t && t->as_instance()) t->as_instance()->remove_self();
                break;
            }
            case 0x27: {                                    // StartDrag
                pop();
                pop();
                if (to_boolean(pop())) {
                    pop(), pop(), pop(), pop();
                }
                break;
            }
            case 0x28: break;
            case 0x83: {
                size_t p = data;
                const std::string url = read_cstr(c, p, dend), window = read_cstr(c, p, dend);
                if (url.rfind("FSCommand:", 0) == 0 && fscommand) fscommand(url.substr(10), window);
                break;
            }
            case 0x9A: {
                const std::string window = to_string(pop()), url = to_string(pop());
                if (url.rfind("FSCommand:", 0) == 0 && fscommand) fscommand(url.substr(10), window);
                break;
            }

            // --- stack and constants
            case 0x88: {
                auto pool = std::make_shared<std::vector<std::string>>();
                size_t p = data + 2;
                const uint16_t n = read_le<uint16_t>(c, data);
                for (uint16_t i = 0; i < n && p < dend; ++i) pool->push_back(read_cstr(c, p, dend));
                ctx.pool = pool;
                break;
            }
            case 0x96: {
                size_t p = data;
                while (p < dend) {
                    const uint8_t t = c[p++];
                    switch (t) {
                        case 0: st.emplace_back(read_cstr(c, p, dend)); break;
                        case 1: {
                            float f;
                            std::memcpy(&f, c.data() + p, 4);
                            st.emplace_back(double(f));
                            p += 4;
                            break;
                        }
                        case 2: st.push_back(Value::null()); break;
                        case 3: st.emplace_back(); break;
                        case 4: {
                            const uint8_t r = c[p++];
                            st.push_back(r < ctx.regs.size() ? ctx.regs[r] : Value());
                            break;
                        }
                        case 5: st.emplace_back(bool(c[p++])); break;
                        case 6: {
                            uint8_t raw[8];
                            std::memcpy(raw, c.data() + p + 4, 4);
                            std::memcpy(raw + 4, c.data() + p, 4);
                            double d;
                            std::memcpy(&d, raw, 8);
                            st.emplace_back(d);
                            p += 8;
                            break;
                        }
                        case 7: st.emplace_back(double(read_le<int32_t>(c, p))); p += 4; break;
                        case 8:
                        case 9: {
                            const size_t i = t == 8 ? c[p] : read_le<uint16_t>(c, p);
                            p += t == 8 ? 1 : 2;
                            st.push_back(ctx.pool && i < ctx.pool->size() ? Value((*ctx.pool)[i]) : Value());
                            break;
                        }
                        default: p = dend; break;
                    }
                }
                break;
            }
            case 0x17: pop(); break;
            case 0x4C: st.push_back(st.empty() ? Value() : st.back()); break;
            case 0x4D: {
                Value a = pop(), b = pop();
                st.push_back(std::move(a));
                st.push_back(std::move(b));
                break;
            }
            case 0x87: {
                const uint8_t r = c[data];
                if (r < ctx.regs.size()) ctx.regs[r] = st.empty() ? Value() : st.back();
                break;
            }

            // --- arithmetic and logic
            case 0x0A: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(a + b); break; }
            case 0x0B: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(a - b); break; }
            case 0x0C: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(a * b); break; }
            case 0x0D: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(a / b); break; }
            case 0x3F: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(std::fmod(a, b)); break; }
            case 0x0E: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(a == b); break; }
            case 0x0F: { const double b = to_number(pop()), a = to_number(pop()); st.emplace_back(a < b); break; }
            case 0x10: { const bool b = to_boolean(pop()), a = to_boolean(pop()); st.emplace_back(a && b); break; }
            case 0x11: { const bool b = to_boolean(pop()), a = to_boolean(pop()); st.emplace_back(a || b); break; }
            case 0x12: st.emplace_back(!to_boolean(pop())); break;
            case 0x13: { const std::string b = to_string(pop()), a = to_string(pop()); st.emplace_back(a == b); break; }
            case 0x29: { const std::string b = to_string(pop()), a = to_string(pop()); st.emplace_back(a < b); break; }
            case 0x68: { const std::string b = to_string(pop()), a = to_string(pop()); st.emplace_back(a > b); break; }
            case 0x14:
            case 0x31: st.push_back(get_member(Value(to_string(pop())), "length")); break;
            case 0x21: { const std::string b = to_string(pop()), a = to_string(pop()); st.emplace_back(a + b); break; }
            case 0x15:
            case 0x35: {
                const double count = to_number(pop()), index = to_number(pop());
                const std::string s = to_string(pop());
                const Value r = call_method(Value(s), "substr", {Value(index - 1), Value(count)});
                st.push_back(r);
                break;
            }
            case 0x18: st.emplace_back(std::trunc(to_number(pop()))); break;
            case 0x32:
            case 0x36: {
                const std::string s = to_string(pop());
                st.push_back(call_method(Value(s), "charCodeAt", {Value(0)}));
                break;
            }
            case 0x33:
            case 0x37: {
                const double n = to_number(pop());
                Value fcc = get_member(get_member(Value(global), "String"), "fromCharCode");
                st.push_back(call(fcc, Value(), {Value(n)}));
                break;
            }
            case 0x47: {
                const Value b = to_primitive(pop()), a = to_primitive(pop());
                if (a.type == Value::String || b.type == Value::String)
                    st.emplace_back(to_string(a) + to_string(b));
                else
                    st.emplace_back(to_number(a) + to_number(b));
                break;
            }
            case 0x48:
            case 0x67: {
                Value b = to_primitive(pop()), a = to_primitive(pop());
                if (op == 0x67) std::swap(a, b);
                if (a.type == Value::String && b.type == Value::String) {
                    st.emplace_back(a.s < b.s);
                } else {
                    const double x = to_number(a), y = to_number(b);
                    if (std::isnan(x) || std::isnan(y))
                        st.emplace_back(op == 0x67 ? Value(false) : Value());
                    else
                        st.emplace_back(x < y);
                }
                break;
            }
            case 0x49: { const Value b = pop(), a = pop(); st.emplace_back(equals(a, b)); break; }
            case 0x66: { const Value b = pop(), a = pop(); st.emplace_back(strict_equals(a, b)); break; }
            case 0x50: st.emplace_back(to_number(pop()) + 1); break;
            case 0x51: st.emplace_back(to_number(pop()) - 1); break;
            case 0x60: { const int32_t b = to_int32(to_number(pop())), a = to_int32(to_number(pop())); st.emplace_back(double(a & b)); break; }
            case 0x61: { const int32_t b = to_int32(to_number(pop())), a = to_int32(to_number(pop())); st.emplace_back(double(a | b)); break; }
            case 0x62: { const int32_t b = to_int32(to_number(pop())), a = to_int32(to_number(pop())); st.emplace_back(double(a ^ b)); break; }
            case 0x63: { const int32_t b = to_int32(to_number(pop())), a = to_int32(to_number(pop())); st.emplace_back(double(int32_t(uint32_t(a) << (b & 31)))); break; }
            case 0x64: { const int32_t b = to_int32(to_number(pop())), a = to_int32(to_number(pop())); st.emplace_back(double(a >> (b & 31))); break; }
            case 0x65: { const int32_t b = to_int32(to_number(pop())), a = to_int32(to_number(pop())); st.emplace_back(double(uint32_t(a) >> (b & 31))); break; }
            case 0x4A: st.emplace_back(to_number(pop())); break;
            case 0x4B: st.emplace_back(to_string(pop())); break;
            case 0x44: {
                const Value v = pop();
                const char* t = "undefined";
                switch (v.type) {
                    case Value::Undefined: t = "undefined"; break;
                    case Value::Null: t = "null"; break;
                    case Value::Boolean: t = "boolean"; break;
                    case Value::Number: t = "number"; break;
                    case Value::String: t = "string"; break;
                    case Value::Obj: t = v.o->as_instance() ? v.o->type_of() : v.o->type_of(); break;
                }
                st.emplace_back(t);
                break;
            }
            case 0x30: {
                const double m = to_number(pop());
                st.emplace_back(m > 0 ? std::floor(double(std::rand()) / (double(RAND_MAX) + 1.0) * m) : 0.0);
                break;
            }
            case 0x34: st.emplace_back(std::floor(time_ms)); break;
            case 0x26: {
                const std::string s = to_string(pop());
                if (trace) trace(s);
                break;
            }

            // --- control flow
            case 0x99: ctx.pc = size_t(int64_t(ctx.pc) + read_le<int16_t>(c, data)); break;
            case 0x9D:
                if (to_boolean(pop())) ctx.pc = size_t(int64_t(ctx.pc) + read_le<int16_t>(c, data));
                break;
            case 0x3E: result = pop(); ctx.pc = ctx.end; break;
            case 0x2A: pop(); ctx.pc = ctx.end; break;      // Throw: abandon the function
            case 0x8F: break;                               // Try: run the try block in line
            case 0x94: {
                const uint16_t size = read_le<uint16_t>(c, data);
                const Value obj = pop();
                ctx.with.emplace_back(ctx.pc + size, obj.is_object() ? obj.o : nullptr);
                break;
            }

            // --- variables and members
            case 0x1C: st.push_back(get_variable(ctx, to_string(pop()))); break;
            case 0x1D: {
                const Value v = pop();
                set_variable(ctx, to_string(pop()), v);
                break;
            }
            case 0x3C: {
                const Value v = pop();
                const std::string name = to_string(pop());
                if (ctx.locals) ctx.locals->set_own(name, v);
                else if (ctx.target) set_member(Value(ctx.target), name, v);
                break;
            }
            case 0x41: {
                const std::string name = to_string(pop());
                if (ctx.locals && !ctx.locals->find_own(name)) ctx.locals->set_own(name, Value());
                break;
            }
            case 0x3A: {
                const std::string name = to_string(pop());
                const Value obj = pop();
                st.emplace_back(obj.is_object() ? obj.o->remove_own(name) : false);
                break;
            }
            case 0x3B: {
                const std::string name = to_string(pop());
                bool done = ctx.locals && ctx.locals->remove_own(name);
                for (auto it = ctx.scope.rbegin(); !done && it != ctx.scope.rend(); ++it) done = *it && (*it)->remove_own(name);
                if (!done) done = global->remove_own(name);
                st.emplace_back(done);
                break;
            }
            case 0x4E: {
                const Value name = pop();
                const Value obj = pop();
                st.push_back(get_member(obj, to_string(name)));
                break;
            }
            case 0x4F: {
                const Value v = pop();
                const Value name = pop();
                const Value obj = pop();
                set_member(obj, to_string(name), v);
                break;
            }
            case 0x22: {
                const double index = to_number(pop());
                const ObjectPtr t = target_from(ctx, pop());
                st.push_back(t && index >= 0 && index < 22 ? get_member(Value(t), kPropertyNames[int(index)]) : Value());
                break;
            }
            case 0x23: {
                const Value v = pop();
                const double index = to_number(pop());
                const ObjectPtr t = target_from(ctx, pop());
                if (t && index >= 0 && index < 22) set_member(Value(t), kPropertyNames[int(index)], v);
                break;
            }
            case 0x45: {
                const Value v = pop();
                Instance* i = v.instance();
                st.push_back(i ? Value(i->target_path()) : Value());
                break;
            }
            case 0x46:
            case 0x55: {
                Value obj = op == 0x46 ? get_variable(ctx, to_string(pop())) : pop();
                st.push_back(Value::null());
                if (obj.is_object()) {
                    std::vector<std::string> names;
                    int n = 0;
                    for (Object* o = obj.o.get(); o && n < 64; o = o->proto.get(), ++n) {
                        o->special_names(names);
                        for (const std::string& k : o->own_names()) {
                            const Property* p = o->find_own(k);
                            if (p && !(p->flags & 1) && k != "__proto__" && k != "__constructor__") names.push_back(k);
                        }
                    }
                    for (const std::string& k : names) st.emplace_back(k);
                }
                break;
            }
            case 0x43: {
                const double n = to_number(pop());
                auto obj = new_object();
                for (int i = 0; i < int(n) && st.size() >= 2; ++i) {
                    const Value v = pop();
                    set_member(Value(obj), to_string(pop()), v);
                }
                st.emplace_back(obj);
                break;
            }
            case 0x42: {
                const double n = to_number(pop());
                auto arr = new_array();
                for (int i = 0; i < int(n) && !st.empty(); ++i) arr->items.push_back(pop());
                st.emplace_back(ObjectPtr(arr));
                break;
            }

            // --- functions and classes
            case 0x9B:
            case 0x8E: {
                size_t p = data;
                auto f = std::make_shared<ScriptFunction>(function_proto);
                f->name = read_cstr(c, p, dend);
                const uint16_t np = read_le<uint16_t>(c, p);
                p += 2;
                if (op == 0x8E) {
                    f->v2 = true;
                    f->registers = c[p];
                    f->flags = uint16_t(c[p + 1] | c[p + 2] << 8);
                    p += 3;
                    for (uint16_t i = 0; i < np; ++i) {
                        const uint8_t r = c[p++];
                        f->params.emplace_back(r, read_cstr(c, p, dend));
                    }
                } else {
                    for (uint16_t i = 0; i < np; ++i) f->params.emplace_back(0, read_cstr(c, p, dend));
                }
                const uint16_t size = read_le<uint16_t>(c, p);
                f->code = ctx.hold;
                f->start = ctx.pc;
                f->length = size;
                f->pool = ctx.pool;
                f->scope = ctx.scope;
                for (const auto& w : ctx.with)
                    if (w.second) f->scope.push_back(w.second);
                if (ctx.locals) f->scope.push_back(ctx.locals);
                f->target = ctx.target;
                function_with_prototype(f);
                ctx.pc += size;
                if (f->name.empty()) {
                    st.emplace_back(ObjectPtr(f));
                } else if (ctx.locals) {
                    ctx.locals->set_own(f->name, Value(ObjectPtr(f)));
                } else if (ctx.target) {
                    set_member(Value(ctx.target), f->name, Value(ObjectPtr(f)));
                }
                break;
            }
            case 0x3D: {
                const std::string name = to_string(pop());
                pop_args(args);
                if (name == "super") {
                    const Value s = get_variable(ctx, "super");
                    auto* so = dynamic_cast<SuperObject*>(s.object());
                    st.push_back(so && so->ctor ? call(Value(so->ctor), so->self, args, so->from) : Value());
                } else {
                    ObjectPtr holder;
                    const Value fn = get_variable(ctx, name, &holder);
                    st.push_back(call(fn, holder ? Value(holder) : Value(), args));
                }
                break;
            }
            case 0x52: {
                const Value name = pop();
                const Value obj = pop();
                pop_args(args);
                const std::string mname = name.nullish() ? std::string() : to_string(name);
                auto* so = dynamic_cast<SuperObject*>(obj.object());
                if (mname.empty()) {
                    if (so) st.push_back(so->ctor ? call(Value(so->ctor), so->self, args, so->from) : Value());
                    else st.push_back(call(obj, Value(), args));
                } else if (so) {
                    Value fn;
                    ObjectPtr owner;
                    if (so->from && lookup(so->from.get(), mname, fn, so->self, &owner)) st.push_back(call(fn, so->self, args, owner));
                    else st.emplace_back();
                } else if (obj.is_object()) {
                    Value fn;
                    ObjectPtr owner;
                    if (lookup(obj.o.get(), mname, fn, obj, &owner)) st.push_back(call(fn, obj, args, owner));
                    else st.emplace_back();
                } else {
                    st.push_back(call(get_member(obj, mname), obj, args));
                }
                break;
            }
            case 0x40: {
                const std::string name = to_string(pop());
                pop_args(args);
                st.push_back(construct(get_variable(ctx, name), args));
                break;
            }
            case 0x53: {
                const Value name = pop();
                const Value obj = pop();
                pop_args(args);
                const std::string mname = name.nullish() ? std::string() : to_string(name);
                st.push_back(construct(mname.empty() ? obj : get_member(obj, mname), args));
                break;
            }
            case 0x54: {
                const Value ctor = pop(), obj = pop();
                st.emplace_back(instance_of(obj, ctor));
                break;
            }
            case 0x2B: {
                const Value obj = pop(), ctor = pop();
                st.push_back(instance_of(obj, ctor) ? obj : Value::null());
                break;
            }
            case 0x69: {
                const Value super_ctor = pop(), sub = pop();
                if (sub.is_object() && super_ctor.is_object()) {
                    const Value sp = get_member(super_ctor, "prototype");
                    auto proto = std::make_shared<Object>(sp.is_object() ? sp.o : object_proto);
                    proto->set_own("__constructor__", super_ctor, 1);
                    proto->set_own("constructor", sub, 1);
                    sub.o->set_own("prototype", Value(ObjectPtr(proto)), 1);
                }
                break;
            }
            case 0x2C: {
                const Value ctor = pop();
                const double n = to_number(pop());
                const Value proto = get_member(ctor, "prototype");
                for (int i = 0; i < int(n) && !st.empty(); ++i) {
                    const Value iface = get_member(pop(), "prototype");
                    if (proto.is_object() && iface.is_object()) proto.o->interfaces.push_back(iface.o);
                }
                break;
            }
            default:
                if (trace) trace("swf: unhandled action 0x" + [&] {
                    char b[8];
                    std::snprintf(b, sizeof b, "%02x", op);
                    return std::string(b);
                }());
                break;
        }
    }
    return result;
}

}  // namespace tdkr::swf
