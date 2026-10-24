// ActionScript 2 for TDKR's Flash UI: values, objects with prototype chains, script and native
// functions and the bytecode interpreter (the opcodes and built-in classes the game's 104
// movies use; see tools/swf.py actions). Movie clips are objects too (swf/player.h).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "swf/movie.h"

namespace tdkr::swf {

class Object;
class Function;
class Instance;
class VM;
using ObjectPtr = std::shared_ptr<Object>;

struct Value {
    enum Type : uint8_t { Undefined, Null, Boolean, Number, String, Obj } type = Undefined;
    bool b = false;
    double n = 0;
    std::string s;
    ObjectPtr o;

    Value() = default;
    Value(bool v) : type(Boolean), b(v) {}
    Value(double v) : type(Number), n(v) {}
    Value(int v) : type(Number), n(v) {}
    Value(std::string v) : type(String), s(std::move(v)) {}
    Value(const char* v) : type(String), s(v) {}
    Value(ObjectPtr v) : type(v ? Obj : Null), o(std::move(v)) {}
    static Value null() {
        Value v;
        v.type = Null;
        return v;
    }
    bool undefined() const { return type == Undefined; }
    bool is_null() const { return type == Null; }
    bool nullish() const { return type == Undefined || type == Null; }
    bool is_object() const { return type == Obj && o; }
    Object* object() const { return type == Obj ? o.get() : nullptr; }
    Function* function() const;
    Instance* instance() const;
};

struct Property {
    Value value;
    std::shared_ptr<Function> getter, setter;
    uint8_t flags = 0;          // ASSetPropFlags: 1 don't enumerate, 2 don't delete, 4 read only
};

class Object : public std::enable_shared_from_this<Object> {
public:
    explicit Object(ObjectPtr proto = nullptr) : proto(std::move(proto)) {}
    virtual ~Object() = default;

    ObjectPtr proto;                                 // __proto__
    std::vector<ObjectPtr> interfaces;               // ImplementsOp (on a prototype)

    // Own properties (insertion order kept for enumeration).
    Property* find_own(const std::string& name);
    const Property* find_own(const std::string& name) const;
    Property& own(const std::string& name);         // created if missing
    void set_own(const std::string& name, Value v, uint8_t flags = 0);
    bool remove_own(const std::string& name);
    const std::vector<std::string>& own_names() const { return order_; }

    // Built-in members with behaviour (movie clip properties, array length): read/write
    // before the ordinary properties. Return false when `name` is not one.
    virtual bool get_special(VM&, const std::string&, Value&) { return false; }
    virtual bool set_special(VM&, const std::string&, const Value&) { return false; }
    virtual void special_names(std::vector<std::string>&) const {}

    virtual Function* as_function() { return nullptr; }
    virtual Instance* as_instance() { return nullptr; }
    virtual class ArrayObject* as_array() { return nullptr; }
    virtual const char* type_of() const { return "object"; }
    // The primitive this object stands for (Number / String / Boolean wrappers), else undefined.
    Value primitive;

private:
    std::unordered_map<std::string, Property> props_;
    std::vector<std::string> order_;
};

class ArrayObject : public Object {
public:
    using Object::Object;
    std::vector<Value> items;
    ArrayObject* as_array() override { return this; }
    bool get_special(VM& vm, const std::string& name, Value& out) override;
    bool set_special(VM& vm, const std::string& name, const Value& v) override;
    void special_names(std::vector<std::string>& out) const override;
};

struct CallArgs {
    Value self;                                      // `this`
    std::vector<Value> args;
    ObjectPtr home;                                  // where the method was found (for super)
    const Value& operator[](size_t i) const {
        static const Value none;
        return i < args.size() ? args[i] : none;
    }
    size_t size() const { return args.size(); }
};

class Function : public Object {
public:
    using Object::Object;
    Function* as_function() override { return this; }
    const char* type_of() const override { return "function"; }
    virtual Value call(VM& vm, CallArgs& a) = 0;
    // `new`: native classes build their own object; null: make one from `prototype`.
    virtual ObjectPtr construct(VM&, CallArgs&) { return nullptr; }
};

class NativeFunction : public Function {
public:
    using Fn = std::function<Value(VM&, CallArgs&)>;
    using Ctor = std::function<ObjectPtr(VM&, CallArgs&)>;
    NativeFunction(ObjectPtr proto, Fn fn, Ctor ctor = nullptr) : Function(std::move(proto)), fn_(std::move(fn)), ctor_(std::move(ctor)) {}
    Value call(VM& vm, CallArgs& a) override { return fn_ ? fn_(vm, a) : Value(); }
    ObjectPtr construct(VM& vm, CallArgs& a) override { return ctor_ ? ctor_(vm, a) : nullptr; }

private:
    Fn fn_;
    Ctor ctor_;
};

class ScriptFunction : public Function {
public:
    using Function::Function;
    ActionBuffer code;
    size_t start = 0, length = 0;
    bool v2 = false;                                  // DefineFunction2
    uint8_t registers = 4;
    uint16_t flags = 0;
    std::vector<std::pair<uint8_t, std::string>> params;   // (register or 0, name)
    std::vector<ObjectPtr> scope;                     // outermost first
    std::shared_ptr<const std::vector<std::string>> pool;
    ObjectPtr target;                                 // the timeline it was defined on
    std::string name;
    Value call(VM& vm, CallArgs& a) override;
};

// `super` inside a method: the prototype above the method's home and the superclass.
class SuperObject : public Object {
public:
    SuperObject(Value self, ObjectPtr from, ObjectPtr ctor) : self(std::move(self)), from(std::move(from)), ctor(std::move(ctor)) {}
    Value self;
    ObjectPtr from;     // lookups start here (superclass prototype)
    ObjectPtr ctor;     // superclass constructor
};

class VM {
public:
    VM();
    ~VM();

