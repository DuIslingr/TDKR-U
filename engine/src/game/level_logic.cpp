#include "game/level_logic.h"
#include "game/checkpoint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <glm/gtc/matrix_transform.hpp>

#include "audio/audio.h"
#include "game/anim_sounds.h"
#include "game/combat.h"
#include "game/player.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompTrigger = 0x37F7A8AE;  // CTriggerComponent
constexpr int kCompRunScript = 0x1E;      // CRunScriptComponent (OnDead)
constexpr int kCompHelicopter = 0x786F6ECB;   // CHelicopterLogic
constexpr int kCompGroup = 0x2AD2A046;        // CGroupObject's builtin data: its members
constexpr int kCompInteractive = 0x34747EBE;   // CInteractionComponent: radius, ?, script (OnScriptEvent), button frame
constexpr int kCompDoor = 0x61;           // CDoorAIComponent
constexpr int kCompEffects = 0x92;        // CEffectsComponent: [[effect, node, autostart, ...], ...]
constexpr int kCompEffectBuiltIn = 0xAE;  // CEffectBuiltInComponent: effect, ?, node, autostart, ?
constexpr int kCompEffectTdkr = 0x2CA13C06;   // TDKR's CEffectComponent: effect ("None": none), ?, node, autostart, ...
constexpr int kCompBuiltinBox = 0x729EC572;        // CBoxNodeComponent: width, depth, height
constexpr int kCompBuiltinCylinder = 0x749126C3;   // CCylinderNodeComponent: radius, height
constexpr int kCompCinematic = 0x41;      // CCinematicComponent: length ms, ..., [11] script event tracks, [13] object clips
// TDKR's CCinematicComponent: [0] length ms, [1] ?, [2] speed, [3] ?, [4] [5] ?, [6] script tracks
// ([[[start, end, script], ...]]), [7] camera tracks ([[type, [[ms, end ms, position, target], ...]]],
// relative to the object), [8] object clips ([[object, [[clip, start, speed, loop, length]], ...]]).
constexpr int kCompCinematicTdkr = 0x17AC851F;
constexpr int kCompPullable = 0x4891830C;   // PullableObjectComponent: idle, struggle, fall, back clips, struggle ms, ...
constexpr int kCompNpcAi = 0x0113F791;     // CNpcAIComponent: behaviour, enemy, ...
constexpr int kCompDestroyable = 0x068D1134;   // CDestroyableComponent
constexpr int kCompPath = 0x15;            // CPathComponent: id, ?, first waypoint, last waypoint, ?
constexpr int kCompMotionOnPath = 0x16;    // CMotionOnPathComponent: node, path id, max speed, accel, brake, loop

// Every string in a value tree that holds script functions.
void collect_scripts(const Value& v, std::vector<std::string>& out) {
    if (v.type == Value::String) {
        if (v.s.find("function") != std::string::npos) out.push_back(v.s);
        return;
    }
    for (const Value& x : v.list) collect_scripts(x, out);
}

}  // namespace

LevelLogic::~LevelLogic() = default;

LevelLogic::LevelLogic(Scene& s, Player& p, const Gol& g) : scene(s), player(p), gol(g), anim_sounds_(std::make_unique<AnimSounds>(g)) {
    runtime_.log = [this](const std::string& line) { log(line); };   // errors stay in the log, as the game's
    register_game_api(*this);
}

LevelLogic::Object* LevelLogic::object(int id) {
    auto it = objects_.find(id);
    if (it == objects_.end()) return nullptr;
    if (!it->second.loaded) {
        if (trace && warned_.insert("unloaded " + std::to_string(id)).second)
            log("object " + std::to_string(id) + " is in mission zone " + std::to_string(it->second.zone) + ", not loaded");
        return nullptr;
    }
    return &it->second;
}

void LevelLogic::load_objects() {
    const Level& level = scene.level();
    std::unordered_set<int> templated;   // gol types whose template scripts are loaded
    bool tdkr_start = false;             // a TDKR level-start spawn point found
    start_spawn_id_ = -1;
    int zone = -1;            // the mission zone whose records these are (-1: none)
    size_t zone_end = 0;
    for (size_t i = 0; i < level.objects.size(); ++i) {
        const LevelObject& o = level.objects[i];
        if (o.kind == "zone") {   // [0] a mission zone, [4] its id; it spans its records up to content_end
            const bool mission = o.fields.size() > 4 && o.fields[0].i == 1 && o.content_end > o.offset;
            zone = mission ? o.fields[4].i : -1;
            zone_end = o.content_end;
        } else if (zone >= 0 && o.offset >= zone_end) {
            zone = -1;
        }
        // Scripts: every chunk defines Obj<id>_<Event> functions in the shared state; the
        // object types' templates define Templ<type>_<Event> for objects without their own.
        std::vector<std::string> chunks;
        collect_scripts(o.fields, chunks);
        for (const auto& [comp, v] : o.components) collect_scripts(v, chunks);
        if (auto g = gol.find(o.type); o.kind == "object" && g != gol.end() && templated.insert(o.type).second)
            for (const auto& [comp, flag] : resolved_components(g->second, gol))
                if (const Value* t = resolved_template(g->second, gol, comp)) collect_scripts(*t, chunks);
        for (const std::string& c : chunks) {
            ++stats_.chunks;
            if (!runtime_.load(c, o.kind + " " + o.name + " #" + std::to_string(i))) ++stats_.chunk_errors;
        }

        Object obj;
        obj.index = i;
        obj.type = o.kind == "object" ? o.name : o.kind;
        obj.collision_only = obj.type == "CollisionMesh";
        if (o.kind == "level_properties" && !o.fields.list.empty()) {
            properties_id_ = o.fields[0].i;
            if (o.fields.size() > 44 && o.fields[41].type == Value::String) {
                music_event_ = o.fields[41].s;
                while (!music_event_.empty() && music_event_.back() == ' ') music_event_.pop_back();
                play_time_limits_[0] = o.fields[43].as_float(), play_time_limits_[1] = o.fields[44].as_float();
            }
            default_lut_ = o.fields.size() > 38 && o.fields[38].type == Value::String ? o.fields[38].s : "";   // colour grading
            // The level's string table (CComponentLevelProperties [24]), after MAIN.
            strings = StringTable();
            strings.load(scene.vfs(), "MAIN");
            if (o.fields.size() > 24 && o.fields[24].type == Value::String) strings.load(scene.vfs(), o.fields[24].s);
            // TDKR: the level's table is [10] ("020_gothamcity.xml": strings/020_GothamCity_*.bin).
            if (o.fields.size() > 10 && o.fields[10].type == Value::String && o.fields[10].s.size() > 4 &&
                to_lower(o.fields[10].s.substr(o.fields[10].s.size() - 4)) == ".xml")
                strings.load(scene.vfs(), o.fields[10].s.substr(0, o.fields[10].s.size() - 4));
            continue;
        }
        if (o.kind == "waypoint" && o.fields.size() > 4) {
            obj.id = o.fields[0].i;
            if (o.fields.size() > 5 && o.fields[5].i >= 0) waypoint_next_[obj.id] = o.fields[5].i;
            obj.position = {o.fields[2].as_float(), o.fields[3].as_float(), o.fields[4].as_float()};
            objects_[obj.id] = obj;
            continue;
        }
        if (o.kind == "spawn_point" && o.base) {
            obj.id = o.base->id;
            obj.position = o.base->position;
            obj.yaw = o.base->rotation.z;
            // The level start: the first spawn point flagged as such (as Scene picks it).
            if (start_spawn_id_ < 0 && o.fields.size() > 0 && o.fields[0].i == 1) start_spawn_id_ = obj.id;
            // TDKR (CSpawnPointObject::Create): field 16 marks the level start (IsLevelStartPoint),
            // as Scene picks it; it wins over the first spawn point.
            if (!tdkr_start && o.fields.size() > 16 && o.fields[16].i == 1) start_spawn_id_ = obj.id, tdkr_start = true;
            objects_[obj.id] = obj;
            continue;
        }
        if (o.kind != "object") continue;
        auto g = gol.find(o.type);
        if (g == gol.end()) continue;
        if (auto p = o.components.find(kCompPath); p != o.components.end() && p->second.size() >= 3)
            path_first_[p->second[0].i] = p->second[2].i;
        auto comp = [&](int c) -> const Value* {
            if (auto it = o.components.find(c); it != o.components.end()) return &it->second;
            return resolved_template(g->second, gol, c);
        };

        if (const Value* t = o.components.count(kCompTrigger) ? &o.components.at(kCompTrigger) : nullptr; t && t->size() >= 14) {
            // CTriggerComponent::Load: id, position, rotation, scale, run count, enabled, script,
            // target (-1: the player), ?; the shape is the box/cylinder node times the scale.
            const Value& v = *t;
            obj.id = v[0].i;
            obj.position = {v[1].as_float(), v[2].as_float(), v[3].as_float()};
            obj.yaw = v[6].as_float();
            const glm::vec3 scale(v[7].as_float(), v[8].as_float(), v[9].as_float());
            obj.enabled = obj.visible = v[11].i != 0;
            Trigger tr;
            tr.runs_left = tr.runs = v[10].i;
            tr.player_target = v[13].i < 0;
            if (const Value* cyl = o.components.count(kCompBuiltinCylinder) ? comp(kCompBuiltinCylinder) : nullptr; cyl && cyl->size() >= 2) {
                tr.cylinder = true;
                tr.center = glm::vec2(obj.position);
                tr.radius = scale.x * (*cyl)[0].as_float();
                const float h = scale.z * (*cyl)[1].as_float();
                tr.min = obj.position - glm::vec3(tr.radius, tr.radius, 0);
                tr.max = obj.position + glm::vec3(tr.radius, tr.radius, h);
            } else {
                // CTriggerShapeAABB: x and y centred, z from the position up; rotation ignored.
                glm::vec3 box(15, 3, 2);
                if (const Value* b = comp(kCompBuiltinBox); b && b->size() >= 3) box = {(*b)[0].as_float(), (*b)[1].as_float(), (*b)[2].as_float()};
                const glm::vec3 lo = glm::vec3(-0.5f * box.x, -0.5f * box.y, 0) * scale;
                const glm::vec3 hi = glm::vec3(0.5f * box.x, 0.5f * box.y, box.z) * scale;
                tr.min = obj.position + glm::min(lo, hi);
                tr.max = obj.position + glm::max(lo, hi);
                // Tilted or turned boxes (e.g. a window's slab leaning with the glass).
                const glm::vec3 rot(v[4].as_float(), v[5].as_float(), v[6].as_float());
                auto turned = [](float deg) { return std::abs(std::remainder(deg, 360.0f)) > 0.01f; };
                if (turned(rot.x) || turned(rot.y) || turned(rot.z)) {
                    Transform tf;
                    tf.position = obj.position, tf.rotation = rot;
                    tr.rotated = true;
                    tr.to_local = glm::inverse(placement_matrix(tf));
                    tr.local_min = glm::min(lo, hi), tr.local_max = glm::max(lo, hi);
                    tr.min = glm::vec3(-1e30f), tr.max = glm::vec3(1e30f);
                }
            }
            obj.trigger = int(triggers_.size());
            triggers_.push_back(tr);
            ++stats_.triggers;
        } else {
            auto tf = o.base ? o.base : std::optional<Transform>();
            if (!tf) continue;   // objects without their own base are not addressable
            obj.id = tf->id;
            obj.position = tf->position;
            obj.yaw = tf->rotation.z;
            obj.enabled = tf->enabled;   // an enabled, invisible object (TDKR's usable planes) can be used
            obj.visible = tf->visible;
        }
        if (const Value* d = comp(kCompDoor); d && d->size() >= 1) {
            Door door;
            door.radius = (*d)[0].as_float();
            if (d->size() >= 3 && (*d)[1].type == Value::String) door.unlock_light = (*d)[1].s;
            if (d->size() >= 3 && (*d)[2].type == Value::String) door.lock_light = (*d)[2].s;
            obj.door = int(doors_.size());
            doors_.push_back(door);
            ++stats_.doors;
        }
        read_effects(obj, o);
        if (const Value* mp = comp(kCompMotionOnPath); mp && mp->size() >= 6 && o.base) {
            Mover m;
            m.id = obj.id, m.type = o.type, m.index = i;
            m.path = (*mp)[1].i;
            m.max_speed = (*mp)[2].as_float(), m.accel = (*mp)[3].as_float(), m.brake = (*mp)[4].as_float();
            m.loop = (*mp)[5].i != 0;
            m.position = o.base->position;
            movers_.push_back(m);
        }
        if (const Value* it = comp(kCompInteractive); it && it->size() >= 1) {
            obj.interactive = true;
            obj.interact_radius = (*it)[0].as_float();
            ++stats_.interactive;
        }
        // An actor starts in its state set's idle state (ActorGetState: "Ranged_Idle", ...).
        if (const Value* st = comp(0x5441A117); st && st->size() > 1 && (*st)[1].type == Value::String) obj.state = (*st)[1].s;
        obj.zone = zone, obj.home = obj.position, obj.init_enabled = obj.enabled, obj.init_visible = obj.visible;
        // A group (CGroupObject::ReadBuiltinData: c c [member ids]).
        if (auto gc = o.components.find(kCompGroup); gc != o.components.end() && gc->second.size() >= 3)
            for (const Value& m : gc->second[2].list) groups_[obj.id].push_back(m.type == Value::Int ? m.i : m.size() ? m[0].i : -1);
        objects_[obj.id] = obj;
    }
    stats_.objects = objects_.size();
    // Movers listed before their path objects were read: nothing to do, paths resolve on start.
}

