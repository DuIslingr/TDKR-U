#include "script/lua_runtime.h"

#include <algorithm>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace tdkr {

namespace {

int lua_print(lua_State* L) {
    auto* rt = static_cast<ScriptRuntime*>(lua_touserdata(L, lua_upvalueindex(1)));
    std::string line;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (const char* s = lua_tostring(L, -1)) line += (i > 1 ? "\t" : "") + std::string(s);
        lua_pop(L, 1);
    }
    rt->log("print: " + line);
    return 0;
}

// The globals a checkpoint leaves alone: Lua's own and the per-coroutine ones.
bool kept_out(const std::string& name) {
    static const char* const kNames[] = {"_G", "_VERSION", "package", "string", "table", "math", "os", "io",
                                         "coroutine", "debug", "this", "CurrentActor"};
    return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

bool save_value(lua_State* L, int index, ScriptRuntime::SavedValue& out, std::vector<const void*>& seen) {
    using T = ScriptRuntime::SavedValue::Type;
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: out.type = T::Bool, out.b = lua_toboolean(L, index); return true;
    case LUA_TNUMBER: out.type = T::Number, out.n = lua_tonumber(L, index); return true;
    case LUA_TSTRING: out.type = T::String, out.s = lua_tostring(L, index); return true;
    case LUA_TTABLE: {
        const void* p = lua_topointer(L, index);
        if (std::find(seen.begin(), seen.end(), p) != seen.end() || seen.size() >= 16) return false;   // cycles, depth
        seen.push_back(p);
        out.type = T::Table;
        const int t = index < 0 ? lua_gettop(L) + index + 1 : index;
        lua_pushnil(L);
        while (lua_next(L, t)) {
            ScriptRuntime::SavedValue k, v;
            const int kt = lua_type(L, -2);
            if ((kt == LUA_TNUMBER || kt == LUA_TSTRING || kt == LUA_TBOOLEAN) && save_value(L, -2, k, seen) &&
                save_value(L, -1, v, seen))
                out.table.push_back(std::move(k)), out.table.push_back(std::move(v));
            lua_pop(L, 1);
        }
        seen.pop_back();
        return true;
    }
    default: return false;   // functions, userdata, coroutines
    }
}

void push_value(lua_State* L, const ScriptRuntime::SavedValue& v);

// The table at the top of the stack gets the saved table's entries; nested tables merge too.
void merge_table(lua_State* L, const ScriptRuntime::SavedValue& v) {
    for (size_t i = 0; i + 1 < v.table.size(); i += 2) {
        push_value(L, v.table[i]);
        if (v.table[i + 1].type == ScriptRuntime::SavedValue::Type::Table) {
            lua_pushvalue(L, -1);
            lua_rawget(L, -3);
            if (lua_istable(L, -1)) {
                merge_table(L, v.table[i + 1]);
                lua_pop(L, 2);
                continue;
            }
            lua_pop(L, 1);
        }
        push_value(L, v.table[i + 1]);
        lua_rawset(L, -3);
    }
}

void push_value(lua_State* L, const ScriptRuntime::SavedValue& v) {
    using T = ScriptRuntime::SavedValue::Type;
    switch (v.type) {
    case T::Bool: lua_pushboolean(L, v.b); break;
    case T::Number: lua_pushnumber(L, v.n); break;
    case T::String: lua_pushlstring(L, v.s.data(), v.s.size()); break;
    case T::Table: lua_newtable(L), merge_table(L, v); break;
    default: lua_pushnil(L);
    }
}

}  // namespace

ScriptRuntime::Globals ScriptRuntime::save_globals() const {
    Globals out;
    std::vector<const void*> seen;
    lua_pushnil(L_);
    while (lua_next(L_, LUA_GLOBALSINDEX)) {
        if (lua_type(L_, -2) == LUA_TSTRING) {
            const std::string name = lua_tostring(L_, -2);
            SavedValue v;
            if (!kept_out(name) && save_value(L_, -1, v, seen)) out.emplace_back(name, std::move(v));
        }
        lua_pop(L_, 1);
    }
    return out;
}

void ScriptRuntime::restore_globals(const Globals& globals) {
    // A table of functions (a module the chunks define) keeps them; a table of data is replaced.
    auto has_functions = [this] {
        bool any = false;
        lua_pushnil(L_);
        while (lua_next(L_, -2)) {
            any = any || lua_isfunction(L_, -1);
            lua_pop(L_, 1);
        }
        return any;
    };
    for (const auto& [name, v] : globals) {
        lua_getglobal(L_, name.c_str());
        if (v.type == SavedValue::Type::Table && lua_istable(L_, -1) && has_functions()) {
            merge_table(L_, v);
            lua_pop(L_, 1);
            continue;
        }
        lua_pop(L_, 1);
        push_value(L_, v);
        lua_setglobal(L_, name.c_str());
    }
}

ScriptRuntime::ScriptRuntime() {
    threads_.resize(kMaxThreads);
    open();
}

ScriptRuntime::~ScriptRuntime() { lua_close(L_); }

void ScriptRuntime::open() {
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, lua_print, 1);
    lua_setglobal(L_, "print");
}

void ScriptRuntime::register_function(const std::string& name, Function fn, void* context) {
    lua_pushlightuserdata(L_, context);
    lua_pushcclosure(L_, fn, 1);
    lua_setglobal(L_, name.c_str());
}