    ObjectPtr global;
    ObjectPtr object_proto, function_proto, array_proto, string_proto, number_proto, boolean_proto,
        movieclip_proto, textfield_proto, textformat_proto;
    std::unordered_map<std::string, ObjectPtr> registered;   // Object.registerClass: linkage -> constructor
    std::function<ObjectPtr()> root;                         // _root / _level0
    std::function<void(const std::string&)> trace;          // trace() output
    std::function<void(const std::string& command, const std::string& args)> fscommand;
    std::function<std::string(const std::string& id)> get_string;   // localisation
    std::function<bool(int key)> key_down;                   // Key.isDown
    std::function<void(const std::string& linkage)> play_sound;
    double time_ms = 0;                                      // getTimer()
    int last_key = 0;
    int version = 8;

    // setInterval / setTimeout
    struct Timer {
        int id = 0;
        Value fn, obj;
        std::string method;
        std::vector<Value> args;
        double period = 0, next = 0;
        bool once = false, dead = false;
    };
    std::vector<Timer> timers;
    int next_timer = 0;
    void update_timers(double now_ms);

    // Run an action buffer on a timeline (`target` is the clip, `this` too).
    void run_actions(const ActionBuffer& code, const ObjectPtr& target, size_t start = 0, size_t length = size_t(-1));

    // Members and calls.
    Value get_member(const Value& obj, const std::string& name);
    void set_member(const Value& obj, const std::string& name, const Value& v);
    bool has_member(const Value& obj, const std::string& name);
    // Find `name` from `obj` up its prototypes; `owner`: the object that has it.
    bool lookup(Object* obj, const std::string& name, Value& out, const Value& self, ObjectPtr* owner = nullptr);
    Value call(const Value& fn, const Value& self, std::vector<Value> args, ObjectPtr home = nullptr);
    Value call_method(const Value& obj, const std::string& name, std::vector<Value> args);
    Value construct(const Value& ctor, std::vector<Value> args);

    // Conversions (Flash 7+ rules).
    double to_number(const Value& v);
    std::string to_string(const Value& v);
    static bool to_boolean(const Value& v);
    Value to_primitive(const Value& v, bool prefer_string = false);
    static std::string number_to_string(double n);
    bool equals(const Value& a, const Value& b);
    static bool strict_equals(const Value& a, const Value& b);
    bool instance_of(const Value& v, const Value& ctor);

    // Builders.
    ObjectPtr new_object();
    std::shared_ptr<ArrayObject> new_array(std::vector<Value> items = {});
    std::shared_ptr<NativeFunction> native(NativeFunction::Fn fn, NativeFunction::Ctor ctor = nullptr);
    void method(const ObjectPtr& on, const std::string& name, NativeFunction::Fn fn);
    std::shared_ptr<Function> function_with_prototype(std::shared_ptr<Function> f);

    // The calling script's timeline (for natives like attachMovie on `this`).
    ObjectPtr current_target() const;
    int depth = 0;                                    // call depth (runaway guard)
    size_t steps = 0;                                 // instructions run (runaway guard)

    struct Context;
    Value execute(Context& ctx);

private:
    Value get_variable(Context& ctx, const std::string& name, ObjectPtr* holder = nullptr);
    void set_variable(Context& ctx, const std::string& name, const Value& v);
    Value resolve_path(Context& ctx, const std::string& path);
    ObjectPtr target_from(Context& ctx, const Value& v);
    void install_builtins();
    std::vector<Context*> stack_;
    friend class ScriptFunction;
};

struct VM::Context {
    const Bytes* code = nullptr;
    ActionBuffer hold;                                // keeps `code` alive
    size_t pc = 0, end = 0;
    std::shared_ptr<const std::vector<std::string>> pool;
    std::vector<Value> regs = std::vector<Value>(4);
    std::vector<ObjectPtr> scope;                     // outermost first (global is implicit)
    ObjectPtr locals;                                 // the activation (functions)
    Value self;
    ObjectPtr target;                                 // current timeline (SetTarget changes it)
    ObjectPtr original_target;
    ObjectPtr home;                                   // for super
    ObjectPtr arguments;
    std::vector<std::pair<size_t, ObjectPtr>> with;   // (end pc, object)
};

std::u32string code_points(const std::string& utf8);
std::string utf8_from_code_points(const std::u32string& s);

inline Function* Value::function() const { return type == Obj && o ? o->as_function() : nullptr; }
inline Instance* Value::instance() const { return type == Obj && o ? o->as_instance() : nullptr; }

}  // namespace tdkr::swf