void LevelLogic::read_effects(Object& obj, const LevelObject& o) {
    obj.effects.clear();
    auto g = gol.find(o.type);
    if (g == gol.end()) return;
    auto comp = [&](int c) -> const Value* {
        if (auto it = o.components.find(c); it != o.components.end()) return &it->second;
        return resolved_template(g->second, gol, c);
    };
    auto str = [](const Value& v) { return v.type == Value::String ? v.s : std::string(); };
    if (const Value* t = comp(kCompEffectTdkr); t && t->size() >= 4) {
        if (str((*t)[0]) != "None" && !str((*t)[0]).empty()) obj.effects.push_back({str((*t)[0]), str((*t)[2]), (*t)[3].i != 0});
    } else if (const Value* b = comp(kCompEffectBuiltIn); b && b->size() >= 4) {
        obj.effects.push_back({str((*b)[0]), str((*b)[2]), (*b)[3].i != 0});
    } else if (const Value* l = comp(kCompEffects); l && l->size() >= 1) {
        for (const Value& e : (*l)[0].list)
            if (e.size() >= 3) obj.effects.push_back({str(e[0]), str(e[1]), e[2].i != 0});
    }
}

bool LevelLogic::start_effect(Object& o, int index) {
    if (o.effects.empty()) return false;
    const bool builtin = o.effects.size() == 1 && o.effects[0].node.empty();
    if (builtin) index = 0;
    if (index < 0 || size_t(index) >= o.effects.size()) return false;
    Object::EffectSlot& e = o.effects[size_t(index)];
    if (e.handle >= 0) scene.particles().stop(e.handle);
    if (&o == &player_object_) o.position = player.center - glm::vec3(0, 0, player.radius.z), o.yaw = player.yaw;
    glm::mat4 at = glm::translate(glm::mat4(1), o.position) * glm::rotate(glm::mat4(1), glm::radians(o.yaw), glm::vec3(0, 0, 1));
    if (o.index != std::numeric_limits<size_t>::max()) at = scene.object_matrix(o.index, e.node);
    e.handle = scene.particles().start(e.effect, at);
    if (trace) log("effect " + e.effect + " on " + std::to_string(o.id) + (e.handle < 0 ? " (unknown)" : ""));
    return e.handle >= 0;
}

// The player's effects are parented to its node (CEffectsComponent::StartEffect): they go
// where it goes.
void LevelLogic::update_player_effects() {
    Object& o = player_object_;
    o.position = player.center - glm::vec3(0, 0, player.radius.z), o.yaw = player.yaw;
    const glm::mat4 at = glm::translate(glm::mat4(1), o.position) * glm::rotate(glm::mat4(1), glm::radians(o.yaw), glm::vec3(0, 0, 1));
    for (Object::EffectSlot& e : o.effects)
        if (e.handle >= 0) scene.particles().move(e.handle, at);
}

void LevelLogic::stop_effect(Object& o, int index) {
    if (o.effects.size() == 1) index = 0;
    if (index < 0 || size_t(index) >= o.effects.size()) return;
    Object::EffectSlot& e = o.effects[size_t(index)];
    if (e.handle >= 0) scene.particles().stop(e.handle);
    e.handle = -1;
}

bool LevelLogic::set_post_var(const std::string& effect, const std::string& var, float to, float seconds, const float* from) {
    float* v = nullptr;
    if (effect == "DOF") {
        if (var == "fFocus") v = &post.dof_focus;
        else if (var == "fRange") v = &post.dof_range;
        else if (var == "fFactor") v = &post.dof_factor;
        else if (var == "fBlurStep") v = &post.blur_step;
    } else if (effect == "HEIGHTFOG") {
        if (var == "FogTop") v = &post.fog_top;
        else if (var == "FogBottom") v = &post.fog_bottom;
        else if (var == "FogOpacity") v = &post.fog_opacity;
    }
    if (!v) return false;
    post_fades_.erase(std::remove_if(post_fades_.begin(), post_fades_.end(), [&](const PostFade& f) { return f.value == v; }),
                      post_fades_.end());
    if (from) *v = *from;
    if (seconds <= 0) *v = to;
    else post_fades_.push_back({v, *v, to, 0, seconds});
    return true;
}

void LevelLogic::start(StartMode mode) {
    const bool keep_player = mode == StartMode::Section;
    respawn_pending = false;
    runtime_.stop_all();
    if (mode != StartMode::Section) {   // the level's scripts start afresh (CLuaScriptManager::ClearGlobals)
        runtime_.reset();
        register_game_api(*this);
    }
    movers_.clear(), waypoint_next_.clear(), path_first_.clear(), vehicles_.clear();
    if (mode == StartMode::New) checkpoint_ = -1, saved_.reset();
    cinematics_.clear(), cinematic_mode = false, look_left_ = 0;
    camera_held_ = restore_view_ = false;
    tutorial_ = TutorialStep{};
    if (audio && !keep_player) audio->stop_all(), music_ = -1, music_wait_ = 1, jetpack_sound_ = -1, breath_sound_ = -1;
    music_event_.clear();
    dialog_.clear(), dialog_active_ = false, dialog_voice_ = -1, dialog_wait_ = 0;
    if (!keep_player) fading_ = false, shake_left_ = 0, time_factor_left_ = 0;
    anim_sounds_->clear();
    if (!keep_player) {   // (the grading version keeps counting so the renderer sees new curves)
        const uint32_t version = post.grading_version;
        post = PostParams{}, post.grading_version = version + 1, post_fades_.clear();
        heat_ = HeatHaze{};
        screen_effect_ = {};
        param_ramps_.clear();
        grading_.dirty = true;
    }
    if (!keep_player) timer = -1, timer_paused = false;
    if (!keep_player) controls = Controls{}, hud_enabled = true, interface_in_cinematic = false, weapon_state.clear(), objectives = {},
                      path_marker = {}, objective_markers.clear();
    objects_.clear();
    groups_.clear();
    group_refs_.clear();
    triggers_.clear();
    doors_.clear();
    delayed_.clear();
    messages_.clear();
    stats_ = {};
    level_ended = false;
    pending_section = 0;
    fog_locked = false;
    fog_zone_ = -2;
    level_fog_color_ = scene.fog_color;
    level_fog_start_ = scene.fog_start;
    level_fog_end_ = scene.fog_end;
    level_fog_density_ = scene.fog_density;
    // The AI controller's functions first (CAIController::Script_Init: AIFunctions, the sound
    // scripting the levels' chunks call), then the level, its quests and the main story.
    for (const auto& [type, g] : gol)
        if (g.name == "AIController") load_type_scripts(type);
    load_objects();
    find_cameras();
    find_fans();
    // Mission zones load when they become the current mission (none yet).
    for (auto& [id, o] : objects_)
        if (o.zone >= 0) o.enabled = o.visible = false, o.loaded = false, show(o, false);
    if (mode != StartMode::Section) quests.load(scene.level(), gol);
    mission_zone = -1;
    walkers_.clear();
    attack_targets.clear(), snipers.clear(), player_action.clear(), player_action_mask = 0, grapple_lock = -1;
    attacking_batman.clear(), batman_target = -1, killed_by_batman.clear(), watchers.clear(), stances.clear();
    player_did = {};
    play_time = 0, player_speed = 0, last_interaction = -1, camera_anim = -1, weather = false, player_hidden = false;
    win = {};
    gadget_ = {}, hud_state = "HUD", script_cam_ = {};
    hack = {.serial = hack.serial}, button_qte = {}, minigame_object = -1, pull_ = {};
    story_request = {};
    riding = -1, hanging_from = -1, held_use = {}, health_bar = -1, qte_navigation = false, bruce_style = false;
    scene.show_coronas(true);
    map_icons.clear();
    level_request = {};
    object_sounds.clear();
    ambience = {};
    last_player_ = player.center;
    control_glow.clear();
    for (auto& [id, o] : objects_)   // vehicles (CComponentVehicleCar) and their parts
        if (object_component(o, 0x96)) vehicle(id);
    if (!keep_player) {
        fov_from_ = fov_to_ = 70, fov_len_ = fov_t_ = 0;
        player_vehicle_ = player_gun_ = player_mech_ = -1;
        qte_ = Qte{}, time_factor_ = 1, time_factor_left_ = 0, slow_ = Slow{};
        zfar = 8000, move_sensitivity = 1, look_sensitivity = 1, player_rush = true, fall_damage = true, jetpack_unlocked = false;
        motion_blur = false;
        player.jetpack = false;
        player.push = glm::vec3(0);
        grading_ = Grading{};
        if (!default_lut_.empty()) grading_.on = true, grading_.current = default_lut_, log("colour grading: " + default_lut_);
    }
    // Apply the starting state: hidden objects are disabled; doors start closed.
    for (auto& [id, o] : objects_) {
        if (o.index == std::numeric_limits<size_t>::max()) continue;
        if (o.collision_only) scene.set_object_visible(o.index, false);
        if (!o.visible) {
            scene.set_object_visible(o.index, false);
            scene.set_object_collision(o.index, false);
        }
        if (o.door >= 0) scene.play_clip(o.index, "closed", true);
        if (o.visible)
            for (size_t k = 0; k < o.effects.size(); ++k)
                if (o.effects[k].autostart) start_effect(o, int(k));
    }
    // The limited-control rig: its scripts, its object (hidden until used), the player
    // type's defaults (CPlayerLimitedControlComponent::Load: strafe speed, yaw, pitch limits).
    limited_ = Limited{};
    for (Object::EffectSlot& e : player_object_.effects)
        if (e.handle >= 0) scene.particles().stop(e.handle);
    player_object_ = Object{};
    player_object_.type = "Player";
    for (const auto& [type_id, type] : gol) {
        if (type.name == "PlayerLimitedControl") limited_.rig_type = type_id;
        if (type.name != "Player") continue;
        if (auto c = type.templates.find(0x36); c != type.templates.end() && c->second.size() >= 3)
            for (int k = 0; k < 3; ++k) limited_.defaults[k] = c->second[size_t(k)].as_float();
        // Its effects (CEffectsComponent; the node "-1" is none: on the player's own node).
        LevelObject lo;
        lo.type = type_id;
        read_effects(player_object_, lo);
    }
    if (limited_.rig_type >= 0) {
        load_type_scripts(limited_.rig_type);
        limited_.rig = scene.spawn_object(limited_.rig_type, -1002, player.center, 0);
        if (limited_.rig != size_t(-1)) scene.set_object_viewmodel(limited_.rig), scene.set_object_visible(limited_.rig, false);
    }
    elevators_.clear();
    for (auto& [id, o] : objects_) {
        const Value* el = o.index < scene.level().objects.size() ? object_component(o, 0x18) : nullptr;
        const Object* a = el && el->size() >= 3 ? object((*el)[0].i) : nullptr;
        const Object* b = el && el->size() >= 3 ? object((*el)[1].i) : nullptr;
        if (!a || !b) continue;
        Elevator e;
        e.id = id, e.index = o.index, e.type = scene.level().objects[o.index].type;
        e.start = a->position, e.end = b->position, e.position = o.position;
        e.speed = std::max((*el)[2].as_float(), 0.1f);
        // CElevatorAIComponent::ReInit: it goes first to the end it is not at.
        e.to_end = glm::distance(e.position, e.start) < glm::distance(e.position, e.end);
        elevators_.push_back(e);
    }
    links_.clear();
    for (auto& [id, o] : objects_)
        if (const Value* l = o.index < scene.level().objects.size() ? object_component(o, 0x17) : nullptr; l && l->size() > 0 && (*l)[0].i > 0)
            link(id, (*l)[0].i);
    // The camera's zone is looked up by position on the first frame (CZonesManager::
    // UpdateVisibility); only teleports set it. A section reload (LoadLevelSections) puts
    // the player back where it was; the level's start script runs again, the spawn's does not.
    scene.zones().force_zone(ZoneSystem::kNone);
    // The player starts at the level's start spawn point, whose script plays the intro and
    // then moves them on (Scene's start position skips ahead for runs without scripts).
    if (mode == StartMode::New && start_spawn_id_ >= 0)
        if (const Object* sp = object(start_spawn_id_)) teleport_player(*sp);
    if (properties_id_ >= 0) start_script("Obj" + std::to_string(properties_id_) + "_OnEnter", properties_id_);
    if (start_spawn_id_ >= 0 && mode == StartMode::New && !skip_intro) start_script("Obj" + std::to_string(start_spawn_id_) + "_OnSpawn", 0);
}

// --- linked objects -------------------------------------------------------------------

void LevelLogic::load_type_scripts(int type) {
    auto g = gol.find(type);
    if (g == gol.end()) return;
    std::vector<std::string> chunks;
    for (const auto& [comp, flag] : resolved_components(g->second, gol))
        if (const Value* t = resolved_template(g->second, gol, comp)) collect_scripts(*t, chunks);
    for (const std::string& c : chunks) {
        ++stats_.chunks;
        if (!runtime_.load(c, g->second.name)) ++stats_.chunk_errors;
    }
}

bool LevelLogic::link(int id, int target) {
    const Object* o = object(id);
    const Object* t = object(target);
    constexpr size_t kNone = std::numeric_limits<size_t>::max();
    if (!o || !t || o->index == kNone || t->index == kNone || id == target) return false;
    unlink(id);
    Link l;
    l.id = id, l.target = target;
    l.last = scene.object_matrix(t->index);
    l.local = glm::inverse(l.last) * scene.object_matrix(o->index);
    links_.push_back(l);
    return true;
}

bool LevelLogic::link_at(int id, int target, const std::string& node, const glm::vec3& offset) {
    const Object* o = object(id);
    const Object* t = object(target);
    constexpr size_t kNone = std::numeric_limits<size_t>::max();
    if (!o || !t || o->index == kNone || t->index == kNone || id == target) return false;
    unlink(id);
    Link l;
    l.id = id, l.target = target, l.node = node;
    l.local = glm::translate(glm::mat4(1), offset);
    links_.push_back(l);
    return true;
}

