#include "assets/level.h"

#include <algorithm>
#include <functional>
#include <regex>
#include <type_traits>

namespace tdkr {

namespace {

#include "assets/level_specs.inc"

// CComponentBase::Load values: c enabled, id, position, rotation, scale, c visible, c always
// visible, c always update (CGameObject::InitComponentBase). The id is field 1.
constexpr size_t kBaseFirst = 1;

// Shown when the level starts: enabled and visible (scripts turn on the rest, e.g. the
// cutscene rigs, which carry their own Batman).
Transform base_transform(const Value& v) {
    Transform t = transform_from_values(v, kBaseFirst);
    t.visible = v[0].i != 0 && v[kBaseFirst + 10].i != 0;
    return t;
}

template <typename K, typename V, size_t N>
auto table(const std::pair<K, V> (&rows)[N]) {
    using Key = std::conditional_t<std::is_same_v<K, const char*>, std::string, K>;
    std::unordered_map<Key, std::string> m;
    for (const auto& [k, v] : rows) m.emplace(k, v);
    return m;
}

const auto& component_loads() { static auto t = table(kComponentLoads); return t; }
const auto& template_loads() { static auto t = table(kTemplateLoads); return t; }
const auto& loader_specs() { static auto t = table(kLoaderSpecs); return t; }

// Inline nested <Name::Load> references.
std::string expand(const std::string& spec, int depth = 0) {
    if (depth > 16) throw FormatError("spec nesting too deep");
    static const std::regex nested("<([^>]+)>");
    std::string out;
    auto begin = std::sregex_iterator(spec.begin(), spec.end(), nested);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out.append(spec, last, it->position() - last);
        auto ls = loader_specs().find((*it)[1].str());
        if (ls == loader_specs().end()) throw FormatError("no spec for " + (*it)[1].str());
        out += expand(ls->second, depth + 1);
        last = it->position() + it->length();
    }
    out.append(spec, last);
    return out;
}

const std::string& loader_spec(const std::string& loader) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(loader);
    if (it != cache.end()) return it->second;
    return cache.emplace(loader, expand("<" + loader + ">")).first->second;
}

// What a flag-0 component stores in a level: the factory class's Load, else the template
// reader (GenerateComponentTemplate), else nothing.
const std::string& component_spec(int comp) {
    static const std::string none;
    if (auto it = component_loads().find(comp); it != component_loads().end()) return loader_spec(it->second);
    if (auto it = template_loads().find(comp); it != template_loads().end()) return loader_spec(it->second);
    return none;
}

const std::string& template_spec(int comp) {
    static const std::string none;
    auto it = template_loads().find(comp);
    return it == template_loads().end() ? none : loader_spec(it->second);
}

void read_mesh_values(const Value& v, LevelObject& o, size_t first = 0) {
    o.model = v[first].s;
    o.mesh_flags = 0;
}

std::string strip(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

const std::unordered_map<int, const LevelTypeSpec*>& level_types() {
    static const auto t = [] {
        std::unordered_map<int, const LevelTypeSpec*> m;
        for (const auto& lt : kLevelTypes) m.emplace(lt.type, &lt);
        return m;
    }();
    return t;
}

void read_level_type(DictStream& st, const LevelTypeSpec& lt, LevelObject& o) {
    o.kind = lt.kind;
    const std::string spec = expand(lt.spec);
    o.fields = st.spec(spec);
    // Most structure records (spawn points, irradiance volumes, occluders, bake groups...)
    // start with the base component's fields.
    if (spec.rfind(loader_spec(kBaseLoader), 0) == 0) o.base = base_transform(o.fields);
    if (o.kind == "skybox") {
        o.model = o.fields[0].s;
    } else if (o.kind == "long_distance_mesh" || o.kind == "reflection_mesh") {
        read_mesh_values(o.fields, o, 14);     // after the 14 base values
    } else if (o.kind == "zone") {
        // The zone's byte size: CLevel skips that much for a mission zone that is not current.
        const int32_t size = o.fields[o.fields.size() - 1].i;
        o.content_end = st.pos + size_t(std::max(size, 0));
    }
}

void read_game_object(DictStream& st, const GolType& type, LevelObject& o) {
    for (const auto& [comp, flag] : type.components) {
        if (comp == kCompGroupHash) {
            o.components[comp] = st.spec(kGroupBuiltinSpec);
        } else if (flag != 0) {
            continue;
        } else if (comp == kCompBaseHash) {
            o.base = base_transform(st.spec(loader_spec(kBaseLoader)));
        } else if (comp == kCompMeshHash) {
            read_mesh_values(st.spec(loader_spec(kMeshLoader)), o);
        } else if (const std::string& spec = component_spec(comp); !spec.empty()) {
            o.components[comp] = st.spec(spec);
        }
    }
}

}  // namespace