void ScriptRuntime::reset() {
    stop_all();
    lua_close(L_);
    open();
}

bool ScriptRuntime::load(const std::string& source, const std::string& chunk_name) {
    if (luaL_loadbuffer(L_, source.data(), source.size(), chunk_name.c_str()) != 0 || lua_pcall(L_, 0, 0, 0) != 0) {
        log(std::string("error in ") + chunk_name + ": " + (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool ScriptRuntime::has_function(const std::string& name) const {
    lua_getglobal(L_, name.c_str());
    const bool ok = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return ok;
}

bool ScriptRuntime::running(const std::string& function) const {
    for (const Thread& t : threads_)
        if (t.state != Thread::State::Free && t.function == function) return true;
    return false;
}

bool ScriptRuntime::call(const std::string& function, int arg, int owner) {
    lua_getglobal(L_, function.c_str());
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    lua_pushinteger(L_, owner);
    lua_setglobal(L_, "this");
    lua_pushinteger(L_, arg);
    if (lua_pcall(L_, 1, 0, 0) != 0) {
        log("error in " + function + ": " + (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

size_t ScriptRuntime::thread_count() const {
    size_t n = 0;
    for (const Thread& t : threads_) n += t.state != Thread::State::Free;
    return n;
}

bool ScriptRuntime::start(const std::string& function, int owner) {
    if (!has_function(function) || running(function)) return false;
    for (Thread& t : threads_) {
        if (t.state != Thread::State::Free) continue;
        t.L = lua_newthread(L_);
        t.ref = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_getglobal(t.L, function.c_str());
        t.function = function;
        t.owner = owner;
        t.state = Thread::State::Ready;
        t.started = false;
        t.event.clear();
        t.event_ids.clear();
        return true;
    }
    log("too many script threads; not starting " + function);
    return false;
}

void ScriptRuntime::finish(Thread& t) {
    luaL_unref(L_, LUA_REGISTRYINDEX, t.ref);
    t = Thread{};
}

void ScriptRuntime::resume(Thread& t) {
    lua_pushinteger(t.L, t.owner);
    lua_setglobal(t.L, "this");
    lua_pushinteger(t.L, t.owner);   // TDKR's name for it (LuaThread::Resume)
    lua_setglobal(t.L, "CurrentActor");
    running_ = t.L;
    const int status = lua_resume(t.L, 0);
    running_ = nullptr;
    t.started = true;
    if (status == LUA_YIELD) {
        // Yielded values: a code, then its arguments (see the header).
        const int n = lua_gettop(t.L);
        const int code = n >= 1 ? int(lua_tointeger(t.L, 1)) : 0;
        if (code == 2) {
            t.state = Thread::State::Sleeping;
            t.sleep_ms = n >= 2 ? float(lua_tonumber(t.L, 2)) : 0.0f;
        } else if (code == 4) {
            t.state = Thread::State::WaitingEvent;
            t.event = n >= 2 && lua_isstring(t.L, 2) ? lua_tostring(t.L, 2) : "";
            t.event_ids.clear();
            for (int k = 3; k <= n; ++k)
                if (const int id = int(lua_tointeger(t.L, k)); id >= 0) t.event_ids.push_back(id);
        } else if (code == 3) {
            t.state = Thread::State::Sleeping;   // teleport-in effect
            t.sleep_ms = 500;
        } else {
            t.state = Thread::State::Sleeping;   // plain coroutine.yield: next update
            t.sleep_ms = 0;
        }
        lua_settop(t.L, 0);
        return;
    }
    if (status != 0) log("error in " + t.function + ": " + (lua_tostring(t.L, -1) ? lua_tostring(t.L, -1) : "?"));
    finish(t);
}

void ScriptRuntime::update(float dt_ms) {
    // Resume in slot order; coroutines started during this update wait for the next one.
    std::vector<size_t> ready;
    for (size_t i = 0; i < threads_.size(); ++i) {
        Thread& t = threads_[i];
        if (t.state == Thread::State::Sleeping) {
            t.sleep_ms -= dt_ms;
            if (t.sleep_ms <= 0) t.state = Thread::State::Ready;
        }
        if (t.state == Thread::State::Ready) ready.push_back(i);
    }
    for (size_t i : ready)
        if (threads_[i].state == Thread::State::Ready) resume(threads_[i]);
}

void ScriptRuntime::send_event(const std::string& event, int id) {
    for (Thread& t : threads_) {
        if (t.state != Thread::State::WaitingEvent || t.event != event) continue;
        if (!t.event_ids.empty() && id >= 0) {
            auto it = std::find(t.event_ids.begin(), t.event_ids.end(), id);
            if (it == t.event_ids.end()) continue;
            t.event_ids.erase(it);
            if (!t.event_ids.empty()) continue;
        }
        t.state = Thread::State::Ready;
    }
}

void ScriptRuntime::stop_all() {
    for (Thread& t : threads_)
        if (t.state != Thread::State::Free) finish(t);
}

void ScriptRuntime::stop_owner(int owner) {
    for (Thread& t : threads_)
        if (t.state != Thread::State::Free && t.owner == owner && t.L != running_) finish(t);
}

}  // namespace tdkr