void LevelLogic::unlink(int id) {
    if (id == 0 && hanging_from >= 0) {   // Batman lets go of the helicopter
        if (trace) log("lets go of " + std::to_string(hanging_from));
        hanging_from = -1;
    }
    for (const Link& l : links_)
        if (l.id == id && l.held && combat) combat->npc_order(id, "imobile", 0, false);
    std::erase_if(links_, [id](const Link& l) { return l.id == id; });
}

void LevelLogic::update_links() {
    for (Link& l : links_) {
        Object* o = object(l.id);
        const Object* t = object(l.target);
        if (!o || !t) continue;
        const bool actor = combat && combat->is_actor(l.id);
        if (actor && !l.held) l.held = combat->npc_order(l.id, "imobile", 0, true);
        const glm::mat4 m = scene.object_matrix(t->index, l.node);
        if (m == l.last) continue;
        l.last = m;
        const glm::mat4 placed = m * l.local;
        o->position = glm::vec3(placed[3]);
        if (actor) combat->place_actor(l.id, o->position);
        else scene.set_object_transform(o->index, placed);
    }
}

// --- checkpoints (the Checkpoint itself: checkpoint.h) ----------------------------------

void LevelLogic::save_checkpoint(int spawn_id) {
    if (restoring_) return;
    if (spawn_id <= 0) {   // SaveGame(): the first enabled spawn point in the player's zone
        const int zone = scene.zones().current();
        size_t best = std::numeric_limits<size_t>::max();
        for (const auto& [id, o] : objects_)
            if (o.type == "spawn_point" && o.enabled && o.index < best && scene.zones().object_zone(o.index) == zone)
                best = o.index, spawn_id = id;
    }
    if (spawn_id <= 0 || !object(spawn_id)) {
        log("SaveGame: no spawn point " + std::to_string(spawn_id));
        return;
    }
    auto cp = std::make_shared<Checkpoint>();
    cp->globals = runtime_.save_globals();
    cp->quests = quests.save();
    cp->spawn = spawn_id;
    cp->section = scene.section();
    for (const auto& [id, o] : objects_) {
        Checkpoint::SavedObject so;
        so.enabled = o.enabled, so.visible = o.visible, so.dead = o.dead, so.interact_enabled = o.interact_enabled;
        so.health = o.health, so.state = o.state, so.position = o.position, so.yaw = o.yaw;
        for (const Object::EffectSlot& e : o.effects) so.effects.push_back(e.handle >= 0);
        const bool actor = combat && combat->is_actor(id);
        if (o.index != std::numeric_limits<size_t>::max() && o.door < 0 && !actor && !mover(id) && !vehicles_.count(id))
            so.has_clip = scene.clip_state(o.index, so.clip);
        cp->objects[id] = std::move(so);
    }
    for (const Trigger& t : triggers_) cp->trigger_runs.push_back(t.runs_left);
    cp->ammo.assign(weapon_ammo.begin(), weapon_ammo.end());
    for (const auto& [id, cam] : cameras_)   // CSurveillanceCameraComponent::SaveSave
        if (cam.off) cp->cameras_off.push_back(id);
    for (const Door& d : doors_) cp->doors.push_back({d.state, d.locked, d.keep_unlocked, d.forced_open});
    for (const Mover& m : movers_) cp->movers.emplace_back(m.id, m.position);
    for (const auto& [id, v] : vehicles_) cp->vehicles.push_back({id, v.position, v.yaw});
    cp->objectives = objectives, cp->markers = objective_markers, cp->path_marker = path_marker;
    cp->fog_color = scene.fog_color, cp->fog_start = scene.fog_start, cp->fog_end = scene.fog_end, cp->fog_density = scene.fog_density;
    cp->fog_locked = fog_locked;
    cp->post = post, cp->heat = heat_;
    cp->grading = grading_.current, cp->grading_on = grading_.on;
    cp->mech = player_mech_, cp->vehicle = player_vehicle_, cp->gun = player_gun_;
    if (combat) cp->combat = combat->save();
    saved_ = std::move(cp);
    checkpoint_ = spawn_id;
    log("checkpoint " + std::to_string(spawn_id));
    if (on_checkpoint) on_checkpoint();
    if (audio) audio->play("sfx_save_progress");
    message("FPS_GAME_SAVED", 2.5f);
}

uint32_t LevelLogic::checkpoint_section() const { return saved_ ? saved_->section : 1; }

void LevelLogic::respawn_at_checkpoint() {
    respawn_pending = false;
    const int zone = mission_zone;   // the mission it was in stays loaded
    const std::shared_ptr<const Checkpoint> cp = saved_;
    if (!cp) {
        start(StartMode::New);
        if (combat) combat->start(true);
        return;
    }
    // CLevel::RestartLevel: the scripts afresh and the level properties' OnEnter started;
    // then (CLevel::RespawnInit) the objects as loaded, with the checkpoint put back.
    quests.carry_over = true;   // the quests and the main story stay where they were
    start(StartMode::Respawn);
    if (combat) combat->start(true);
    runtime_.restore_globals(cp->globals);   // (CLevel::RespawnInit: LoadCurrentProgress, the scripts' _G)
    restoring_ = true;
    runtime_.update(0);
    restoring_ = false;
    if (zone >= 0 && mission_zone != zone) {
        if (mission_zone >= 0) load_zone(mission_zone, false);
        mission_zone = zone, load_zone(zone, true);
    }
    restore_checkpoint(*cp);
    quests.restore(cp->quests);
    quests.respawned();
}

void LevelLogic::restore_checkpoint(const Checkpoint& cp) {
    for (const auto& [id, so] : cp.objects) {
        Object* o = object(id);
        if (!o) continue;
        o->health = so.health, o->dead = so.dead, o->state = so.state, o->interact_enabled = so.interact_enabled;
        const bool moved = glm::distance(o->position, so.position) > 1e-3f || std::abs(o->yaw - so.yaw) > 1e-3f;
        o->position = so.position, o->yaw = so.yaw;
        if (moved && o->index != std::numeric_limits<size_t>::max() && o->door < 0 && !mover(id) && !vehicles_.count(id) &&
            !(combat && combat->is_actor(id)))
            scene.set_object_pose(o->index, o->position, o->yaw);
        enable(*o, so.enabled && !so.dead);   // CHealthComponent::SaveLoad: the dead are disabled
        if (so.enabled && o->visible != so.visible) show(*o, so.visible);
        for (size_t k = 0; k < o->effects.size() && k < so.effects.size(); ++k) {
            if (so.effects[k] && o->effects[k].handle < 0 && o->enabled) start_effect(*o, int(k));
            if (!so.effects[k] && o->effects[k].handle >= 0) stop_effect(*o, int(k));
        }
        // CAnimationComponent::SaveLoad plays the clip again; one that had run its course
        // is held at its end rather than played over.
        if (so.has_clip && o->index != std::numeric_limits<size_t>::max()) {
            Scene::ClipState now;
            if (!scene.clip_state(o->index, now) || now.name != so.clip.name || now.reverse != so.clip.reverse) {
                scene.play_clip(o->index, so.clip.name, so.clip.loop, so.clip.reverse);
                if (!so.clip.loop && so.clip.ms >= so.clip.length - 1.0f) scene.set_clip_time(o->index, so.clip.length / 1000.0f);
            }
        }
    }
    for (size_t i = 0; i < triggers_.size() && i < cp.trigger_runs.size(); ++i) triggers_[i].runs_left = cp.trigger_runs[i], triggers_[i].inside = false;
    weapon_ammo.clear();
    for (const auto& [weapon, n] : cp.ammo) weapon_ammo[weapon] = n;
    for (auto& [id, cam] : cameras_) cam.off = std::find(cp.cameras_off.begin(), cp.cameras_off.end(), id) != cp.cameras_off.end(), cam.seen = 0;
    for (size_t i = 0; i < doors_.size() && i < cp.doors.size(); ++i) {
        const Checkpoint::SavedDoor& sd = cp.doors[i];
        doors_[i].locked = sd.locked, doors_[i].keep_unlocked = sd.keep_unlocked, doors_[i].forced_open = sd.forced_open;
        if (sd.forced_open && (sd.state == Door::State::Open || sd.state == Door::State::Opening))
            for (auto& [id, o] : objects_)
                if (o.door == int(i)) set_door(o, true);
    }
    for (const auto& [id, at] : cp.movers)
        if (Mover* m = mover(id); m && glm::distance(m->position, at) > 1e-3f) m->position = at, scene.move_object(m->index, at);
    for (const Checkpoint::SavedVehicle& sv : cp.vehicles)
        if (Vehicle* v = vehicle(sv.id)) {
            v->position = sv.position, v->yaw = sv.yaw;
            scene.set_object_pose(v->index, v->position, v->yaw);
            if (Object* o = object(sv.id)) o->position = sv.position, o->yaw = sv.yaw;
            place_vehicle_parts(*v);
        }
    objectives = cp.objectives, objectives.show = 0, objectives.alpha = 0;
    objective_markers = cp.markers, path_marker = cp.path_marker;
    scene.fog_color = cp.fog_color, scene.fog_start = cp.fog_start, scene.fog_end = cp.fog_end, scene.fog_density = cp.fog_density;
    for (FogFade& f : fog_) f.active = false;
    fog_locked = cp.fog_locked;
    const uint32_t version = post.grading_version;
    post = cp.post, post.grading_version = version + 1, post_fades_.clear();
    heat_ = cp.heat;
    grading_ = Grading{}, grading_.current = cp.grading, grading_.on = cp.grading_on, grading_.dirty = true;
    if (combat) combat->restore(cp.combat);
    // PlayerComponent::SpawnPlayer at the checkpoint, in what it was in; then the spawn
    // point's script (CSpawnPointObject::SpawnedHere).
    if (Object* sp = object(cp.spawn)) teleport_player(*sp);
    if (Object* m = cp.mech >= 0 ? object(cp.mech) : nullptr) {
        m->position = player.center - glm::vec3(0, 0, player.radius.z), m->yaw = player.yaw;
        enter_mech(cp.mech);
    } else if (cp.vehicle >= 0) {
        enter_vehicle(cp.vehicle);
    } else if (cp.gun >= 0) {
        man_gun(cp.gun);
    }
    checkpoint_ = cp.spawn;
    start_script("Obj" + std::to_string(cp.spawn) + "_OnSpawn", 0);
    start_fade(true, 1000, glm::vec3(0));
    log("respawned at checkpoint " + std::to_string(cp.spawn));
}

// (TDKR's EnableControls switches the interface and the input, not the design flags of
// SetGlideControlEnable (CBaseControlScheme::EnableGlideControl): the glide keeps its own.)
void LevelLogic::enable_controls(bool on) {
    controls.move = controls.rotate = controls.jump = controls.combat = controls.grapnel = controls.counter = controls.gadgets =
        controls.silent = controls.batarang = on;
    controls.shoot = controls.aim = controls.rush = on;
    hud_enabled = on;
}

// CZonesManager::NextMission: the mission zone changes. The old one's objects go
// (UnloadMission), the new one's come in as the level placed them, and its Obj<id>_Init runs.
void LevelLogic::next_mission(int zone) {
    if (zone == mission_zone) return;
    if (mission_zone >= 0) load_zone(mission_zone, false);   // CZonesManager::UnloadMission
    mission_zone = zone;
    if (zone >= 0) load_zone(zone, true);
    if (zone >= 0) start_script("Obj" + std::to_string(zone) + "_Init", zone);
}

void LevelLogic::move_group(int group, int dest, int ref) {
    if (!is_group(group)) return;
    if (auto it = group_refs_.find(group); it != group_refs_.end()) ref = it->second;
    const Object* d = object(dest);
    const Object* r = object(ref);
    if (!d || !r) return;
    const glm::vec3 from = r->position, to = d->position;
    const float turn = d->yaw - r->yaw, a = glm::radians(turn), c = std::cos(a), s = std::sin(a);
    std::vector<int> pending{group};
    for (size_t k = 0; k < pending.size() && k < 4096; ++k)
        for (int id : group_members(pending[k])) {
            if (is_group(id)) {
                pending.push_back(id);
                continue;
            }
            Object* o = object(id);
            if (!o) continue;
            const glm::vec2 rel(o->position - from);
            o->position = glm::vec3(to.x + c * rel.x - s * rel.y, to.y + s * rel.x + c * rel.y, to.z + o->position.z - from.z);
            o->yaw += turn;
            if (o->index < scene.level().objects.size() && o->type != "waypoint") scene.set_object_pose(o->index, o->position, o->yaw);
        }
    group_refs_[group] = dest;
}

void LevelLogic::unload_mission(int zone) {
    if (zone < 0 || zone != mission_zone) return;
    load_zone(zone, false);
    mission_zone = -1;
}

void LevelLogic::load_zone(int zone, bool load) {
    int n = 0;
    for (auto& [id, o] : objects_) {
        if (o.zone != zone || o.loaded == load) continue;
        ++n;
        if (load) {   // as the level has it
            o.loaded = true, o.dead = false, o.state = default_state(o, 1);
            enable(o, o.init_enabled);
            show(o, o.init_visible);
            if (o.position != o.home && o.index < scene.level().objects.size()) {   // moved on an earlier visit
                o.position = o.home;
                if (const LevelObject& lo = scene.level().objects[o.index]; lo.base) o.yaw = lo.base->rotation.z;
                scene.set_object_pose(o.index, o.position, o.yaw);
            }
        } else {
            enable(o, false);
            show(o, false);
            o.loaded = false;
            runtime_.stop_owner(id);
        }
    }
    if (trace && n) log(std::string(load ? "mission zone in: " : "mission zone out: ") + std::to_string(zone) + " (" + std::to_string(n) + " objects)");
}