Transform transform_from_values(const Value& v, size_t first) {
    Transform t;
    t.id = v[first].i;
    t.position = {v[first + 1].as_float(), v[first + 2].as_float(), v[first + 3].as_float()};
    t.rotation = {v[first + 4].as_float(), v[first + 5].as_float(), v[first + 6].as_float()};
    t.scale = {v[first + 7].as_float(), v[first + 8].as_float(), v[first + 9].as_float()};
    if (v.size() > first + 10) t.visible = v[first + 10].i != 0;
    return t;
}

Gol load_gol(const Bytes& data) {
    DictStream st(data);
    if (st.h() != 0x474F || st.h() != 3) throw FormatError("not a version-3 GO library");
    const int n = st.i();
    std::vector<size_t> offsets(n);
    for (auto& off : offsets) off = size_t(st.i());
    std::vector<size_t> sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    Gol gol;
    for (size_t off : offsets) {
        st.pos = off;
        GolType t;
        t.id = st.i();
        const int np = st.i();
        for (int k = 0; k < np; ++k) t.parents.push_back(st.i());
        const int nc = st.h();
        t.name = strip(st.s());
        for (int k = 0; k < nc; ++k) {
            const int comp = st.i();
            const int flag = st.h();
            t.components.emplace_back(comp, flag);
        }
        // Templates follow in listed order; each entry must end where the next begins.
        auto next = std::upper_bound(sorted.begin(), sorted.end(), off);
        const size_t entry_end = next == sorted.end() ? st.end : *next;
        try {
            for (const auto& [comp, flag] : t.components) t.templates[comp] = st.spec(template_spec(comp));
            t.templates_ok = st.pos == entry_end;
        } catch (const FormatError&) {
            t.templates_ok = false;
        }
        if (!t.templates_ok) t.templates.clear();   // 10 level-structure entries end a few bytes short
        gol[t.id] = std::move(t);
    }
    return gol;
}

Level load_level(const Bytes& data, const Gol& gol) {
    DictStream st(data);
    if (st.h() != 0x4E56) throw FormatError("missing 'NV' tag");
    if (st.h() != 3) throw FormatError("unsupported level version");
    Level level;
    level.header_count = st.i();   // an editor figure: LoadNextObject reads until -1
    level.strings = st.strings();
    bool terminated = false;
    while (st.pos + 4 <= st.end) {
        LevelObject o;
        o.offset = st.pos;
        o.type = st.i();
        if (o.type == -1) {
            terminated = true;
            break;
        }
        try {
            if (auto lt = level_types().find(o.type); lt != level_types().end()) {
                read_level_type(st, *lt->second, o);
            } else if (auto g = gol.find(o.type); g != gol.end()) {
                o.kind = "object";
                o.name = g->second.name;
                read_game_object(st, g->second, o);
            } else {
                throw FormatError("unknown object type " + std::to_string(o.type));
            }
        } catch (const FormatError& ex) {
            throw FormatError("object at " + std::to_string(o.offset) + " (type " + std::to_string(o.type) + "): " + ex.what());
        }
        level.objects.push_back(std::move(o));
    }
    if (!terminated) throw FormatError("no end marker");
    if (st.pos != st.end) throw FormatError(std::to_string(st.end - st.pos) + " bytes left after the last object");
    return level;
}

std::vector<std::pair<int, int>> resolved_components(const GolType& type, const Gol& gol) {
    std::map<int, int> comps;
    std::function<void(const GolType&, int)> add = [&](const GolType& t, int depth) {
        if (depth > 16) return;
        for (int p : t.parents) {
            if (auto it = gol.find(p); it != gol.end()) add(it->second, depth + 1);
        }
        for (const auto& [c, f] : t.components) comps[c] = f;
    };
    add(type, 0);
    return {comps.begin(), comps.end()};
}