void LevelLogic::start_script(const std::string& function, int owner) {
    if (runtime_.start(function, owner) && trace) log("start " + function);
}

LevelLogic::Object& LevelLogic::add_object(int id, size_t index, const std::string& type, const glm::vec3& position, float yaw) {
    Object& o = objects_[id];
    o = Object{};
    o.id = id;
    o.index = index;
    o.type = type;
    o.position = position;
    o.yaw = yaw;
    o.home = position;
    o.enabled = o.visible = false;
    if (index < scene.level().objects.size()) read_effects(o, scene.level().objects[index]);
    o.state = default_state(o, 1);
    ++stats_.objects;
    return o;
}

void LevelLogic::enable(Object& o, bool on) {
    if (o.enabled == on) return;
    if (trace) log((on ? "enable " : "disable ") + std::to_string(o.id) + " " + o.type);
    o.enabled = on;
    show(o, on);
    if (!on)
        for (size_t k = 0; k < o.effects.size(); ++k) stop_effect(o, int(k));
    if (o.trigger >= 0 && !on) triggers_[size_t(o.trigger)].inside = false;
    // CTriggerComponent::Enable: turned on again, its run count starts over.
    if (o.trigger >= 0 && on) triggers_[size_t(o.trigger)].runs_left = triggers_[size_t(o.trigger)].runs;
}

void LevelLogic::show(Object& o, bool on) {
    o.visible = on;
    if (o.index == std::numeric_limits<size_t>::max()) return;
    scene.set_object_visible(o.index, on && !o.collision_only);
    if (o.door < 0 || doors_[size_t(o.door)].state == Door::State::Closed) scene.set_object_collision(o.index, on);
}

void LevelLogic::message(const std::string& id, float seconds) {
    std::string text = strings.find(id);
    if (const std::string prefix = "Objective: "; text.empty() && id.rfind(prefix, 0) == 0) {
        const std::string t = strings.find(id.substr(prefix.size()));
        if (!t.empty()) text = prefix + t;
    }
    if (text.empty()) text = id;
    // Desktop: the prompts' wording for mouse and keyboard, and keys for any button glyphs
    // (^NN^) left. Touch keeps the original (its glyphs are the on-screen buttons).
    if (input == InputScheme::Desktop) {
        if (std::string d = desktop_text(id); !d.empty()) text = d;
        for (size_t at = text.find('^'); at != std::string::npos; at = text.find('^', at + 1)) {
            const size_t end = text.find('^', at + 1);
            if (end == std::string::npos) break;
            const std::string key = desktop_key(std::atoi(text.substr(at + 1, end - at - 1).c_str()));
            if (key.empty()) continue;
            text.replace(at, end - at + 1, key);
        }
    }
    // Voice-over lines name their speaker: VO_<code>_..., the code standing for one of
    // MAIN's STR_NAME_* entries (inferred from the ids and the reference footage).
    std::string speaker;
    if (id.rfind("VO_", 0) == 0) {
        static const std::pair<const char*, const char*> kSpeakers[] = {
            {"KAL", "NAME_KAL"},         {"YEL", "NAME_YELENA"},  {"RUF", "NAME_RUFUS"},    {"MAZ", "NAME_MAZRAH"},
            {"BEC", "NAME_BECKER"},      {"PRO", "NAME_PROMETHEUS"}, {"CHA", "NAME_CHAO"},  {"WIL", "NAME_WILCOX"},
            {"THU", "NAME_BOREAS_THUG"}, {"BOR", "NAME_BOREAS_THUG"}, {"BOSS", "NAME_THUG_BOSS"}, {"VOL", "NAME_VOLTERITE"},
            {"OVE", "NAME_OVERSEER"}};
        const size_t end = id.find('_', 3);
        const std::string code = id.substr(3, end == std::string::npos ? std::string::npos : end - 3);
        for (const auto& [c, name] : kSpeakers)
            if (code == c) speaker = strings.find(name);
    }
    messages_.push_back({id, text, seconds, speaker});
    while (messages_.size() > 8) messages_.pop_front();
    log("message: " + id + (text != id ? " \"" + text + "\"" : ""));
}

// PlayerComponent::TeleportInSpawnpoint: the player stands at the spawn point, whose
// Obj<id>_OnSpawn runs (owned by the player).
void LevelLogic::teleport_player(const Object& spawn) {
    player.spawn(spawn.position, spawn.yaw);
    start_script("Obj" + std::to_string(spawn.id) + "_OnSpawn", 0);
    if (have_handover_) {   // a cinematic hands over to the player where its camera was
        player.yaw = glm::degrees(std::atan2(-handover_.x, handover_.y));
        player.pitch = glm::degrees(std::asin(glm::clamp(handover_.z, -1.0f, 1.0f)));
        have_handover_ = false;
    }
    if (spawn.index != std::numeric_limits<size_t>::max()) scene.zones().force_zone(scene.zones().object_zone(spawn.index));
}

void LevelLogic::send_event_later(const std::string& event, float seconds, int id) {
    delayed_.push_back({event, id, seconds});
}

void LevelLogic::fade_fog(int what, glm::vec4 target, float seconds) {
    FogFade& f = fog_[what];
    switch (what) {
        case 0: f.from = scene.fog_color; break;
        case 1: f.from = glm::vec4(scene.fog_start); break;
        case 2: f.from = glm::vec4(scene.fog_end); break;
        default: f.from = glm::vec4(scene.fog_density); break;
    }
    f.to = target;
    f.elapsed = 0;
    f.duration = std::max(seconds, 0.0f);
    f.active = true;
}

// CLevelFog::UpdateFromZones: the camera's zone sets the fog (its own, or the level's),
// unless a script has locked it.
void LevelLogic::update_zone_fog() {
    const int zone = scene.zones().current();
    if (zone == fog_zone_ || fog_locked) return;
    const bool first = fog_zone_ == -2;
    fog_zone_ = zone;
    glm::vec4 color = level_fog_color_;
    float start = level_fog_start_, end = level_fog_end_, density = level_fog_density_;
    if (zone >= 0) {
        const ZoneSystem::Fog& f = scene.zones().zones()[size_t(zone)].fog;
        if (f.override) color = f.color, start = f.start, end = f.end, density = f.density;
    }
    const float seconds = first ? 0.0f : 1.0f;
    fade_fog(0, color, seconds);
    fade_fog(1, glm::vec4(start), seconds);
    fade_fog(2, glm::vec4(end), seconds);
    fade_fog(3, glm::vec4(density), seconds);
}

void LevelLogic::update_fog(float dt) {
    for (int i = 0; i < 4; ++i) {
        FogFade& f = fog_[i];
        if (!f.active) continue;
        f.elapsed += dt;
        const float t = f.duration > 0 ? std::min(f.elapsed / f.duration, 1.0f) : 1.0f;
        const glm::vec4 v = f.from + (f.to - f.from) * t;
        if (i == 0) scene.fog_color = glm::vec4(glm::vec3(v), scene.fog_color.a);
        else if (i == 1) scene.fog_start = v.x;
        else if (i == 2) scene.fog_end = v.x;
        else scene.fog_density = v.x;
        if (t >= 1) f.active = false;
    }
}

// CTriggerComponent::Update / TestTargetForAction / DoAction for the player: the
// player's capsule (feet up to its height, with its radius) against the trigger shape.
void LevelLogic::update_triggers() {
    const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
    const float height = 2 * player.radius.z, r = player.radius.x;
    for (auto& [id, o] : objects_) {
        if (o.trigger < 0) continue;
        Trigger& t = triggers_[size_t(o.trigger)];
        if (!o.enabled) continue;
        const std::string prefix = "Obj" + std::to_string(id) + "_";
        auto act = [&](const char* event) {
            const std::string fn = prefix + event;
            if (!runtime_.has_function(fn)) return;
            start_script(fn, id);
            if (t.runs_left > 0 && --t.runs_left == 0) enable(o, false);
        };
        // An Obj<id>_Always runs every update instead of the inside/outside tests.
        if (runtime_.has_function(prefix + "Always")) {
            act("Always");
            continue;
        }
        if (!t.player_target) continue;
        bool in = feet.z <= t.max.z && feet.z + height >= t.min.z;
        if (t.rotated) {
            const glm::vec3 c = glm::vec3(t.to_local * glm::vec4(player.center, 1));
            const glm::vec3 reach(r, r, player.radius.z);
            in = glm::all(glm::greaterThanEqual(c, t.local_min - reach)) && glm::all(glm::lessThanEqual(c, t.local_max + reach));
        } else if (in && t.cylinder) {
            const float reach = t.radius + r;
            in = glm::dot(glm::vec2(feet) - t.center, glm::vec2(feet) - t.center) <= reach * reach;
        } else if (in) {
            in = feet.x + r >= t.min.x && feet.x - r <= t.max.x && feet.y + r >= t.min.y && feet.y - r <= t.max.y;
        }
        const bool was = t.inside;
        t.inside = in;
        if (trace && in != was) {
            char at[64];
            std::snprintf(at, sizeof at, " at (%.1f %.1f %.1f)", player.center.x, player.center.y, player.center.z);
            log((in ? "enter trigger " : "leave trigger ") + std::to_string(id) + at);
        }
        if (in && !was) act("OnEnter");
        if (!in && was) act("OnExit");
        if (!o.enabled) continue;
        act(in ? "WhileIn" : "WhileOut");
    }
}

// CDoorAIComponent: a closed door opens when the player comes within its radius, unless
// it is locked (scripts, or enemies around and not kept unlocked); it closes again when
// the player has left. Collision is off from the moment it starts opening.
void LevelLogic::update_doors(float) {
    const glm::vec3 eye = player.eye();
    for (auto& [id, o] : objects_) {
        if (o.door < 0 || !o.enabled) continue;
        Door& d = doors_[size_t(o.door)];
        bool near = glm::distance(eye, o.position) <= d.radius;
        for (const glm::vec3& a : actor_positions) near = near || glm::distance(a, o.position) <= d.radius * 0.6f;
        const bool locked = d.locked && !d.keep_unlocked;
        // The lock lights follow the lock state, whatever the door's clip says.
        if (!d.unlock_light.empty()) scene.set_node_visible(o.index, d.unlock_light, !locked);
        if (!d.lock_light.empty()) scene.set_node_visible(o.index, d.lock_light, locked);
        switch (d.state) {
            case Door::State::Closed:
                if (d.forced_open || (near && !locked)) set_door(o, true);
                break;
            case Door::State::Opening:
                // Half open: walkable, and no longer hides what is behind it.
                if (scene.clip_progress(o.index) >= 0.5f) scene.set_object_collision(o.index, false);
                if (scene.clip_finished(o.index)) {
                    d.state = Door::State::Open;
                    scene.set_object_collision(o.index, false);
                    const bool ok = scene.play_clip(o.index, "open", true);
                    if (trace) log("door " + std::to_string(id) + " open" + (ok ? "" : " (no clip)"));
                }
                break;
            case Door::State::Open:
                if (!d.forced_open && (!near || locked)) set_door(o, false);
                break;
            case Door::State::Closing:
                if (scene.clip_finished(o.index)) {
                    d.state = Door::State::Closed;
                    scene.play_clip(o.index, "closed", true);
                    scene.zones().set_door_open(o.index, false);
                }
                break;
        }
    }
}

void LevelLogic::set_door(Object& o, bool open_now) {
    Door& d = doors_[size_t(o.door)];
    if (trace) log((open_now ? "open door " : "close door ") + std::to_string(o.id));
    // A door is an open portal from the moment it starts opening until it has fully closed.
    if (open_now) scene.zones().set_door_open(o.index, true);
    if (open_now) {
        if (d.state == Door::State::Open || d.state == Door::State::Opening) return;
        d.state = Door::State::Opening;
        if (audio && o.index < scene.level().objects.size())
            anim_sounds_->play_state(*audio, scene.level().objects[o.index].type, "Opening", o.position);
        // Its collision (which also hides what is behind it from view rays) goes once half open.
        if (!scene.play_clip(o.index, "opening", false)) {
            d.state = Door::State::Open;
            scene.set_object_collision(o.index, false);
        }
    } else {
        if (d.state == Door::State::Closed || d.state == Door::State::Closing) return;
        // There is no closing clip: "opening" plays backwards. Collision returns at once,
        // so nothing can walk into a closing door.
        d.state = Door::State::Closing;
        if (audio && o.index < scene.level().objects.size())
            anim_sounds_->play_state(*audio, scene.level().objects[o.index].type, "Closing", o.position);
        if (!scene.play_clip(o.index, "opening", false, true)) {
            d.state = Door::State::Closed;
            scene.play_clip(o.index, "closed", true);
            scene.zones().set_door_open(o.index, false);
        }
        if (o.visible) scene.set_object_collision(o.index, true);
    }
}

// ShowObjectiveMessage (Android 1.1.6): with a PowerA pad, STR_TUTORIAL..., STR_SAVE_LUCIUS_OBJ_6...
// and STR_TURBULENCE_THIEF_MINIGAME... become their _POWER_A versions, when the table has them.
std::string LevelLogic::prompt_id(const std::string& id) const {
    if (!gamepad) return id;
    for (const char* prefix : {"STR_TUTORIAL", "STR_SAVE_LUCIUS_OBJ_6", "STR_TURBULENCE_THIEF_MINIGAME"}) {
        const std::string p = prefix;
        if (id.compare(0, p.size(), p) == 0) {
            const std::string pad = p + "_POWER_A" + id.substr(p.size());
            return strings.find(pad).empty() ? id : pad;
        }
    }
    return id;
}

bool LevelLogic::exec(const std::string& code) {
    static int counter = 0;
    const std::string name = "Exec" + std::to_string(++counter);
    if (!runtime_.load("function " + name + "(...)\n" + code + "\nend\n", name)) return false;
    start_script(name, 0);
    return true;
}

bool LevelLogic::has_object_script(const Object& o, const std::string& event) const {
    if (runtime_.has_function("Obj" + std::to_string(o.id) + "_" + event)) return true;
    return o.index < scene.level().objects.size() &&
           runtime_.has_function("Templ" + std::to_string(scene.level().objects[o.index].type) + "_" + event);
}

bool LevelLogic::start_object_script(const Object& o, const std::string& event) {
    const std::string own = "Obj" + std::to_string(o.id) + "_" + event;
    if (runtime_.has_function(own)) return start_script(own, o.id), true;
    if (o.index < scene.level().objects.size()) {
        const std::string templ = "Templ" + std::to_string(scene.level().objects[o.index].type) + "_" + event;
        if (runtime_.has_function(templ)) return start_script(templ, o.id), true;
    }
    return false;
}

bool LevelLogic::use() {
    if (riding >= 0) {   // off the Batpod
        dismount_batpod();
        return true;
    }
    // Out of the mounted gun or the mech: its interaction's exit.
    for (int id : {player_gun_, player_mech_}) {
        if (id < 0) continue;
        const Object* o = object(id);
        if (!o || !start_object_script(*o, "OnExit")) leave_vehicle();
        return true;
    }
    // CInteractionComponent::IsInInteractionRadius: the nearest usable object within its radius.
    Object* best = nullptr;
    float best_d = 1e30f;
    for (auto& [id, o] : objects_) {
        if (!o.interactive || !o.enabled || !o.interact_enabled || o.dead || !use_does_something(o)) continue;
        const float d = id == hanging_from ? 0.0f : glm::distance(glm::vec2(o.position), glm::vec2(player.center));
        if (id != hanging_from && (d > o.interact_radius || d > best_d || std::abs(o.position.z - player.center.z) > 3)) continue;
        best = &o;
        best_d = d;
    }
    if (!best) return false;
    // The original pushes its circuit minigame (GS_FaneMinigame) first when the instance's
    // minigame flag ([3]) is set, but no campaign object sets it: the use is OnEnter alone.
    // TDKR (CInteractionComponent::OnInteract): with a target object (SetInteractionParams),
    // that object is enabled instead; its button goes either way.
    if (best->interact_hold > 0) {   // a use that takes time: it starts
        if (held_use.object != best->id) {
            held_use = {best->id, best->interact_hold, best->interact_hold};
            if (trace) log("use " + std::to_string(best->id) + ": held for " + std::to_string(best->interact_hold) + " s");
        }
        return true;
    }
    interact(*best);
    return true;
}

void LevelLogic::interact(Object& o) {
    last_interaction = o.id;
    if (trace) log("use " + std::to_string(o.id));
    if (Object* t = o.interact_target >= 0 ? object(o.interact_target) : nullptr) {
        o.interact_enabled = false;
        enable(*t, true);
        if (t->trigger >= 0) triggers_[size_t(t->trigger)].inside = false;
    } else {
        start_object_script(o, "OnScriptEvent");
    }
}

void LevelLogic::update_held_use(float dt) {
    if (held_use.object < 0) return;
    Object* o = object(held_use.object);
    const bool in_reach = o && o->enabled && o->interact_enabled && !o->dead &&
                          glm::distance(glm::vec2(o->position), glm::vec2(player.center)) <= o->interact_radius + 1.0f;
    if (!in_reach || !attacking_batman.empty()) {   // let go: it starts over
        if (trace) log("use " + std::to_string(held_use.object) + ": let go");
        held_use = {};
        return;
    }
    if ((held_use.left -= dt) > 0) return;
    held_use = {};
    interact(*o);
}

float LevelLogic::fade_alpha() const {
    if (!fading_) return 0;
    const float k = fade_left_ / fade_total_;   // 1 at the start, 0 at the end
    return fade_in_ ? k : 1 - k;                // fading in leaves a clear screen; out, a covered one
}

glm::vec2 LevelLogic::shake_offset() const {
    if (shake_left_ <= 0) return glm::vec2(0);
    const float a = shake_amount_ * 4.0f * (shake_left_ / std::max(shake_total_, 1e-3f));
    return glm::vec2(std::sin(shake_clock_ * 47.0f) + 0.5f * std::sin(shake_clock_ * 91.0f),
                     std::cos(shake_clock_ * 53.0f) + 0.5f * std::sin(shake_clock_ * 77.0f)) * a;
}

const Value* LevelLogic::object_component(const Object& o, int comp) const {
    if (o.index >= scene.level().objects.size()) return nullptr;
    const LevelObject& lo = scene.level().objects[o.index];
    if (auto it = lo.components.find(comp); it != lo.components.end()) return &it->second;
    auto g = gol.find(lo.type);
    return g == gol.end() ? nullptr : resolved_template(g->second, gol, comp);
}

namespace {

// Script events in a cinematic's tracks: [start ms, end ms, "function Name(...) ..."].
void collect_events(const Value& v, std::vector<std::pair<float, std::string>>& out) {
    if (v.type != Value::List) return;
    if (v.size() >= 3 && v[0].type == Value::Int && v[2].type == Value::String) {
        const std::string& s = v[2].s;
        const size_t f = s.find("function ");
        if (f != std::string::npos) {
            const size_t a = f + 9, b = s.find('(', a);
            if (b != std::string::npos) out.push_back({float(v[0].i), s.substr(a, b - a)});
        }
        return;
    }
    for (const Value& x : v.list) collect_events(x, out);
}

}  // namespace

bool LevelLogic::start_cinematic(int id) {
    Object* o = object(id);
    if (!o) return false;
    if (const Value* t = object_component(*o, kCompCinematicTdkr); t && t->size() >= 9) {
        const Value& c = *t;
        end_cinematic(id);
        Cinematic cin;
        cin.id = id, cin.index = o->index;
        cin.duration = c[0].as_float();
        cin.was_visible = o->visible;
        cin.origin = o->position;
        collect_events(c[6], cin.events);
        std::sort(cin.events.begin(), cin.events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const Value& track : c[7].list)
            if (track.size() >= 2)
                for (const Value& k : track[1].list)
                    if (k.size() >= 8)
                        cin.camera.push_back({k[0].as_float(), {k[2].as_float(), k[3].as_float(), k[4].as_float()},
                                              {k[5].as_float(), k[6].as_float(), k[7].as_float()}});
        std::sort(cin.camera.begin(), cin.camera.end(), [](const auto& a, const auto& b) { return a.ms < b.ms; });
        if (!o->enabled) enable(*o, true);
        for (const Value& e : c[8].list) {   // each object's first clip (the tracks hold one)
            if (e.size() < 2 || e[1].size() == 0 || e[1][0].size() < 4) continue;
            Object* obj = object(e[0].i);
            if (!obj || obj->index == std::numeric_limits<size_t>::max() || obj == o) continue;   // (the placeholder's own idle)
            if (!obj->visible) show(*obj, true);
            scene.play_clip(obj->index, e[1][0][0].s, e[1][0][3].i != 0);
        }
        if (trace) log("cinematic " + std::to_string(id) + " for " + std::to_string(int(cin.duration)) + " ms, " +
                       std::to_string(cin.events.size()) + " events, " + std::to_string(cin.camera.size()) + " camera keys");
        cinematics_.push_back(std::move(cin));
        return true;
    }
    const Value* c = object_component(*o, kCompCinematic);
    if (!c || c->size() < 1) return false;
    end_cinematic(id);
    Cinematic cin;
    cin.id = id, cin.index = o->index;
    cin.duration = (*c)[0].as_float();
    cin.was_visible = o->visible;
    if (c->size() > 11) collect_events((*c)[11], cin.events);
    std::sort(cin.events.begin(), cin.events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!o->enabled) enable(*o, true);
    else show(*o, true);
    // The clips it plays on objects ([13]: object, [[clip, ...]]), else the object's own.
    bool own = false;
    if (c->size() > 13)
        for (const Value& e : (*c)[13].list) {
            if (e.size() < 2 || e[1].size() == 0 || e[1][0].size() == 0) continue;
            Object* t = object(e[0].i);
            if (!t || t->index == std::numeric_limits<size_t>::max()) continue;
            if (!t->visible) show(*t, true);
            scene.play_clip(t->index, e[1][0][0].s, false);
            own = own || t == o;
        }
    if (!own)
        if (const Value* anim = object_component(*o, kCompAnimation); anim && anim->size() > 1 && (*anim)[1].type == Value::String)
            scene.play_clip(o->index, (*anim)[1].s, false);
    if (trace) log("cinematic " + std::to_string(id) + " for " + std::to_string(int(cin.duration)) + " ms, " +
                   std::to_string(cin.events.size()) + " events");
    cinematics_.push_back(std::move(cin));
    return true;
}

void LevelLogic::end_cinematic(int id) {
    for (auto it = cinematics_.begin(); it != cinematics_.end(); ++it) {
        if (it->id != id) continue;
        if (Object* o = object(id); o && !it->was_visible) show(*o, false);
        cinematics_.erase(it);
        runtime_.send_event("CinematicEnd_" + std::to_string(id), id);
        return;
    }
}

void LevelLogic::update_cinematics(float dt) {
    for (size_t i = 0; i < cinematics_.size();) {
        Cinematic& c = cinematics_[i];
        c.time += dt * 1000.0f;
        while (c.next < c.events.size() && c.events[c.next].first <= c.time) start_script(c.events[c.next++].second, c.id);
        if (c.time >= c.duration && !c.ending) {
            c.ending = true;   // let scripts started now see the cinematic (and its camera)
            ++i;
            continue;
        }
        if (c.ending) {
            if (trace) log("cinematic " + std::to_string(c.id) + " ended");
            end_cinematic(c.id);   // (erases it)
            continue;
        }
        ++i;
    }
}

void LevelLogic::script_camera(const glm::vec3& position, const glm::vec3& target, float ms) {
    ScriptCam& c = script_cam_;
    if (c.on && ms > 0) {   // from where it is now
        const float k = c.length > 0 ? std::min(c.t / c.length, 1.0f) : 1.0f, e = k * k * (3 - 2 * k);
        c.from_position = glm::mix(c.from_position, c.to_position, e), c.from_target = glm::mix(c.from_target, c.to_target, e);
    } else {
        c.from_position = position, c.from_target = target;
    }
    c.to_position = position, c.to_target = target;
    c.t = 0, c.length = ms / 1000.0f, c.on = true;
}

void LevelLogic::play_ambience(const std::string& preset) {
    static std::unordered_map<std::string, Ambience> presets;
    if (presets.empty()) {   // data/BATMAN_TDKR.vxa
        const auto& files = scene.vfs().find("BATMAN_TDKR.vxa");
        if (!files.empty()) {
            const Bytes b = scene.vfs().read(files[0]);
            size_t o = 0;
            auto u32 = [&]() -> uint32_t {
                if (o + 4 > b.size()) return 0;
                uint32_t v;
                std::memcpy(&v, b.data() + o, 4);
                o += 4;
                return v;
            };
            auto f32 = [&]() {
                const uint32_t u = u32();
                float f;
                std::memcpy(&f, &u, 4);
                return f;
            };
            auto str = [&]() {
                const uint32_t n = u32();
                std::string s = o + n <= b.size() ? std::string(reinterpret_cast<const char*>(b.data() + o), n) : "";
                o += n;
                u32();
                return s;
            };
            o = 4;
            o += u32();            // the version string
            o += 4, u32();         // "data", size
            for (uint32_t k = 0, n = u32(); k < n && o < b.size(); ++k) {
                Ambience a;
                const std::string name = str();
                u32();
                a.min_s = f32(), a.max_s = f32();
                f32(), f32();
                for (uint32_t s = 0, m = u32(); s < m && o < b.size(); ++s) a.sounds.push_back(str());
                presets[name] = std::move(a);
            }
        }
    }
    auto it = presets.find(preset);
    ambience = it != presets.end() ? it->second : Ambience{};
    ambience.timer = ambience.sounds.empty() ? -1 : ambience.min_s;
}

void LevelLogic::update_ambience(float dt) {
    if (!audio || ambience.timer < 0 || (ambience.timer -= dt) > 0) return;
    const std::string& s = ambience.sounds[size_t(std::rand()) % ambience.sounds.size()];
    if (audio->table().event(s)) audio->play_event(s, {});
    else audio->play(s);
    ambience.timer = ambience.min_s + (ambience.max_s - ambience.min_s) * float(std::rand()) / float(RAND_MAX);
}

namespace {
// The game's level names, in its order, and their files.
const std::pair<const char*, const char*> kLevels[] = {
    {"STR_FPS_LEVEL_GOTHAM", "GothamCity.lvc"},           {"STR_FPS_LEVEL_GOTHAM_ISLAND_2", "GothamCity_Island2.lvc"},
    {"STR_FPS_LEVEL_THEPIT", "ThePit.lvc"},               {"STR_FPS_LEVEL_STOCKEXCHANGE", "StockExchange.lvc"},
    {"STR_FPS_LEVEL_UNDERGROUND", "Underground.lvc"},     {"STR_FPS_LEVEL_BATCAVE", "BatCave.lvc"},
    {"STR_FPS_LEVEL_POLICE", "Police_Station.lvc"},       {"STR_FPS_LEVEL_MILITARY", "Military.lvc"},
    {"STR_FPS_LEVEL_STADIUM", "Stadion.lvc"},
};
}  // namespace