const Value* resolved_template(const GolType& type, const Gol& gol, int comp) {
    if (auto it = type.templates.find(comp); it != type.templates.end()) return &it->second;
    for (int p : type.parents) {
        if (auto it = gol.find(p); it != gol.end() && it->second.id != type.id) {
            if (const Value* v = resolved_template(it->second, gol, comp)) return v;
        }
    }
    return nullptr;
}

namespace {
bool is_helper_model(const std::string& model) {
    // Editor stand-ins: N.O.V.A. 3's helper boxes and placeholders, TDKR's editor_* icons
    // and test collision boxes.
    const std::string m = to_lower(model);
    for (const char* h : {"helpers_box", "staticmeshplaceholder", "editor_", "meshplaceholder", "box_collision"})
        if (m.find(h) != std::string::npos) return true;
    return false;
}

std::optional<Transform> object_transform(const LevelObject& o, const GolType& type, const Gol& gol) {
    if (o.base) return o.base;
    if (const Value* t = resolved_template(type, gol, kCompBase)) return base_transform(*t);
    return std::nullopt;
}
}  // namespace

std::vector<Placement> model_placements(const Level& level, const Gol& gol, bool library) {
    std::vector<Placement> out;
    for (size_t index = 0; index < level.objects.size(); ++index) {
        const LevelObject& o = level.objects[index];
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        std::string model = o.model;
        uint32_t mesh_flags = o.mesh_flags;
        if (model.empty() && library) {
            if (const Value* t = resolved_template(g->second, gol, kCompMesh)) model = (*t)[0].s;
        }
        auto tf = library ? object_transform(o, g->second, gol) : o.base;
        if (model.empty() || !tf || is_helper_model(model)) continue;
        Placement p;
        p.object_index = index;
        p.model = model;
        p.mesh_flags = mesh_flags;
        p.transform = *tf;
        p.object = o.name;
        const Value* anim = nullptr;
        if (auto it = o.components.find(kCompAnimation); it != o.components.end()) anim = &it->second;
        else if (library) anim = resolved_template(g->second, gol, kCompAnimation);
        if (anim && anim->size() >= 3 && (*anim)[0].type == Value::String) {
            p.anim_file = (*anim)[0].s;
            p.anim_clip = (*anim)[1].s;
            p.anim_speed = (*anim)[2].as_float();
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Placement> collision_placements(const Level& level, const Gol& gol) {
    std::vector<Placement> out;
    for (size_t index = 0; index < level.objects.size(); ++index) {
        const LevelObject& o = level.objects[index];
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        const Value* data = nullptr;
        if (auto it = o.components.find(kCompCollision); it != o.components.end()) data = &it->second;
        else data = resolved_template(g->second, gol, kCompCollision);
        auto tf = object_transform(o, g->second, gol);
        if (data && tf && data->size() > kCollisionModelField && (*data)[kCollisionModelField].type == Value::String &&
            !(*data)[kCollisionModelField].s.empty()) {
            Placement p;
            p.object_index = index;
            p.model = (*data)[kCollisionModelField].s;
            p.transform = *tf;
            p.object = o.name;
            out.push_back(std::move(p));
        }
    }
    return out;
}

std::vector<SkyboxRef> level_skyboxes(const Level& level) {
    std::vector<SkyboxRef> out;
    for (const auto& o : level.objects) {
        if (o.kind != "skybox" || o.fields.size() < 4) continue;
        out.push_back({o.fields[0].s, o.fields[3].i, true});
    }
    return out;
}

std::vector<LevelLight> level_lights(const Level& level, const Gol&) {
    std::vector<LevelLight> out;
    for (size_t index = 0; index < level.objects.size(); ++index) {
        const LevelObject& o = level.objects[index];
        if (o.kind != "light" || !o.base || o.fields.size() < 19) continue;
        // After the 14 base values: f, f, i, i, f (CComponentLight::Load). Not yet checked
        // against a level that uses them; none of the shipped levels has a light record.
        LevelLight l;
        l.object = "light";
        l.index = index;
        l.position = o.base->position;
        const uint32_t c = uint32_t(o.fields[16].i);
        l.color = glm::vec4((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24) / 255.0f;
        l.intensity = o.fields[14].as_float();
        l.radius = o.fields[15].as_float();
        out.push_back(l);
    }
    return out;
}

void keep_section(Level&, uint32_t) {}

}  // namespace tdkr