std::string LevelLogic::level_file(const std::string& name) {
    for (const auto& [n, f] : kLevels)
        if (name == n) return f;
    return "";
}

std::string LevelLogic::level_name(const std::string& file) {
    for (const auto& [n, f] : kLevels)
        if (to_lower(file) == to_lower(f)) return n;
    return "";
}

void LevelLogic::update_object_sounds() {
    if (!audio) return;
    for (auto it = object_sounds.begin(); it != object_sounds.end();) {
        std::erase_if(it->second, [&](int h) { return !audio->playing(h); });
        if (it->second.empty()) {
            it = object_sounds.erase(it);
            continue;
        }
        const Object* o = it->first ? object(it->first) : nullptr;
        const glm::vec3 at = it->first == 0 ? player.center : o ? o->position : glm::vec3(0);
        if (it->first == 0 || o)
            for (int h : it->second) audio->set_position(h, at);
        ++it;
    }
}

void LevelLogic::gadget_start() {
    gadget_ = {};
    gadget_.on = true;
    gadget_.eye = player.eye();
    gadget_.position = gadget_.from_position = gadget_.to_position = gadget_.eye;
    gadget_.target = gadget_.from_target = gadget_.to_target = gadget_.eye + player.view_forward();
    gray_effect(300, glm::vec3(0.35f, 1.0f, 0.45f), false);   // night vision
}

void LevelLogic::gadget_target(const glm::vec3& target, float zoom, float ms) {
    if (!gadget_.on) gadget_start();
    Gadget& g = gadget_;
    g.from_position = g.position, g.from_target = g.target;
    g.to_target = target;
    g.to_position = g.eye + (target - g.eye) * std::clamp(zoom, 0.0f, 0.95f);
    g.t = 0, g.length = std::max(ms, 1.0f) / 1000.0f;
}

void LevelLogic::gadget_stop(float ms) {
    if (!gadget_.on) return;
    gadget_.on = false;
    gray_effect(ms, glm::vec3(0.35f, 1.0f, 0.45f), true);
}

void LevelLogic::update_gadget(float dt) {
    Gadget& g = gadget_;
    if (!g.on) return;
    g.t = std::min(g.t + dt, g.length);
    const float k = g.length > 0 ? g.t / g.length : 1.0f, e = k * k * (3 - 2 * k);
    g.position = glm::mix(g.from_position, g.to_position, e);
    g.target = glm::mix(g.from_target, g.to_target, e);
}

bool LevelLogic::cinematic_view(Scene::CameraView& out) const {
    if (script_cam_.on) {
        const ScriptCam& c = script_cam_;
        const float k = c.length > 0 ? std::min(c.t / c.length, 1.0f) : 1.0f, e = k * k * (3 - 2 * k);
        out.position = glm::mix(c.from_position, c.to_position, e);
        const glm::vec3 d = glm::mix(c.from_target, c.to_target, e) - out.position;
        out.forward = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec3(0, 1, 0);
        out.up = glm::vec3(0, 0, 1);
        out.fov = glm::radians(55.0f);
        return true;
    }
    if (gadget_.on) {   // the binoculars
        out.position = gadget_.position;
        const glm::vec3 d = gadget_.target - gadget_.position;
        out.forward = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec3(0, 1, 0);
        out.up = glm::vec3(0, 0, 1);
        out.fov = glm::radians(50.0f);
        return true;
    }
    for (auto it = cinematics_.rbegin(); it != cinematics_.rend(); ++it) {   // TDKR's script cameras
        const auto& keys = it->camera;
        if (keys.empty()) continue;
        size_t k = 0;
        while (k + 1 < keys.size() && keys[k + 1].ms <= it->time) ++k;
        glm::vec3 p = keys[k].position, t = keys[k].target;
        if (k + 1 < keys.size() && it->time > keys[k].ms) {   // between two keys: linear
            const float f = std::clamp((it->time - keys[k].ms) / std::max(keys[k + 1].ms - keys[k].ms, 1.0f), 0.0f, 1.0f);
            p = glm::mix(p, keys[k + 1].position, f), t = glm::mix(t, keys[k + 1].target, f);
        }
        out.position = it->origin + p;
        const glm::vec3 d = t - p;
        out.forward = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec3(0, 1, 0);
        out.up = glm::vec3(0, 0, 1);
        out.fov = glm::radians(60.0f);
        return true;
    }
    if (camera_anim >= 0)
        if (auto it = objects_.find(camera_anim); it != objects_.end() && scene.camera_view(it->second.index, out)) return true;
    for (auto it = cinematics_.rbegin(); it != cinematics_.rend(); ++it)
        if (scene.camera_view(it->index, out)) return true;
    return false;
}

int LevelLogic::cinematic_zone() const {
    Scene::CameraView v;
    for (auto it = cinematics_.rbegin(); it != cinematics_.rend(); ++it)
        if (scene.camera_view(it->index, v)) return scene.zones().object_zone(it->index);
    // Manning a gun: the gun's zone (it turns, so its collision under the view does not count).
    if (auto it = objects_.find(player_gun_); player_gun_ >= 0 && it != objects_.end()) return scene.zones().object_zone(it->second.index);
    return ZoneSystem::kNone;
}

bool LevelLogic::player_locked() const {
    Scene::CameraView v;
    return cinematic_mode || cinematic_view(v);
}

void LevelLogic::player_look_to(const glm::vec3& at, float seconds) {
    look_at_ = at;
    look_total_ = look_left_ = std::max(seconds, 0.01f);
    look_from_yaw_ = player.yaw, look_from_pitch_ = player.pitch;
    look_aimed_ = false;
}

void LevelLogic::camera_look_to(const glm::vec3& at, float seconds) {
    if (!camera_held_) held_yaw_ = player.yaw, held_pitch_ = player.pitch;
    camera_held_ = true, restore_view_ = false;
    player_look_to(at, seconds);
}

void LevelLogic::camera_to_player(bool keep_view) {
    if (!camera_held_) return;
    camera_held_ = false, look_left_ = 0;
    restore_view_ = !keep_view;
}

std::string LevelLogic::state_clip(int type, const std::string& state) {
    std::string name = state;
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    const AnimSounds::State* st = anim_sounds_->state(type, name);
    return st ? st->clip : "";
}

std::vector<std::string> LevelLogic::state_clips(int type, const std::string& state) {
    std::string name = state;
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    const AnimSounds::State* st = anim_sounds_->state(type, name);
    return st ? st->clips : std::vector<std::string>{};
}

bool LevelLogic::look_target(float& yaw, float& pitch) {
    if (restore_view_) {
        restore_view_ = false;
        yaw = held_yaw_, pitch = held_pitch_;
        return true;
    }
    if (look_left_ <= 0 && !camera_held_) return false;
    if (!look_aimed_) {
        const glm::vec3 d = look_at_ - player.eye();
        look_to_yaw_ = glm::degrees(std::atan2(-d.x, d.y));
        look_to_pitch_ = glm::degrees(std::atan2(d.z, glm::length(glm::vec2(d))));
        look_aimed_ = true;
    }
    const float k = look_left_ > 0 ? 1 - look_left_ / look_total_ : 1;
    float dy = std::fmod(look_to_yaw_ - look_from_yaw_ + 540.0f, 360.0f) - 180.0f;
    yaw = look_from_yaw_ + dy * k;
    pitch = look_from_pitch_ + (look_to_pitch_ - look_from_pitch_) * k;
    return true;
}

void LevelLogic::start_tutorial_step(const std::string& kind, float ms) {
    tutorial_ = TutorialStep{};
    tutorial_.kind = kind;
    tutorial_.time_left = ms > 0 ? ms / 1000.0f : -1;
    tutorial_.start = player.center;
    tutorial_.last_yaw = player.yaw, tutorial_.last_pitch = player.pitch;
    if (combat) {
        tutorial_.weapon = combat->weapon_type();
        tutorial_.count = kind == "Grenade" ? combat->throws() : kind == "GrenadeSwitch" ? combat->switches()
                        : kind == "Detonate" ? combat->detonations() : 0;
    }
    tutorial_.active = true;
    if (trace) log("tutorial step " + kind);
}

void LevelLogic::update_tutorial(float dt) {
    TutorialStep& t = tutorial_;
    if (!t.active) return;
    bool done = false;
    if (t.kind == "Rotate") {   // a good look around
        float dy = std::fmod(player.yaw - t.last_yaw + 540.0f, 360.0f) - 180.0f;
        t.progress += std::abs(dy) + std::abs(player.pitch - t.last_pitch);
        t.last_yaw = player.yaw, t.last_pitch = player.pitch;
        done = t.progress > 90;
    } else if (t.kind == "Move") {
        done = glm::distance(glm::vec2(player.center), glm::vec2(t.start)) > 3;
    } else if (t.kind == "Jump") {
        done = !player.on_ground && player.vertical_speed > 1;
    } else if (t.kind == "Reload") {
        done = combat && combat->reloading();
    } else if (t.kind == "Shoot") {
        done = combat && combat->since_shot() < 0.1f;
    } else if (t.kind == "ChangeWeapon") {
        done = combat && combat->weapon_type() != t.weapon;
    } else if (t.kind == "Grenade") {   // a grenade (or mine) thrown
        done = !combat || combat->throws() != t.count;
    } else if (t.kind == "GrenadeSwitch") {   // grenades and mines swapped
        done = !combat || !combat->mines_enabled || combat->switches() != t.count;
    } else if (t.kind == "Detonate") {   // the mines set off
        done = !combat || !combat->mines_placed() || combat->detonations() != t.count;
    }
    // Steps for things this port does not have yet (sprint, aiming, grenades, powers)
    // end with their time (or at once without one).
    else if (t.time_left < 0) {
        done = true;
    }
    if (t.time_left >= 0 && (t.time_left -= dt) <= 0) done = true;
    if (!done) return;
    t.active = false;
    if (trace) log("tutorial step " + t.kind + " done");
    runtime_.send_event("TutorialStepEnded");
}

void LevelLogic::hide_message(const std::string& id) {   // "" hides them all (HideMessage)
    for (Message& m : messages_)
        if (id.empty() || m.id == id) m.seconds_left = 0;
}

void LevelLogic::teleport_object(Object& o, const glm::vec3& position) {
    o.position = position;
    if (combat && combat->place_actor(o.id, position)) return;
    if (o.index != std::numeric_limits<size_t>::max()) scene.set_object_pose(o.index, position, o.yaw);
}

void LevelLogic::mover_state(Mover& m, const std::string& state) {
    const AnimSounds::State* st = anim_sounds_->state(m.type, state);
    if (!st) return;
    if (!st->clip.empty()) scene.play_clip(m.index, st->clip, state == "Move" || state == "Idle");
    if (!audio) return;
    for (int h : anim_sounds_->play_state(*audio, m.type, state, m.position)) m.sounds.push_back(h);
}

bool LevelLogic::motion_start(int id) {
    for (Mover& m : movers_) {
        if (m.id != id) continue;
        if (m.moving) return true;
        // The route: the path's waypoint chain, from where the object is if it is not at
        // the start (CMotionOnPathComponent::MotionStart).
        m.points.clear(), m.point_ids.clear();
        auto first = path_first_.find(m.path);
        std::unordered_set<int> seen;
        for (int w = first != path_first_.end() ? first->second : -1; w >= 0 && seen.insert(w).second;) {
            const Object* wp = object(w);
            if (!wp) break;
            m.points.push_back(wp->position), m.point_ids.push_back(w);
            auto next = waypoint_next_.find(w);
            w = next == waypoint_next_.end() ? -1 : next->second;
        }
        if (m.points.empty()) return false;
        if (glm::distance(m.points.front(), m.position) > 0.5f) m.points.insert(m.points.begin(), m.position), m.point_ids.insert(m.point_ids.begin(), -1);
        if (m.points.size() < 2) return false;
        m.segment = 0, m.along = 0, m.speed = 0, m.moving = true, m.stop_at = -1;
        if (trace) log("motion start " + std::to_string(id));
        mover_state(m, "MoveStart");
        return true;
    }
    return false;
}

void LevelLogic::motion_stop(int id, int at_waypoint) {
    for (Mover& m : movers_) {
        if (m.id != id || !m.moving) continue;
        if (at_waypoint >= 0) m.stop_at = at_waypoint;
        else m.speed = 0, m.moving = false;
    }
}

void LevelLogic::motion_max_speed(int id, float speed) {
    for (Mover& m : movers_)
        if (m.id == id && speed > 0) m.max_speed = speed;
}

void LevelLogic::update_movers(float dt) {
    for (Mover& m : movers_) {
        if (!m.moving) continue;
        // Distance left to the end (or the waypoint to stop at).
        float left = glm::distance(m.points[m.segment], m.points[m.segment + 1]) - m.along;
        for (size_t s = m.segment + 1; s + 1 < m.points.size() && m.point_ids[s] != m.stop_at; ++s)
            left += glm::distance(m.points[s], m.points[s + 1]);
        // UpdateAccelBrake: accelerate to the maximum, brake when the stopping distance
        // reaches what is left.
        const Mover* lead = m.follow >= 0 ? mover(m.follow) : nullptr;
        if (m.speed * m.speed * 0.5f / std::max(m.brake, 0.01f) >= left) {
            m.speed = std::max(m.speed - m.brake * dt, 0.5f);
        } else if (lead && lead->moving) {
            // Following (CMotionOnPathComponent::Update): behind by more than the distance,
            // speed up; ahead by more, brake; within it, take the leader's speed.
            const float d = glm::dot(lead->position - m.position, lead->direction + m.direction) * 0.5f;
            if (d > m.follow_distance) m.speed = std::min(m.speed + m.accel * dt, m.max_speed);
            else if (d < -m.follow_distance) m.speed = std::max(m.speed - m.brake * dt, 0.0f);
            else if (m.speed < lead->speed) m.speed = std::min(m.speed + m.accel * dt, lead->speed);
            else m.speed = std::max(m.speed - m.brake * dt, lead->speed);
        } else {
            m.speed = std::min(m.speed + m.accel * dt, m.max_speed);
        }
        float step = std::min(m.speed * dt, left);
        bool arrived = left <= 1e-3f;
        while (step > 0 && !arrived) {
            const float len = glm::distance(m.points[m.segment], m.points[m.segment + 1]);
            if (m.along + step < len) {
                m.along += step;
                break;
            }
            step -= len - m.along;
            m.along = 0;
            ++m.segment;
            if (m.segment + 1 >= m.points.size() || m.point_ids[m.segment] == m.stop_at) arrived = true;
        }
        if (m.segment + 1 >= m.points.size()) m.segment = m.points.size() - 2, m.along = glm::distance(m.points[m.segment], m.points[m.segment + 1]);
        const glm::vec3 a = m.points[m.segment], b = m.points[m.segment + 1];
        const float len = glm::distance(a, b);
        const glm::vec3 pos = len > 0 ? a + (b - a) * (m.along / len) : b;
        const glm::vec3 delta = pos - m.position;
        if (len > 0) m.direction = (b - a) / len;
        // Carry the player standing on it (the floor under the player is its collision).
        float floor_z;
        uint32_t owner = 0;
        const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z - 0.05f);
        const bool riding = scene.collision().ray_down(feet + glm::vec3(0, 0, 0.3f), 0.8f, floor_z, &owner) && owner == m.index;
        m.position = pos;
        scene.move_object(m.index, pos);
        if (riding) player.center += delta;
        if (audio)
            for (int h : m.sounds) audio->set_position(h, pos);
        if (arrived) {
            m.moving = false, m.speed = 0;
            if (audio)
                for (int h : m.sounds) audio->stop(h, 300);
            m.sounds.clear();
            mover_state(m, "MoveStopAt");
            m.sounds.clear();
            if (trace) log("motion end " + std::to_string(m.id));
        }
    }
}

void LevelLogic::dialog(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) dialog_.push_back(l);
    dialog_active_ = true;
}

void LevelLogic::update_dialog(float dt) {
    if (!dialog_active_) return;
    if (dialog_voice_ >= 0 && audio && audio->playing(dialog_voice_)) {
        // The line's subtitle stays while its voice lasts.
        if (!messages_.empty() && messages_.back().id == dialog_line_) messages_.back().seconds_left = std::max(messages_.back().seconds_left, 0.5f);
        return;
    }
    dialog_voice_ = -1;
    if ((dialog_wait_ -= dt) > 0) return;
    if (dialog_.empty()) {
        dialog_active_ = false;
        runtime_.send_event("MessageEnd");
        return;
    }
    const std::string line = dialog_.front();
    dialog_.pop_front();
    dialog_line_ = line;
    dialog_voice_ = audio ? audio->play(line) : -1;
    // Without a voice the line stays up for a few seconds; after one, a short pause.
    dialog_wait_ = dialog_voice_ >= 0 ? 0.3f : 3.0f;
    // Lines with text show it while the voice plays; voice-only barks (VFX_...) show nothing.
    if (!strings.find(line).empty()) message(line, dialog_voice_ >= 0 ? 4.0f : 3.0f);
}

void LevelLogic::update_jetpack_sound() {
    if (!audio) return;
    if (player.jetpack) {
        if (jetpack_sound_ < 0 || !audio->playing(jetpack_sound_)) jetpack_sound_ = audio->play("sfx_jetpack");   // "initial"
        else audio->set_state(jetpack_sound_, player.thrusting ? "ride" : "idle");
    } else if (jetpack_sound_ >= 0) {
        audio->set_state(jetpack_sound_, "stop");   // plays its tail out
        jetpack_sound_ = -1;
    }
}

void LevelLogic::update_breathing(float dt) {
    if (!audio || !combat) return;
    if (player_locked() || combat->player_dead || player_vehicle_ >= 0 || player_mech_ >= 0) {   // PlayerComponent::Enable(false)
        if (breath_sound_ >= 0) audio->stop(breath_sound_, 500), breath_sound_ = -1;
        return;
    }
    if (breath_sound_ < 0 || !audio->playing(breath_sound_)) {
        breath_sound_ = audio->play("kal_breathing_system");   // its first state: breath_normal
        breath_state_ = "breath_normal", breath_timer_ = breath_recovery_ = 0;
        breath_damage_ = combat->stats().damage_taken, breath_rushing_ = player.rushing;
    }
    auto set = [&](const char* state) {
        if (breath_state_ == state) return;
        breath_state_ = state;
        audio->set_state(breath_sound_, state);
        if (breath_state_ == "breath_battle") breath_timer_ = 5;
    };
    // CWeaponComponent::DoFire, PlayerComponent::ReceiveDamage, Start/EndRush.
    if (combat->since_shot() < dt || combat->stats().damage_taken > breath_damage_) set("breath_battle");
    breath_damage_ = combat->stats().damage_taken;
    if (player.rushing && !breath_rushing_) set("breath_running");
    if (!player.rushing && breath_rushing_) set("breath_recovery"), breath_recovery_ = 4;
    breath_rushing_ = player.rushing;
    if (breath_recovery_ > 0 && (breath_recovery_ -= dt) <= 0 && breath_state_ == "breath_recovery") set("breath_normal");
    // PlayerComponent::Update.
    if (combat->player_health < combat->player_max_health * 0.2f) set("breath_dying");
    else if (breath_timer_ <= 0) {
        if (breath_state_ == "breath_dying") set("breath_normal");
    } else if ((breath_timer_ -= dt) <= 0) {
        set("breath_normal");
    }
}

void LevelLogic::update_music(float dt) {
    if (!audio || music_event_.empty()) return;
    if (audio->event_playing("env_m_action")) {   // scripted action music has the floor
        if (music_ >= 0) audio->stop(music_, 3000), music_ = -1;
        music_wait_ = 4;
        return;
    }
    if (music_ >= 0 && audio->playing(music_)) return;
    if (music_ >= 0) {   // a track ended: a pause before the next
        music_ = -1;
        music_wait_ = 12 + float(std::rand()) / float(RAND_MAX) * 10;
    }
    if ((music_wait_ -= dt) > 0) return;
    Audio::Params p;
    p.fade_in_ms = 2000;
    music_ = audio->play_event(music_event_, p);
    if (music_ < 0) music_wait_ = 30;
}

const std::array<uint8_t, 1024>* LevelLogic::lut(const std::string& name) {
    std::string base = name;
    for (char& c : base) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (const size_t dot = base.rfind('.'); dot != std::string::npos) base.resize(dot);
    if (auto it = luts_.find(base); it != luts_.end()) return &it->second;
    // lut3/<name>.pvr: PVR v2 (52-byte header: height at 4, width at 8), RGBA 8888. The
    // LUTs are per-channel curves: red and blue along x, green along y.
    if (!scene.vfs().exists(base + ".pvr")) {
        log("colour grading: no LUT " + name);
        return nullptr;
    }
    const Bytes d = scene.vfs().read(base + ".pvr");
    auto u32 = [&](size_t p) { return p + 4 <= d.size() ? uint32_t(d[p] | d[p + 1] << 8 | d[p + 2] << 16 | uint32_t(d[p + 3]) << 24) : 0u; };
    const uint32_t header = u32(0), h = u32(4), w = u32(8);
    if (w == 0 || h == 0 || d.size() < header + size_t(w) * h * 4) return nullptr;
    auto texel = [&](uint32_t x, uint32_t y, int c) { return float(d[header + (size_t(y) * w + x) * 4 + size_t(c)]); };
    // Linear filtering at the colour itself as the coordinate (clamped to the edge texels).
    auto sample = [&](float u, uint32_t n, auto&& at) {
        const float p = std::clamp(u * float(n) - 0.5f, 0.0f, float(n - 1));
        const uint32_t i = uint32_t(p), j = std::min(i + 1, n - 1);
        return at(i) + (at(j) - at(i)) * (p - float(i));
    };
    std::array<uint8_t, 1024>& out = luts_[base];
    for (int i = 0; i < 256; ++i) {
        const float u = float(i) / 255.0f;
        out[size_t(i) * 4 + 0] = uint8_t(std::lround(sample(u, w, [&](uint32_t x) { return texel(x, 0, 0); })));
        out[size_t(i) * 4 + 1] = uint8_t(std::lround(sample(u, h, [&](uint32_t y) { return texel(0, y, 1); })));
        out[size_t(i) * 4 + 2] = uint8_t(std::lround(sample(u, w, [&](uint32_t x) { return texel(x, 0, 2); })));
        out[size_t(i) * 4 + 3] = 255;
    }
    return &out;
}

void LevelLogic::set_color_grading(const std::string& name, float ms) {
    // CLevel::ChangeColorGrading: with a time and grading on, blend from the current LUT.
    if (ms > 0 && grading_.on) grading_.previous = grading_.current, grading_.total = grading_.left = ms / 1000.0f;
    else grading_.left = grading_.total = 0;
    grading_.current = name;
    grading_.on = true;
    grading_.dirty = true;
}

void LevelLogic::update_grading(float dt) {
    if (grading_.left > 0) grading_.left = std::max(grading_.left - dt, 0.0f), grading_.dirty = true;
    if (!grading_.dirty) return;
    grading_.dirty = false;
    const std::array<uint8_t, 1024>* now = grading_.on ? lut(grading_.current) : nullptr;
    post.grading = now != nullptr;
    if (!now) return;
    // Blend2: mix(new, old, factor), the factor going from 1 to 0 over the time.
    const std::array<uint8_t, 1024>* old = grading_.left > 0 ? lut(grading_.previous) : nullptr;
    const float f = old ? grading_.left / grading_.total : 0.0f;
    for (size_t i = 0; i < 1024; ++i)
        post.grading_curve[i] = old ? uint8_t(std::lround(float((*now)[i]) + (float((*old)[i]) - float((*now)[i])) * f)) : (*now)[i];
    ++post.grading_version;
}

void LevelLogic::advance_real_time(float dt) {
    // CSlowMotion::Update.
    if (slow_.t >= 0) {
        Slow& s = slow_;
        s.t += dt * 1000.0f;
        if (s.t < s.in) s.factor = s.from + (s.to - s.from) * s.t / s.in;
        else if (s.t < s.in + s.hold) s.factor = s.to;
        else if (s.out > 0 && s.t < s.in + s.hold + s.out) s.factor = s.to + (1 - s.to) * (s.t - s.in - s.hold) / s.out;
        else s.factor = s.to == 1 || s.out > 0 || s.hold > 0 ? 1.0f : s.to, s.t = -1;
    }
    // CQTEManager::Update: each ramp moves the factor towards its target over its game
    // time; while one runs the factor holds.
    float step = dt * 1000.0f * time_scale();
    if (qte_.left <= 0 && !qte_.ramps.empty()) {
        qte_.target = qte_.ramps.front().first, qte_.left = qte_.ramps.front().second;
        qte_.ramps.pop_front();
    }
    if (qte_.left > 0) {
        if (step <= 0 && qte_.target == 1) step = 1;   // frozen: the way back still runs
        time_factor_ += std::clamp(step / qte_.left, 0.0f, 1.0f) * (qte_.target - time_factor_);
        time_factor_left_ = 1e8f;
        qte_.left -= step;
    } else {
        time_factor_left_ = std::max(time_factor_left_ - dt, 0.0f);
    }
    if (!qte_.kind.empty()) {
        qte_.time += dt;
        if (auto_qte && qte_.time > 0.5f) qte_press(qte_.kind);
    }
}

void LevelLogic::qte_start(const std::string& kind) {
    std::string k = kind;
    for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (k != "fire" && k != "repel" && k != "move") return;
    qte_.kind = k, qte_.time = 0;
}

void LevelLogic::qte_stop() {
    qte_.kind.clear();
    qte_.ramps.clear();
    qte_.left = 0;
}

bool LevelLogic::qte_press(const std::string& kind) {
    if (qte_.kind.empty() || kind != qte_.kind) return false;
    // Done: the ramps give way to one back to normal speed.
    qte_.kind.clear();
    qte_.ramps.clear();
    qte_.ramps.push_back({1.0f, 1.0f});
    qte_.left = 0;
    return true;
}

void LevelLogic::set_primary_objective(const std::string& id) {
    if (audio) audio->play("sfx_menu_objectives_pop_up");
    const std::string t = strings.find(id);
    objectives.primary = t.empty() ? id : t;
    objectives.primary_done = false;
    objectives.secondary.clear();
    objectives.show = 5;
}

void LevelLogic::add_secondary_objective(const std::string& id) {
    if (audio) audio->play("sfx_menu_objectives_pop_up");
    const std::string t = strings.find(id);
    objectives.secondary.push_back({id, t.empty() ? id : t, false});
    objectives.show = 5;
}

void LevelLogic::secondary_objective_complete(const std::string& id) {
    if (audio) audio->play("sfx_menu_objective_completed");
    for (auto& s : objectives.secondary)
        if (s.id == id) s.done = true, objectives.show = 5;
    if (!objectives.secondary.empty() &&
        std::all_of(objectives.secondary.begin(), objectives.secondary.end(), [](const auto& s) { return s.done; }))
        primary_objective_complete();
}

void LevelLogic::primary_objective_complete() {
    if (audio) audio->play("sfx_menu_objective_completed");
    objectives.primary_done = true;
    objectives.show = 5;
}

void LevelLogic::screen_effect(ScreenEffect kind, float ms) {
    post.sepia = -1, post.levels_white = 1, post.flash_threshold = -2, post.gray = -1;   // CLevel::StopPostEffect
    screen_effect_.kind = kind, screen_effect_.time = 0, screen_effect_.length = std::max(ms, 1.0f) / 1000.0f;
    screen_effect_.back = false;
    update_screen_effect(0);
}

void LevelLogic::gray_effect(float ms, const glm::vec3& tint, bool back) {
    screen_effect(ScreenEffect::Gray, ms <= 0 ? 1000 : ms);
    screen_effect_.back = back;
    post.gray_tint = tint;
    update_screen_effect(0);
}

void LevelLogic::update_screen_effect(float dt) {
    auto& e = screen_effect_;
    if (e.kind == ScreenEffect::None) return;
    e.time += dt;
    const float k = e.time / e.length;
    switch (e.kind) {
        case ScreenEffect::Sepia: post.sepia = std::clamp(k, 0.0f, 1.0f); break;   // held once full
        case ScreenEffect::Levels:
            post.levels_white = std::clamp(0.1f + k * 0.9f, 0.1f, 1.0f);
            if (k >= 1) e.kind = ScreenEffect::None, post.levels_white = 1;
            break;
        case ScreenEffect::Gray:   // CLevel::UpdatePostEffect_Gray: the colour left, held at the end
            post.gray = std::clamp(e.back ? k : 1 - k, 0.0f, 1.0f);
            break;
        case ScreenEffect::Flash:
            post.flash_threshold = 2 * k - 1;
            if (k >= 1) e.kind = ScreenEffect::None, post.flash_threshold = -2;
            break;
        default: break;
    }
}

void LevelLogic::update_motion_blur(float dt) {
    const float dyaw = std::abs(std::remainder(player.yaw - blur_yaw_, 360.0f)), dpitch = std::abs(player.pitch - blur_pitch_);
    blur_yaw_ = player.yaw, blur_pitch_ = player.pitch;
    post.radial_blur = 0;
    if (!motion_blur || dt <= 0) return;
    // The original measures a frame at its 30 fps.
    const float k = (1.0f / 30.0f) / dt, turn_yaw = dyaw * k, turn_pitch = dpitch * k;
    if (turn_yaw > 8 || turn_pitch > 8) post.radial_blur = std::min((turn_yaw + turn_pitch) * 0.001f, 1.0f);
}

void LevelLogic::update_heat_haze(float dt) {
    HeatHaze& h = heat_;
    const float ms = dt * 1000.0f;
    if (h.stopping) h.now = std::max(h.now - ms, 0.0f);
    else h.now = std::min(h.now + ms, h.max);
    post.heat_haze = h.now > 0 && h.max > 0;
    if (!post.heat_haze) return;
    // Facing the haze's source (0 behind .. 1 straight at it) deepens the tint.
    float strength = 0.84375f, tint = 0.415f;
    if (const Object* o = h.object >= 0 ? object(h.object) : nullptr) {
        const glm::vec3 to = o->position - player.center;
        const float facing = glm::length(to) > 1e-3f ? (glm::dot(glm::normalize(to), player.view_forward()) + 1) * 0.5f : 0.5f;
        tint = 0.332f + facing * 0.15f, strength = facing + 0.3f;
    }
    const float k = h.now / h.max;
    post.heat_strength = k * strength;
    post.heat_overbright = 1 + k * 0.68421f;
    post.heat_blend = 1 - k * tint;
    post.heat_time += dt;
    if (!post.heat_texture) post.heat_texture = scene.texture("PE_HeatDistort");
}

void LevelLogic::update(float dt) {
    play_time += dt;
    frame_ms = dt * 1000.0f;
    if (dt > 0) player_speed = glm::distance(glm::vec2(player.center), glm::vec2(last_player_)) / dt;
    last_player_ = player.center;
    for (ParamRamp& r : param_ramps_) {
        r.elapsed = std::min(r.elapsed + dt, r.seconds);
        scene.set_object_param(r.object, r.param, glm::vec4(r.from + (r.to - r.from) * r.elapsed / r.seconds), 1);
    }
    std::erase_if(param_ramps_, [](const ParamRamp& r) { return r.elapsed >= r.seconds; });
    update_grading(dt);
    update_heat_haze(dt);
    update_screen_effect(dt);
    update_vehicles(dt);
    if (timer >= 0 && !timer_paused) {   // ILevelInterface::UpdateTimer
        const int before = int(timer);
        timer -= dt;
        if (timer >= 0 && int(timer) != before && audio) audio->play("sfx_ui_time_counter");
    }
    fov_t_ += dt;
    objectives.show = std::max(objectives.show - dt, 0.0f);
    objectives.alpha = std::clamp(objectives.alpha + (objectives.show > 0 ? 2.0f : -2.0f) * dt, 0.0f, 1.0f);   // 0.002 per ms
    update_music(dt);
    update_jetpack_sound();
    update_breathing(dt);
    update_dialog(dt);
    update_movers(dt);
    update_elevators(dt);
    update_links();
    update_player_effects();
    update_limited(dt);
    update_cinematics(dt);
    update_tutorial(dt);
    look_left_ = std::max(look_left_ - dt, 0.0f);
    if (fading_) {
        fade_left_ = std::max(fade_left_ - dt, 0.0f);
        if (fade_left_ <= 0 && fade_in_) fading_ = false;   // faded out stays covered
    }
    shake_clock_ += dt;
    shake_left_ = std::max(shake_left_ - dt, 0.0f);
    // Clip sounds and effects (doors and movers play their states' sounds as they enter them).
    for (const auto& [id, o] : objects_)
        if (o.index < scene.level().objects.size() && o.visible && o.door < 0 &&
            std::none_of(movers_.begin(), movers_.end(), [&](const Mover& m) { return m.index == o.index; }))
            anim_sounds_->update(scene, audio, o.index, scene.level().objects[o.index].type);
    // PostEffects::Update: variables move linearly to their targets.
    for (PostFade& f : post_fades_) {
        f.elapsed += dt;
        const float t = std::min(f.elapsed / f.duration, 1.0f);
        *f.value = f.from + (f.to - f.from) * t;
    }
    post_fades_.erase(std::remove_if(post_fades_.begin(), post_fades_.end(), [](const PostFade& f) { return f.elapsed >= f.duration; }),
                      post_fades_.end());
    for (auto it = delayed_.begin(); it != delayed_.end();) {
        it->seconds -= dt;
        if (it->seconds <= 0) {
            runtime_.send_event(it->event, it->id);
            it = delayed_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& m : messages_) m.seconds_left -= dt;
    while (!messages_.empty() && messages_.front().seconds_left <= 0) messages_.pop_front();
    quests.update(dt);
    update_object_sounds();
    update_ambience(dt);
    update_gadget(dt);
    update_button_qte(dt);
    if (script_cam_.on) script_cam_.t += dt;
    update_walkers(dt);
    update_cameras(dt);
    update_held_use(dt);
    update_triggers();
    update_doors(dt);
    update_zone_fog();
    update_fog(dt);
    runtime_.update(dt * 1000.0f);
}


void LevelLogic::end_hack(bool won) {
    if (!hack.active) return;
    hack.active = false;
    hud_state = "HUD";
    enable_controls(true);   // CLevel::EnableControls
    if (trace) log(std::string("hack: ") + (won ? "won" : "lost"));
    const std::string& fn = won ? hack.win : hack.lose;
    if (!fn.empty()) start_script(fn, -1);
}

bool LevelLogic::is_helicopter(int id) const {
    const auto it = objects_.find(id);
    return it != objects_.end() && object_component(it->second, kCompHelicopter) != nullptr;
}

void LevelLogic::grab_helicopter(int id) {
    Object* o = object(id);
    if (!o || o->dead || hanging_from == id) return;
    hanging_from = id;
    if (trace) log("hangs from helicopter " + std::to_string(id));
    ai_event(id, "On_Actor_Attached");
}

void LevelLogic::ai_event(int id, const std::string& event) {
    const Object* o = object(id);
    const Value* ai = o ? object_component(*o, kCompNpcAi) : nullptr;
    if (!ai || ai->size() < 1) return;
    for (const auto& [type, g] : gol)
        if (g.name == "AIController") start_script("Templ" + std::to_string(type) + "_" + std::to_string((*ai)[0].i) + "_" + event, id);
}

void LevelLogic::mount_batpod(int id) {
    Object* o = object(id);
    if (!o || riding == id) return;
    if (riding >= 0) dismount_batpod();
    riding = id;
    enable(*o, true);
    o->interact_enabled = false;
    if (o->index < scene.level().objects.size()) scene.set_object_collision(o->index, false);
    if (trace) log("on the Batpod " + std::to_string(id));
}

void LevelLogic::dismount_batpod() {
    Object* o = object(riding);
    riding = -1;
    if (!o) return;
    // Off to its left, standing (CBatpodHandleComponent::ExitVehicle).
    const float y = glm::radians(o->yaw);
    const glm::vec3 left(-std::cos(y), -std::sin(y), 0);
    player.center = o->position + left * 1.2f + glm::vec3(0, 0, player.radius.z + 0.1f);
    o->interact_enabled = true;
    if (o->index < scene.level().objects.size()) scene.set_object_collision(o->index, true);
    if (trace) log("off the Batpod " + std::to_string(o->id));
}

bool LevelLogic::press_button_qte() {
    if (button_qte.object < 0) return false;
    const ButtonQte q = button_qte;
    button_qte = {};
    if (trace) log("qte: pressed at " + std::to_string(q.object));
    if (audio) audio->play("sfx_qte_success");
    if (!q.win.empty()) start_script(q.win, -1);
    // PlayerComponent::StartScriptedMinigame: the pull (PullableObjectComponent: idle, struggle,
    // fall and back clips, then the struggle's length in ms).
    Object* o = minigame_object == q.object ? object(q.object) : nullptr;
    if (!o || o->index >= scene.level().objects.size()) return true;
    auto c = scene.level().objects[o->index].components.find(kCompPullable);
    if (c == scene.level().objects[o->index].components.end() || c->second.size() < 5) return true;
    const Value& v = c->second;
    scene.play_clip(o->index, v[1].s, true);
    pull_ = {o->id, std::max(v[4].as_float(), 0.0f) / 1000.0f, v[2].s};
    minigame_object = -1;
    return true;
}

void LevelLogic::update_button_qte(float dt) {
    if (button_qte.object >= 0) {
        button_qte.left -= dt;
        if (button_qte.left <= 0) {   // missed
            const std::string lose = button_qte.lose;
            if (trace) log("qte: missed at " + std::to_string(button_qte.object));
            button_qte = {};
            if (audio) audio->play("sfx_qte_fail");
            if (!lose.empty()) start_script(lose, -1);
        }
    }
    if (pull_.object >= 0 && (pull_.left -= dt) <= 0) {   // down it comes: its fall, its script
        if (Object* o = object(pull_.object)) {
            if (o->index < scene.level().objects.size()) scene.play_clip(o->index, pull_.fall, false);
            start_object_script(*o, "OnScriptEvent");
        }
        pull_ = {};
    }
}

std::string LevelLogic::default_state(const Object& o, size_t which) const {
    const Value* st = object_component(o, 0x5441A117);   // CStateSetComponent
    return st && st->size() > which && (*st)[which].type == Value::String ? (*st)[which].s : std::string();
}

bool LevelLogic::destroyable(int id) const {
    const Object* o = object(id);
    return o && !o->dead && o->enabled && o->visible && object_component(*o, kCompDestroyable);
}

void LevelLogic::destroy(int id) {
    Object* obj = object(id);
    const Value* d = obj ? object_component(*obj, kCompDestroyable) : nullptr;
    if (!d || obj->dead) return;
    Object& o = *obj;
    o.dead = true, o.health = 0;
    if (d->size() > 1 && !(*d)[1].s.empty() && o.index < scene.level().objects.size()) scene.play_clip(o.index, (*d)[1].s, false);
    if (d->size() > 4 && !(*d)[4].s.empty() && audio) {
        Audio::Params p;
        p.position = &o.position;
        if (audio->play((*d)[4].s, p) < 0) audio->play_event((*d)[4].s, p);
    }
    if (d->size() > 5 && !(*d)[5].s.empty() && o.index < scene.level().objects.size())
        scene.particles().start((*d)[5].s, scene.object_matrix(o.index));
    std::erase_if(objective_markers, [&](const ObjectiveMarker& m) { return m.id == o.id; });
    if (trace) log("destroyed " + std::to_string(o.id));
    start_object_script(o, "OnScriptEvent");
}

bool LevelLogic::is_enemy(int id) const {
    const Object* o = object(id);
    if (!o || o->dead || !o->enabled || !o->visible || o->index >= scene.level().objects.size()) return false;
    if (o->enemy >= 0) return o->enemy != 0;   // NPCSetEnemyFlag
    if (auto it = attack_targets.find(id); it != attack_targets.end() && it->second == 0) return true;   // fighting Batman
    // CNpcAIComponent: [0] its behaviour (AI_BEHAVIOR_*), [1] whether it is an enemy (the
    // mercenaries, snipers and thugs; not the civilians or the police).
    const LevelObject& lo = scene.level().objects[o->index];
    const Value* ai = nullptr;
    if (auto it = lo.components.find(kCompNpcAi); it != lo.components.end()) ai = &it->second;
    else if (auto g = gol.find(lo.type); g != gol.end()) ai = resolved_template(g->second, gol, kCompNpcAi);
    return ai && ai->size() > 1 && (*ai)[1].i != 0;
}

}  // namespace tdkr
