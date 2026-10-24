// TDKR's own script functions (libKRHP.so's RegisterScriptFunctions), on top of those
// N.O.V.A. 3 shares (game_api.cpp). Behaviour follows the decompiled natives; where the game
// does more than this port can yet (NPC AI, the HUD's per-object buttons), the note says so.
//
// The scripts' id for the player is 0 (TeleportPlayerToZone, ActorIsInStateType(0, ...)).
#include <algorithm>
#include <cmath>
#include <random>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "audio/audio.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace tdkr {

namespace {

LevelLogic& ctx(lua_State* L) { return *static_cast<LevelLogic*>(lua_touserdata(L, lua_upvalueindex(1))); }
int int_arg(lua_State* L, int i) { return int(lua_tointeger(L, i)); }
std::string str_arg(lua_State* L, int i) { return lua_isstring(L, i) ? lua_tostring(L, i) : ""; }
bool bool_arg(lua_State* L, int i) { return lua_toboolean(L, i) != 0; }
constexpr size_t kNoIndex = std::numeric_limits<size_t>::max();
constexpr int kCompInteraction = 0x34747EBE;   // CInteractionComponent
constexpr int kCompNpcAi = 0x0113F791;         // CNpcAIComponent (what makes an actor)

// CLevel::FindObjectOrWaypoint's position: the player's feet for 0.
bool position_of(LevelLogic& c, int id, glm::vec3& out) {
    if (id == 0) {
        out = c.player.center - glm::vec3(0, 0, c.player.radius.z);
        return true;
    }
    const LevelLogic::Object* o = c.object(id);
    if (!o) return false;
    out = o->position;
    return true;
}

bool find_state(const Gol& gol, int type, const std::string& name, ActorState& out) { return find_actor_state(gol, type, name, out); }
int object_type(LevelLogic& c, const LevelLogic::Object& o) { return c.object_type(o); }

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

// --- quests (CQuestManager::SetQuestStatus) ------------------------------------------------

template <int Status>
int api_set_quest(lua_State* L) {
    ctx(L).quests.set_status(int_arg(L, 1), Status);
    return 0;
}
int api_get_quest_status(lua_State* L) {
    lua_pushinteger(L, ctx(L).quests.status(int_arg(L, 1)));
    return 1;
}
// EnableMainStory(): the main story runs from its current quest (the first at the start).
// IsQuestDisabled / Enabled / Running / Completed / Failed: the quest's status is one of them.
template <int A, int B = -1, int C = -1>
int api_is_quest(lua_State* L) {
    const int st = ctx(L).quests.status(int_arg(L, 1));
    lua_pushboolean(L, st == A || st == B || st == C);
    return 1;
}
// GetQuestsWithStatus("enabled" | "active" | "completed" | "failed" | "canceled"): the quest objects
// in that status (2, 3, 7, 4, 9), as values.
int api_get_quests_with_status(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string name = str_arg(L, 1);
    const int status = name == "enabled" ? Quests::Initialising : name == "active" ? Quests::Running : name == "completed" ? Quests::Completed
                     : name == "failed" ? Quests::Failed : name == "canceled" ? Quests::Canceled : Quests::Disabled;
    int n = 0;
    for (const Quests::Quest& q : c.quests.all())
        if (q.status == status) lua_pushinteger(L, q.object), ++n;
    return n;
}
// RequireLoadLevel(level name[, spawn]): change level (the quests carry over).
int api_require_load_level(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string file = LevelLogic::level_file(str_arg(L, 1));
    if (file.empty()) {
        c.log("RequireLoadLevel: no level " + str_arg(L, 1));
        return 0;
    }
    c.level_request = {file, lua_gettop(L) >= 2 ? int_arg(L, 2) : -1};
    return 0;
}
// GetCurrentLevel(level name): whether that is the level being played.
int api_get_current_level(lua_State* L) {
    LevelLogic& c = ctx(L);
    lua_pushboolean(L, LevelLogic::level_name(c.scene.level_name()) == str_arg(L, 1));
    return 1;
}
int api_add_map_icon(lua_State* L) {   // AddGenericObjectOnMap(object, icon, ...)
    ctx(L).map_icons[int_arg(L, 1)] = str_arg(L, 2);
    return 0;
}
int api_remove_map_icon(lua_State* L) {   // RemoveGenericObjectFromMap(object)
    ctx(L).map_icons.erase(int_arg(L, 1));
    return 0;
}
int api_enable_main_story(lua_State* L) {
    ctx(L).quests.start_story();
    return 0;
}
int api_start_main_story_from_quest(lua_State* L) {
    ctx(L).quests.start_story(int_arg(L, 1));
    return 0;
}
int api_current_main_story_quest(lua_State* L) {
    lua_pushinteger(L, ctx(L).quests.story_quest());
    return 1;
}
int api_next_mission(lua_State* L) {
    ctx(L).next_mission(int_arg(L, 1));
    return 0;
}
// UnlockMission(chapter, mission) / RequireLoadChapterMission(chapter, mission): the
// chapter list's progress and a level change; logged for now.
int api_log_mission(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.log(std::string(lua_tostring(L, lua_upvalueindex(2))) + "(" + std::to_string(int_arg(L, 1)) + ", " +
          std::to_string(int_arg(L, 2)) + ")");
    return 0;
}

// --- objectives (CHUDDisplay::AddObjectiveToScene "ObjectiveBtn") -------------------------

// ObjectiveAdd(object, kind): the objective marker over a living object ("objective",
// "kill", "sideObjective", ...).
int api_objective_add(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1);
    const LevelLogic::Object* o = c.object(id);
    if (!o || o->dead) return 0;
    std::string kind = lua_isstring(L, 2) ? lua_tostring(L, 2) : "none";
    for (LevelLogic::ObjectiveMarker& m : c.objective_markers)
        if (m.id == id) {
            m.text = kind;
            return 0;
        }
    c.objective_markers.push_back({id, kind, ""});
    return 0;
}
int api_objective_remove(lua_State* L) {
    const int id = int_arg(L, 1);
    std::erase_if(ctx(L).objective_markers, [id](const LevelLogic::ObjectiveMarker& m) { return m.id == id; });
    return 0;
}

// --- controls (CLevel::Set<Kind>ControlEnable, PlayerComponent's tutorial flags) ----------

int api_enable_controls(lua_State* L) {
    ctx(L).enable_controls(bool_arg(L, 1));
    return 0;
}
template <bool LevelLogic::Controls::*Field>
int api_set_control(lua_State* L) {
    ctx(L).controls.*Field = bool_arg(L, 1);
    return 0;
}
// IsCounterAttackControlEnabled: the counter button is up (CHUDDisplay::IsUIButtonEnabled(5, 7):
// the scripts allow it and an enemy's attack can be countered now).
int api_is_counter_enabled(lua_State* L) {
    LevelLogic& c = ctx(L);
    lua_pushboolean(L, c.controls.counter && c.counter_open);
    return 1;
}
// IgnoreControls(direction bits, on): the stick no longer pushes the player that way.
int api_ignore_controls(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int bits = int_arg(L, 1) & 0xff;
    c.controls.ignore = bool_arg(L, 2) ? c.controls.ignore | bits : c.controls.ignore & ~bits;
    return 0;
}
int api_enable_control_glow(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (bool_arg(L, 2)) c.control_glow.insert(str_arg(L, 1));
    else c.control_glow.erase(str_arg(L, 1));
    return 0;
}
template <bool LevelLogic::PlayerDid::*Field>
int api_player_did(lua_State* L) {
    lua_pushboolean(L, ctx(L).player_did.*Field);
    return 1;
}

// --- time, chance, distances, positions ----------------------------------------------------

int api_get_time(lua_State* L) {   // CLevel's play time, seconds
    lua_pushnumber(L, ctx(L).play_time);
    return 1;
}
int api_get_time_ms(lua_State* L) {
    lua_pushinteger(L, lua_Integer(ctx(L).play_time * 1000.0));
    return 1;
}
// Random(): 0..1; Random(n): 1..n; Random(a, b): a..b.
int api_random(lua_State* L) {
    const int n = lua_gettop(L);
    if (n == 0) {
        lua_pushnumber(L, std::uniform_real_distribution<double>(0, 1)(rng()));
        return 1;
    }
    const int lo = n == 1 ? 1 : int_arg(L, 1), hi = n == 1 ? int_arg(L, 1) : int_arg(L, 2);
    lua_pushinteger(L, hi <= lo ? lo : std::uniform_int_distribution<int>(lo, hi)(rng()));
    return 1;
}
// GetDistFromMC(id): metres from the player (-1 if there is no such object).
int api_dist_from_mc(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 a, b;
    position_of(c, 0, b);
    lua_pushnumber(L, position_of(c, int_arg(L, 1), a) ? glm::distance(a, b) : -1.0f);
    return 1;
}
int api_dist_from_mc_z(lua_State* L) {   // the height difference
    LevelLogic& c = ctx(L);
    glm::vec3 a, b;
    position_of(c, 0, b);
    lua_pushnumber(L, position_of(c, int_arg(L, 1), a) ? std::abs(a.z - b.z) : -1.0f);
    return 1;
}
template <bool Ground>
int api_dist_between(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 a, b;
    if (!position_of(c, int_arg(L, 1), a) || !position_of(c, int_arg(L, 2), b)) {
        lua_pushnumber(L, -1);
        return 1;
    }
    lua_pushnumber(L, Ground ? glm::distance(glm::vec2(a), glm::vec2(b)) : glm::distance(a, b));
    return 1;
}
int api_object_get_position(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 p(0);
    position_of(c, int_arg(L, 1), p);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    lua_pushnumber(L, p.z);
    return 3;
}
int api_object_set_position(lua_State* L) {
    LevelLogic& c = ctx(L);
    const glm::vec3 p(float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)));
    if (int_arg(L, 1) == 0) c.player.center = p + glm::vec3(0, 0, c.player.radius.z + 0.05f);   // as it is: gliding stays
    else if (LevelLogic::Object* o = c.object(int_arg(L, 1))) c.teleport_object(*o, p);
    return 0;
}
// getMCWorldHeight(): the player's height in the world, whole metres.
int api_mc_world_height(lua_State* L) {
    LevelLogic& c = ctx(L);
    lua_pushinteger(L, lua_Integer(c.player.center.z - c.player.radius.z));
    return 1;
}
// IsCameraFacing(object[, degrees]): the object (its middle) is within that angle of the
// view's direction (default: the field of view).
int api_is_camera_facing(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 at;
    if (!position_of(c, int_arg(L, 1), at)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const float deg = lua_gettop(L) >= 2 && int_arg(L, 2) != 0 ? float(lua_tonumber(L, 2)) : c.game_fov();
    const glm::vec3 to = glm::normalize(at + glm::vec3(0, 0, 0.9f) - c.player.eye());
    lua_pushboolean(L, glm::dot(to, c.player.view_forward()) > std::cos(glm::radians(deg)));
    return 1;
}
int api_is_sound_playing(lua_State* L) {   // IsSoundPlaying(label) / IsSoundIdPlaying(handle)
    LevelLogic& c = ctx(L);
    bool on = false;
    if (c.audio) on = lua_type(L, 1) == LUA_TNUMBER ? c.audio->playing(int_arg(L, 1)) : c.audio->playing(str_arg(L, 1));
    lua_pushboolean(L, on);
    return 1;
}
// The weather (CLevel +0xa98: rain): StartWeatherEffect / StopWeatherEffect, IsWeatherEffectActive.
int api_weather(lua_State* L) {
    ctx(L).weather = lua_tointeger(L, lua_upvalueindex(2)) != 0;
    return 0;
}
int api_is_weather(lua_State* L) {
    lua_pushboolean(L, ctx(L).weather);
    return 1;
}
int api_false(lua_State* L) {   // IsLowEndDevice: this port draws everything
    lua_pushboolean(L, 0);
    return 1;
}
int api_nothing(lua_State*) { return 0; }   // analytics, store prompts, editor hints

// SetWinScreen(title, xp, wayne tech[, name, value, value]...): the end of a mission; the XP and
// WayneTech go to the inventory (CInventory::AddXPPoints / AddWayneTechPoints).
int api_set_win_screen(lua_State* L) {
    LevelLogic& c = ctx(L);
    std::string title = str_arg(L, 1);
    if (title.empty())
        if (const Quests::Quest* q = c.quests.find(c.quests.last_completed())) title = q->title;
    const std::string text = c.strings.find(title);
    c.win = {true, text.empty() ? title : text, int_arg(L, 2), int_arg(L, 3)};
    c.xp += int_arg(L, 2), c.wayne_tech += int_arg(L, 3);
    c.log("win screen: " + c.win.title + ", " + std::to_string(c.win.xp) + " XP, " + std::to_string(c.win.coins) + " WayneTech");
    return 0;
}
int api_add_xp(lua_State* L) {   // AddXP(n)
    ctx(L).xp += int_arg(L, 1);
    return 0;
}
int api_add_wayne_tech(lua_State* L) {   // AddWayneTech(n, ...)
    ctx(L).wayne_tech += int_arg(L, 1);
    return 0;
}

// --- script globals (CScriptGlobals) -------------------------------------------------------

int api_set_global_var(lua_State* L) {
    ctx(L).global_vars[str_arg(L, 1)] = int_arg(L, 2);
    return 0;
}
int api_register_global_var(lua_State* L) {
    ctx(L).global_vars.try_emplace(str_arg(L, 1), int_arg(L, 2));
    return 0;
}
int api_get_global_var(lua_State* L) {   // -9999 when unknown
    const auto& g = ctx(L).global_vars;
    auto it = g.find(str_arg(L, 1));
    lua_pushinteger(L, it == g.end() ? -9999 : it->second);
    return 1;
}

// --- objects -------------------------------------------------------------------------------

// Enable / Disable(object): without one, the script's own object (CLuaScriptManager's current).
int api_enable_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    int id = int_arg(L, 1);
    if (lua_gettop(L) == 0) {
        lua_getglobal(L, "CurrentActor");
        id = int(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    if (LevelLogic::Object* o = c.object(id)) c.enable(*o, lua_tointeger(L, lua_upvalueindex(2)) != 0);
    return 0;
}
// SetWeaponAmmo(weapon, count) / GetWeaponAmmo(weapon) (the player's inventory).
int api_set_weapon_ammo(lua_State* L) {
    ctx(L).weapon_ammo[str_arg(L, 1)] = int_arg(L, 2);
    return 0;
}
int api_get_weapon_ammo(lua_State* L) {
    const auto& a = ctx(L).weapon_ammo;
    auto it = a.find(str_arg(L, 1));
    lua_pushinteger(L, it == a.end() ? 0 : it->second);
    return 1;
}
// ShowObject / HideObject: 0 is Batman.
int api_show_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    const bool on = lua_tointeger(L, lua_upvalueindex(2)) != 0;
    if (int_arg(L, 1) == 0) {
        c.player_hidden = !on;
        if (c.trace) c.log(on ? "show the player" : "hide the player");
    }
    else if (LevelLogic::Object* o = c.object(int_arg(L, 1))) c.show(*o, on);
    return 0;
}
int api_enable_show(lua_State* L) {   // EnableShowObject / DisableHideObject
    LevelLogic& c = ctx(L);
    const bool on = lua_tointeger(L, lua_upvalueindex(2)) != 0;
    if (LevelLogic::Object* o = c.object(int_arg(L, 1))) c.enable(*o, on), c.show(*o, on);
    return 0;
}
int api_enable_interaction(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (LevelLogic::Object* o = c.object(int_arg(L, 1))) o->interact_enabled = lua_tointeger(L, lua_upvalueindex(2)) != 0;
    return 0;
}
// SetInteractionParams(object, radius, target, button frame) (CInteractionComponent::
// SetInteractionParams): its radius, and the object using it enables (OnInteract).
int api_set_interaction_params(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || !c.object_component(*o, kCompInteraction)) return 0;
    o->interactive = true;
    o->interact_radius = float(lua_tointeger(L, 2));
    o->interact_target = lua_isnumber(L, 3) ? int_arg(L, 3) : -1;
    o->interact_enabled = true;
    return 0;
}
int api_current_interaction(lua_State* L) {
    lua_pushinteger(L, ctx(L).last_interaction);
    return 1;
}

// --- actors (CStateSetComponent, CNpcAIComponent) -------------------------------------------

// The player's state type now (STATE_TYPE_*), from what the hero is doing.
int player_state_mask(const Player& p, float speed) {
    constexpr int kIdle = 1, kMove = 2, kJump = 1 << 3, kFall = 1 << 4, kRun = 1 << 9, kGlide = 1 << 21, kGrapnel = 1 << 22,
                  kOnGround = 1 << 29;
    if (p.grappling) return kGrapnel;
    if (p.gliding) return kGlide;
    if (!p.on_ground) return p.vertical_speed > 0 ? kJump : kFall;
    return kOnGround | (p.rushing ? kMove | kRun : speed > 0.2f ? kMove : kIdle);
}
std::string player_state_name(const Player& p, float speed) {
    if (p.grappling) return "Grapnel_Fly_Idle";
    if (p.gliding) return p.plunging ? "Glide_Plunge" : "Glide_Idle";
    if (!p.on_ground) return p.vertical_speed > 0 ? "Jump" : "Fall_Loop";
    if (p.rushing) return "Run";
    return speed > 0.2f ? "Walk" : "Idle";
}

// ActorSetState(actor, state[, wait, loop, ?]): the actor's state set plays the state's clip
// (an NPC: CNpcAIComponent::SetState); with wait = 1 the script waits for the clip.
int api_actor_set_state(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1);
    const std::string state = str_arg(L, 2);
    if (id == 0) return 0;   // the player's states follow the hero's movement
    LevelLogic::Object* o = c.object(id);
    if (!o || o->index == kNoIndex) return 0;
    ActorState st;
    if (!find_state(c.gol, object_type(c, *o), state, st) || st.clips.empty()) {
        if (c.warned_.insert("state " + o->type + " " + state).second) c.log("ActorSetState: " + o->type + " has no state " + state);
        return 0;
    }
    o->state = state;
    const std::string& clip = st.clips[size_t(std::uniform_int_distribution<size_t>(0, st.clips.size() - 1)(rng()))];
    c.scene.play_clip(o->index, clip, st.loop);
    Scene::ClipState cs;
    if (lua_gettop(L) >= 3 && int_arg(L, 3) == 1 && c.scene.clip_state(o->index, cs)) {
        lua_pushinteger(L, 2);
        lua_pushinteger(L, lua_Integer(cs.length));
        return lua_yield(L, 2);
    }
    return 0;
}
int api_actor_get_state(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1);
    if (id == 0) {
        lua_pushstring(L, (c.player_action.empty() ? player_state_name(c.player, c.player_speed) : c.player_action).c_str());
        return 1;
    }
    const LevelLogic::Object* o = c.object(id);
    lua_pushstring(L, o ? o->state.c_str() : "");
    return 1;
}
// ActorIsInStateType(actor, STATE_TYPE_* mask).
int api_actor_is_in_state_type(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1), mask = int_arg(L, 2);
    int now = 0;
    if (id == 0) {
        now = c.player_action.empty() ? player_state_mask(c.player, c.player_speed) : c.player_action_mask;
    } else if (const LevelLogic::Object* o = c.object(id)) {
        ActorState st;
        if (!o->state.empty() && find_state(c.gol, object_type(c, *o), o->state, st)) now = st.mask;
    }
    lua_pushboolean(L, (now & mask) != 0);
    return 1;
}
// IsActorLookingAt(actor, target, angle) (CGameObject::IsLookingAt): the actor's facing and the way to
// the target, flat, are closer than cos(angle). (The game passes the number to cosf as it is.)
int api_is_actor_looking_at(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 from, to;
    if (!position_of(c, int_arg(L, 1), from) || !position_of(c, int_arg(L, 2), to)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    float yaw = c.player.yaw;
    if (int_arg(L, 1) != 0)
        if (const LevelLogic::Object* o = c.object(int_arg(L, 1))) yaw = o->yaw;
    const glm::vec2 facing(-std::sin(glm::radians(yaw)), std::cos(glm::radians(yaw)));
    glm::vec2 d(to - from);
    d = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec2(0);
    lua_pushboolean(L, glm::dot(facing, d) > std::cos(float(lua_tonumber(L, 3))));
    return 1;
}
// ActorLookTo(actor, target): the actor turns to face the target (CGameObject::LookAt).
int api_actor_look_to(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 at;
    if (!position_of(c, int_arg(L, 2), at)) return 0;
    if (int_arg(L, 1) == 0) {
        const glm::vec3 d = at - c.player.center;
        c.player.yaw = c.face_request = glm::degrees(std::atan2(-d.x, d.y));
        return 0;
    }
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->index == kNoIndex) return 0;
    const glm::vec3 d = at - o->position;
    if (glm::length(glm::vec2(d)) < 1e-3f) return 0;
    o->yaw = glm::degrees(std::atan2(-d.x, d.y));
    c.scene.set_object_pose(o->index, o->position, o->yaw);
    return 0;
}
// SpawnObject(type, waypoint[, model]): a new actor of a gol type at a waypoint (CZonesManager::
// SpawnObject); its id, or -1. (The model override is not applied yet.)
int api_spawn_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string type = str_arg(L, 1);
    const LevelLogic::Object* wp = c.object(int_arg(L, 2));
    int type_id = -1;
    for (const auto& [id, g] : c.gol)
        if (g.name == type) type_id = id;
    if (!wp || type_id < 0) {
        lua_pushinteger(L, -1);
        return 1;
    }
    const int id = c.next_spawn_id++;
    const size_t index = c.scene.spawn_object(type_id, id, wp->position, wp->yaw);
    if (index == kNoIndex) {
        lua_pushinteger(L, -1);
        return 1;
    }
    LevelLogic::Object& o = c.add_object(id, index, type, wp->position, wp->yaw);
    c.enable(o, true);
    c.show(o, true);
    ActorState st;   // its state set's first state (CStateSetComponent's default: Idle)
    if (find_state(c.gol, type_id, "Idle", st) && !st.clips.empty()) {
        o.state = "Idle";
        c.scene.play_clip(index, st.clips[0], true);
    }
    lua_pushinteger(L, id);
    return 1;
}
// KillActor(actor[, ...]): dead at once (CGameObject::Kill), its death state if it has one.
int api_kill_actor(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (lua_isnil(L, 1)) return 0;
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->dead) return 0;
    o->dead = true;
    o->health = 0;
    // Its death: the state named, else its type's death states.
    const std::string named = lua_gettop(L) >= 2 ? str_arg(L, 2) : "";
    for (const std::string& state : {named, std::string("Enemy_Cross_Death"), std::string("Combat_death"), std::string("Death")})
        if (!state.empty() && c.actor_state(*o, state, true)) break;
    std::erase_if(c.objective_markers, [&](const LevelLogic::ObjectiveMarker& m) { return m.id == o->id; });
    c.runtime_.send_event("Dead", o->id);
    return 0;
}
// GrapplingLockTarget(target, ...): the grapnel's next shot goes to that object or waypoint (an
// enemy: it is pulled).
int api_grappling_lock_target(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.grapple_lock = c.object(int_arg(L, 1)) ? int_arg(L, 1) : -1;
    return 0;
}
// NPCSetSniper(actor, waypoint, stand ground, sweep[, ...]): the sniper sweeps its aim around
// the waypoint (CSniperComponent::Sweep_SetParams / Sweep_Start).
int api_npc_set_sniper(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 at;   // (0: Batman)
    if (c.object(int_arg(L, 1)) && position_of(c, int_arg(L, 2), at) && bool_arg(L, 4)) c.snipers[int_arg(L, 1)] = int_arg(L, 2);
    return 0;
}
// MissionFailedAutoReload(): back to the checkpoint.
int api_mission_failed_reload(lua_State* L) {
    ctx(L).respawn_player();
    return 0;
}
// NPCIsKilledByMC(actor[, any way]): dead, and Batman's doing.
int api_npc_killed_by_mc(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = c.object(int_arg(L, 1));
    lua_pushboolean(L, o && o->dead && c.killed_by_batman.count(o->id));
    return 1;
}
// ActorIsAttacking(actor, target (-1: anyone)[, ...]) / ActorIsAttacked(actor[, ...]).
int api_actor_is_attacking(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1), target = lua_gettop(L) >= 2 ? int_arg(L, 2) : -1;
    bool on = false;
    if (id == 0) on = c.batman_target >= 0 && (target == -1 || target == c.batman_target);
    else on = c.attacking_batman.count(id) && (target == -1 || target == 0);
    lua_pushboolean(L, on);
    return 1;
}
int api_actor_is_attacked(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1);
    lua_pushboolean(L, id == 0 ? !c.attacking_batman.empty() : c.batman_target == id);
    return 1;
}
int api_actor_fade_out(lua_State* L) {   // ActorFadeOut(actor) (CGameObject::FadeOut): gone
    if (LevelLogic::Object* o = ctx(L).object(int_arg(L, 1))) ctx(L).show(*o, false);
    return 0;
}
// SetOnGargoyle(actor, gargoyle[, state]): the actor perched where the gargoyle object is, facing its way.
int api_set_on_gargoyle(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* g = c.object(int_arg(L, 2));
    if (!g) return 0;
    if (int_arg(L, 1) == 0) {
        c.player.spawn(g->position, g->yaw);
        c.face_request = g->yaw;
    } else if (LevelLogic::Object* o = c.object(int_arg(L, 1))) {
        o->yaw = g->yaw;
        c.teleport_object(*o, g->position);
    }
    return 0;
}
// ActorGrapnelEnable(actor, on): CGameObject::SetTargetable.
int api_actor_grapnel_enable(lua_State* L) {
    if (LevelLogic::Object* o = ctx(L).object(int_arg(L, 1))) o->targetable = bool_arg(L, 2);
    return 0;
}
int api_set_actor_health(lua_State* L) {
    if (LevelLogic::Object* o = ctx(L).object(int_arg(L, 1)); o && !o->dead) o->health = float(lua_tonumber(L, 2));
    return 0;
}

// NPCGoToWaypoint(actor, ?, waypoint[, run]) / NPCGoToWaypointWithNavmesh.
int api_npc_go_to_waypoint(lua_State* L) {
    ctx(L).walk_to(int_arg(L, 1), int_arg(L, 3), bool_arg(L, 4));
    return 0;
}
// NPCGoToWaypointWithNavmesh(actor, waypoint): there at a run (CNpcAIComponent::NPCGotoWaypoint
// with its run flag), not if it is dead.
int api_npc_go_to_waypoint_navmesh(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* o = c.object(int_arg(L, 1)); o && !o->dead) c.walk_to(o->id, int_arg(L, 2), true);
    return 0;
}
// SetActorSpeedWalk / SetActorSpeedRun(actor, metres per second).
int api_set_actor_speed(lua_State* L) {
    ctx(L).set_actor_speed(int_arg(L, 1), float(lua_tonumber(L, 2)), lua_tointeger(L, lua_upvalueindex(2)) != 0);
    return 0;
}

// NPCAttackTarget(actor, target): the actor fights the target (0: Batman) (CAwarenessComponent::
// SetCurrentTarget, CNpcAIComponent::StartCatchEnemy); not the dead or those mid-takedown.
int api_npc_attack_target(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->dead || lua_isnil(L, 2) || int_arg(L, 2) < 0) return 0;
    c.attack_targets[o->id] = int_arg(L, 2);
    return 0;
}
// NPCSetStance(actor, waypoint, "Aggressive" | "Passive" | "Coward" | "StandGround" | "Mission"):
// an aggressive actor goes for Batman, a passive one stops.
int api_npc_set_stance(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string stance = str_arg(L, 3);
    if (!c.object(int_arg(L, 1))) return 0;
    if (stance == "Aggressive") c.attack_targets[int_arg(L, 1)] = 0;
    else c.attack_targets.erase(int_arg(L, 1));
    return 0;
}

// NPCSetTarget(actor, target) (CNpcAIComponent::NPCTargetSet): what it aims at; Batman (0)
// is a fight, anyone else (the hostages) it turns to face.
int api_npc_set_target(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    glm::vec3 at;
    if (!o || o->dead || lua_isnil(L, 2) || !position_of(c, int_arg(L, 2), at)) return 0;
    if (int_arg(L, 2) == 0) c.attack_targets[o->id] = 0;
    else if (glm::length(glm::vec2(at - o->position)) > 1e-3f) o->yaw = glm::degrees(std::atan2(-(at.x - o->position.x), at.y - o->position.y));
    return 0;
}
// GetEnemiesInRange(object, radius[, height]): the living enemies within `radius` of the object
// and `height` (0: any) of its level, nearest first, at most 18; nil when none.
int api_get_enemies_in_range(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 from;
    const float radius = float(lua_tonumber(L, 2));
    float height = float(lua_tonumber(L, 3));
    if (height <= 0.1f) height = 10000;
    std::vector<std::pair<float, int>> found;
    if (position_of(c, int_arg(L, 1), from))
        for (const auto& [id, o] : c.objects())
            if (c.is_enemy(id) && std::abs(o.position.z - from.z) <= height)
                if (const float d = glm::distance(o.position, from); d < radius) found.push_back({d, id});
    std::sort(found.begin(), found.end());
    if (found.empty()) {
        lua_pushnil(L);
        return 1;
    }
    const int n = std::min(int(found.size()), 18);
    for (int i = 0; i < n; ++i) lua_pushinteger(L, found[size_t(i)].second);
    return n;
}
int api_get_frame_time(lua_State* L) {   // GetFrameTime: the last frame's milliseconds (Application +0xf7c)
    lua_pushinteger(L, lua_Integer(ctx(L).frame_ms));
    return 1;
}
// SpawnObjectNearActor(actor, distance, type[, model][, {front_angle, back_angle}]): a new
// actor of that type `distance` from the actor, in a random direction (CZonesManager::
// SpawnObjectNearActor); its id, or -1.
int api_spawn_object_near_actor(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* near = c.object(int_arg(L, 1));
    const std::string type = str_arg(L, 3);
    int type_id = -1;
    for (const auto& [id, g] : c.gol)
        if (g.name == type) type_id = id;
    if (!near || type_id < 0) {
        lua_pushinteger(L, -1);
        return 1;
    }
    const float a = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(rng()), d = float(lua_tonumber(L, 2));
    const glm::vec3 at = near->position + glm::vec3(std::cos(a) * d, std::sin(a) * d, 0.0f);
    const int id = c.next_spawn_id++;
    const size_t index = c.scene.spawn_object(type_id, id, at, near->yaw);
    if (index == kNoIndex) {
        lua_pushinteger(L, -1);
        return 1;
    }
    LevelLogic::Object& o = c.add_object(id, index, type, at, near->yaw);
    c.enable(o, true);
    c.show(o, true);
    ActorState st;
    if (find_state(c.gol, type_id, "Idle", st) && !st.clips.empty()) {
        o.state = "Idle";
        c.scene.play_clip(index, st.clips[0], true);
    }
    lua_pushinteger(L, id);
    return 1;
}
// EnableTalkMode(on): Batman talks on the radio (CActorBaseComponent::EnableTalk); its click.
int api_enable_talk_mode(lua_State* L) {
    if (bool_arg(L, 1) && ctx(L).audio) ctx(L).audio->play("sfx_radio_start");
    return 0;
}
// PostProcessingEffectAdd(name, ms | {time_to_run, time_to_fade_in, extra_texture, ...}) /
// PostProcessingEffectRemove(name) (CPostProcessManager): RadialBlur, ShockWave, HeatHaze and
// ColorCorrection (a 16x16x16 LUT in commons_tex) are not drawn yet; the trace shows them.
int api_post_effect(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.trace) c.log(std::string(lua_tointeger(L, lua_upvalueindex(2)) ? "post effect on: " : "post effect off: ") + str_arg(L, 1));
    return 0;
}

// EnterBatPod(object) / ExitBatPod([object]) / IsPlayerOnTheBatOrBatpod().
int api_enter_batpod(lua_State* L) {
    ctx(L).mount_batpod(int_arg(L, 1));
    return 0;
}
int api_exit_batpod(lua_State* L) {
    ctx(L).dismount_batpod();
    return 0;
}
int api_on_batpod(lua_State* L) {
    lua_pushboolean(L, ctx(L).riding >= 0);
    return 1;
}
// QTE_StartNavigation() / QTE_EndNavigation() / QTE_IsInNavigation(): the chase mode (the
// interface off while in it: CLevel::EnableInterface).
int api_qte_navigation(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.qte_navigation = lua_tointeger(L, lua_upvalueindex(2)) != 0;
    c.hud_enabled = !c.qte_navigation;
    return 0;
}
int api_qte_in_navigation(lua_State* L) {
    lua_pushboolean(L, ctx(L).qte_navigation);
    return 1;
}
// QTE_GrapplingSetTarget(target, waypoint[, ?, function]) (CActorBaseComponent::
// SetGrapplingTarget): the grapnel's next shot goes to the target.
int api_qte_grappling_set_target(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.object(int_arg(L, 1))) c.grapple_lock = int_arg(L, 1);
    return 0;
}
// StartCombatMusic(): the music gives way to env_music_action_low; its handle.
// StopCombatMusic(handle).
int api_start_combat_music(lua_State* L) {
    LevelLogic& c = ctx(L);
    int h = -1;
    if (c.audio) {
        c.audio->stop_music(500);
        h = c.audio->play_event("env_music_action_low", Audio::Params{});
    }
    lua_pushinteger(L, h);
    return 1;
}
int api_stop_combat_music(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (c.audio && int_arg(L, 1) > 0) c.audio->stop(int_arg(L, 1), 1000);
    return 0;
}
// ActorAttackableEnable(actor, on) (CGameObject::SetAttackable): whether Batman may go for it.
int api_actor_attackable(lua_State* L) {
    if (LevelLogic::Object* o = ctx(L).object(int_arg(L, 1))) o->targetable = bool_arg(L, 2);
    return 0;
}

// IsDead(actor): what is not an actor (CZonesManager::FindActor: no NPC AI, e.g. the
// Underground's electric turret) or not there at all counts as dead.
int api_is_dead(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = lua_isnil(L, 1) ? nullptr : c.object(int_arg(L, 1));
    lua_pushboolean(L, !o || o->dead || !c.object_component(*o, kCompNpcAi));
    return 1;
}
// ActorIsDetected(actor): an enemy is aware of it (CAwarenessComponent: its target the actor,
// its level above 0). Here: one fighting it, or (Batman) a sniper aiming at him.
int api_actor_is_detected(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1);
    bool seen = id == 0 && (!c.attacking_batman.empty() || !c.snipers.empty());
    for (const auto& [enemy, target] : c.attack_targets) {
        const LevelLogic::Object* o = c.object(enemy);
        if (target == id && o && !o->dead && o->enabled) seen = true;
    }
    lua_pushboolean(L, seen);
    return 1;
}
// NPCGoToActorWithNavmesh(actor, target): where the target is now (CNpcAIComponent::
// NPCGotoPosition). To Batman (0) an enemy comes to fight him.
int api_npc_go_to_actor(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->dead) return 0;
    if (int_arg(L, 2) == 0) {
        if (c.is_enemy(o->id)) c.attack_targets[o->id] = 0;
        else c.walk_to_point(o->id, c.player.center - glm::vec3(0, 0, c.player.radius.z), false);
    } else if (const LevelLogic::Object* t = c.object(int_arg(L, 2))) {
        c.walk_to_point(o->id, t->position, false);
    }
    return 0;
}
// NPCGoToObject(actor, ?, object, run, ?) (DoNPCGoToWaypointOrObject): to an object; to Batman
// (0) to fight him.
int api_npc_go_to_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->dead) return 0;
    if (int_arg(L, 3) == 0) c.attack_targets[o->id] = 0;
    else c.walk_to(o->id, int_arg(L, 3), bool_arg(L, 4));
    return 0;
}
// NPCReturnToSavedPosition(actor) (CNpcAIComponent::EndRecordingPath): back where it stood.
int api_npc_return(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->dead) return 0;
    c.attack_targets.erase(o->id);
    c.walk_to_point(o->id, o->home, false);
    return 0;
}
// NPCSetEnemyFlag(actor, waypoint, enemy[, ?]) (CNpcAIComponent::SetEnemy).
int api_npc_set_enemy_flag(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o) return 0;
    o->enemy = bool_arg(L, 3) ? 1 : 0;
    if (!o->enemy) c.attack_targets.erase(o->id);
    return 0;
}
int api_set_invincible(lua_State* L) {   // SetInvincible(actor, on): the health component's flag
    if (LevelLogic::Object* o = ctx(L).object(int_arg(L, 1))) o->invincible = bool_arg(L, 2);
    return 0;
}
// LinkObject(object, parent (0: Batman), node, dx, dy, dz, ...) / UnlinkObject(object).
int api_link_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.link_at(int_arg(L, 1), int_arg(L, 2), str_arg(L, 3),
              glm::vec3(float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5)), float(lua_tonumber(L, 6))));
    return 0;
}
int api_unlink_object(lua_State* L) {
    ctx(L).unlink(int_arg(L, 1));
    return 0;
}
// NoiseEvent(object, ?, metres, ?) (CAIController::NoiseRegister): enemies within earshot come.
int api_noise_event(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 at;
    if (!position_of(c, int_arg(L, 1), at)) return 0;
    const float range = float(lua_tonumber(L, 3));
    for (const auto& [id, o] : c.objects())
        if (c.is_enemy(id) && glm::distance(o.position, at) <= range && !c.attack_targets.count(id)) c.attack_targets[id] = 0;
    return 0;
}
// QTE_DisableControl() (PlayerComponent::ResetQTE): the QTE button goes.
int api_qte_disable(lua_State* L) {
    ctx(L).button_qte = {};
    return 0;
}
// StartIntroStory(story, callback[, ?]): a storyboard (StoryBoard.xml), then the callback.
int api_start_intro_story(lua_State* L) {
    ctx(L).story_request = {str_arg(L, 1), str_arg(L, 2)};
    return 0;
}
// HideObjectNode(object, node) (CGameObject::SetNodeVisible).
int api_hide_object_node(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (const LevelLogic::Object* o = c.object(int_arg(L, 1)); o && o->index != kNoIndex) c.scene.set_node_visible(o->index, str_arg(L, 2), false);
    return 0;
}
// DisableControlEvent(event) / EnableControlEvent(event) (CPlayerControl::EnableInputEvent):
// "MoveFast" is the run.
int api_control_event(lua_State* L) {
    if (str_arg(L, 1) == "MoveFast") ctx(L).player_rush = lua_tointeger(L, lua_upvalueindex(2)) != 0;
    return 0;
}

// --- cameras (CCameraMgr) ------------------------------------------------------------------

// PlayCameraAnim(object, clip, loop[, wait]): the view follows the camera animated in the
// object's model (CCameraMgr::SetMaxCamera) until the scripts switch back to the player; with
// `wait`, the script waits for the clip.
int api_play_camera_anim(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->index == kNoIndex) return 0;
    if (!o->visible) c.show(*o, true);
    c.camera_anim = o->id;
    Scene::ClipState cs;
    if (c.scene.play_clip(o->index, str_arg(L, 2), bool_arg(L, 3)) && lua_gettop(L) >= 4 && bool_arg(L, 4) &&
        c.scene.clip_state(o->index, cs)) {
        lua_pushinteger(L, 2);
        lua_pushinteger(L, lua_Integer(cs.length));
        return lua_yield(L, 2);
    }
    return 0;
}
// CameraLookAt(x, y, z, ms) or (object, ms): the view turns to a point over ms.
int api_camera_look_at(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int n = lua_gettop(L);
    glm::vec3 at;
    float ms = 0;
    if (n == 4 || n >= 7) {
        at = {float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3))};
        ms = float(lua_tonumber(L, 4));
    } else if (n >= 2) {
        if (!position_of(c, int_arg(L, 1), at)) return 0;
        ms = float(lua_tonumber(L, 2));
    } else {
        c.camera_to_player(true);
        return 0;
    }
    c.player_look_to(at, std::max(ms, 1.0f) / 1000.0f);   // the view turns there, then is the player's again
    return 0;
}
// CameraFollowObject(camera, node, dx, dy, dz, ?, target, node, dx, dy, dz, ?, ms): the view from a
// point (an object, usually a camera dummy, plus the offset) at another; over ms from the last (0: a cut).
int api_camera_follow_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 from, to;
    if (!position_of(c, int_arg(L, 1), from) || !position_of(c, int_arg(L, 7), to)) return 0;
    const glm::vec3 a(float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)), float(lua_tonumber(L, 5)));
    const glm::vec3 b(float(lua_tonumber(L, 9)), float(lua_tonumber(L, 10)), float(lua_tonumber(L, 11)));
    // The camera's point is 1.6 m up (the scripts take that off for their eye-level dummies: -1.6);
    // the point looked at is as given.
    c.script_camera(from + a + glm::vec3(0, 0, 1.6f), to + b,
                    float(lua_tonumber(L, 13)));
    return 0;
}

// CameraSwitchToPlayer / CameraSwitchToPlayerResetOrientation([ms]): the player's camera again.
int api_camera_to_player(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.camera_anim = -1;
    c.script_camera_off();
    c.camera_to_player(lua_tointeger(L, lua_upvalueindex(2)) == 0);
    return 0;
}

// StartCinematicObject(object[, skippable]): its tracks start (it does not wait; it returns 2
// and its length, as the game's does).
int api_start_cinematic_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.start_cinematic(int_arg(L, 1))) {
        c.log("StartCinematicObject: no cinematic " + std::to_string(int_arg(L, 1)));
        return 0;
    }
    lua_pushinteger(L, 2);
    lua_pushinteger(L, lua_Integer(c.cinematic_length(int_arg(L, 1))));
    return 2;
}
int api_end_cinematic_object(lua_State* L) {
    ctx(L).end_cinematic(int_arg(L, 1));
    return 0;
}
// PlayAnimation(object, clip[, wait, loop]): the object's clip; with `wait` the script waits for it.
int api_play_animation(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o || o->index == kNoIndex) return 0;
    Scene::ClipState cs;
    if (c.scene.play_clip(o->index, str_arg(L, 2), bool_arg(L, 4)) && bool_arg(L, 3) && c.scene.clip_state(o->index, cs)) {
        lua_pushinteger(L, 2);
        lua_pushinteger(L, lua_Integer(cs.length));
        return lua_yield(L, 2);
    }
    return 0;
}

// The binoculars: StartGadgetZoom(), GadgetZoomTarget(object, zoom, ms, ms, ?, dx, dy, dz[, ?, node]),
// StopGadgetZoom(ms). The point looked at is the object's (an actor's head), plus the offset.
int api_start_gadget_zoom(lua_State* L) {
    ctx(L).gadget_start();
    return 0;
}
int api_gadget_zoom_target(lua_State* L) {
    LevelLogic& c = ctx(L);
    const LevelLogic::Object* o = c.object(int_arg(L, 1));
    if (!o) return 0;
    ActorState st;
    const bool actor = find_state(c.gol, object_type(c, *o), "Idle", st);
    const glm::vec3 offset(float(lua_tonumber(L, 6)), float(lua_tonumber(L, 7)), float(lua_tonumber(L, 8)));
    c.gadget_target(o->position + glm::vec3(0, 0, actor ? 1.6f : 0.0f) + offset, float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)));
    return 0;
}
int api_stop_gadget_zoom(lua_State* L) {
    ctx(L).gadget_stop(float(lua_tonumber(L, 1)));
    return 0;
}
int api_camera_set_fov(lua_State* L) {   // CameraSetFOV(degrees, ms)
    ctx(L).set_fov(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)));
    return 0;
}
int api_set_hud_state(lua_State* L) {   // SetHudState(frame)
    ctx(L).hud_state = str_arg(L, 1);
    return 0;
}
// CameraShake(type, amount, ms) (CGameCamera::ShakeCamera).
int api_camera_shake(lua_State* L) {
    ctx(L).shake(float(lua_tonumber(L, 3)), std::min(float(lua_tonumber(L, 2)), 3.0f));
    return 0;
}

// EndCinematic(): CLevel::EndCinematic gives the player the view and controls back (the
// follow camera behind Batman).
int api_end_cinematic(lua_State* L) {
    LevelLogic& c = ctx(L);
    c.cinematic_mode = false;
    c.camera_anim = -1;
    c.script_camera_off();
    c.camera_to_player(false);
    return 0;
}

// --- slow motion (CSlowMotion) -------------------------------------------------------------

// SetSlowMotion(factor, ms in, ms held, ms out, ...) (CSlowMotion::Set).
int api_set_slow_motion(lua_State* L) {
    ctx(L).slow_motion(float(lua_tonumber(L, 1)), float(lua_tonumber(L, 2)), float(lua_tonumber(L, 3)), float(lua_tonumber(L, 4)));
    return 0;
}
int api_reset_slow_motion(lua_State* L) {   // ResetSlowMotion(ms)
    ctx(L).reset_slow_motion(float(lua_tonumber(L, 1)));
    return 0;
}
// ResetObjectSlowMotion(object, ms): Batman's (0) is the game's.
int api_reset_object_slow_motion(lua_State* L) {
    if (int_arg(L, 1) == 0) ctx(L).reset_slow_motion(float(lua_tonumber(L, 2)));
    return 0;
}
int api_reset_ignored_controls(lua_State* L) {   // ResetAllIgnoredControls
    ctx(L).controls.ignore = 0;
    return 0;
}
// QTE_AutoGrapnel(target, waypoint, ...): Batman fires the grapnel at the target by himself
// (CActorBaseComponent::SetGrapplingTarget), over its edge.
int api_auto_grapnel(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 at;
    if (!position_of(c, int_arg(L, 1), at)) return 0;
    glm::vec2 d(at - c.player.center);
    if (glm::length(d) > 1e-3f) d = glm::normalize(d);
    c.player.grapple_to(at + glm::vec3(d * 1.5f, 0.0f));
    return 0;
}

// The constants RegisterScriptGlobalVariables and CAIController::Script_Init set.
const char* const kConstants = R"(
CurrentActor = -1
EventSender = -1
STATE_TYPE_IDLE = 1  STATE_TYPE_MOVE = 2  STATE_TYPE_ATTACK = 4  STATE_TYPE_JUMP = 8  STATE_TYPE_FALL = 16
STATE_TYPE_GRAB = 32  STATE_TYPE_CLIMB = 64  STATE_TYPE_HURT = 128  STATE_TYPE_DIE = 256  STATE_TYPE_RUN = 512
STATE_TYPE_DASH = 1024  STATE_TYPE_SPECIAL = 2048  STATE_TYPE_STRUGGLE = 4096  STATE_TYPE_RELOAD = 8192
STATE_TYPE_DIZZY = 16384  STATE_TYPE_INTERACTION = 32768  STATE_TYPE_FINISHING = 65536  STATE_TYPE_PARRY = 131072
STATE_TYPE_DODGE = 262144  STATE_TYPE_IGNORE_ACTOR_COLLISION = 524288  STATE_TYPE_GARGOYLE = 1048576
STATE_TYPE_GLIDE = 2097152  STATE_TYPE_GRAPNEL = 4194304  STATE_TYPE_BATARANG = 8388608
STATE_TYPE_USE_ANIMATED_CAPE = 16777216  STATE_TYPE_ADDITIVE_ANIMATION_STATE = 33554432  STATE_TYPE_DOWN = 67108864
STATE_TYPE_HIGN = 134217728  STATE_TYPE_LOW = 268435456  STATE_TYPE_ON_GROUND = 536870912
STATE_TYPE_NON_ENEMY_STRIKE = 1073741824
SFX_FLY = 1  SFX_GRAPNEL = 2  SFX_LAND_ENEMY = 3  SFX_GRAPNEL_GLIDE = 4  SFX_GRAPNEL_FORCED = 5
SFX_CLOTHING_MOVEMENT = 6  SFX_CAPE_RAIN = 7  SFX_FLY_FAST = 8  SFX_FLY_FAST_PLUNGE = 9  SFX_FLY_FORCED = 10
SFX_FLY_FAST_FORCED = 11  SFX_FLY_FAST_PLUNGE_FORCED = 12
NPC_SOUND_WALK_DISTANCE = 8  NPC_SOUND_RUN_DISTANCE = 12  MC_SOUND_DISTANCE = 40  MC_MAX_DISTANCE = 40
CAMERA_SHAKE_HORIZONTAL = 0  CAMERA_SHAKE_VERTICAL = 1  CAMERA_SHAKE_CIRCULAR = 2  CAMERA_SHAKE_RANDOM = 3
CAMERA_SHAKE_FOV = 4  CAMERA_SHAKE_BOMB = 5
CHAPTER1 = 0  CHAPTER2 = 1  CHAPTER3 = 2  CHAPTER4 = 3  CHAPTER5 = 4
REWARD_COINS = 0  REWARD_GUN = 1  REWARD_SWORD = 2  REWARD_TREASURE = 3  REWARD_HORSE = 4  REWARD_PROPERTY = 5
INTERFERENCE_COOLDOWN_TIME = 10000  gCurSoundZone = 1  gCurSoundHeightZone = 0
NPCSpawned = {}  NPCAlarmPostWait = {}  NPCAlarmPost = {}  AlarmPost = {}  AlarmPostMC = {}  NPCWeaponCacheWait = {}
NPCWeaponCache = {}  WeaponCache = {}  NPCWeather = {}  ActorSFXState = {}
Talk = 0  Smash = 1  PodEnter = 2  BatEnter = 3  Use_Lever = 4  C4Disarm = 5  XMine = 6
ITEM_UNKNOWN = -1  ITEM_MONEY = 0  ITEM_WEAPON = 1  ITEM_COLLECTIBLE = 2  InVengeance = 0  BreakVengeance = 0
WEAPON_NONE = -1  WEAPON_UMBRELLA = 0  WEAPON_SWORD = 1  WEAPON_PISTOL = 2  WEAPON_MUSKET = 3  WEAPON_FIST = 7
WEAPON_BATARANG = 8  WEAPON_ROCKET_LAUNCHER = 9
WEAPON_SUBTYPE_NORMAL = 0  WEAPON_SUBTYPE_COMBAT = 1  WEAPON_SUBTYPE_STONE = 2  WEAPON_SUBTYPE_EMP = 3
WEAPON_SUBTYPE_FLASH = 4  WEAPON_SUBTYPE_GAS = 5  WEAPON_SUBTYPE_ZOOM = 6
AI_BEHAVIOR_ENEMY_RANGED_1 = 0  AI_BEHAVIOR_ENEMY_MELEE_1 = 2  AI_BEHAVIOR_HELICOPTER = 5  AI_BEHAVIOR_COP_1 = 6
AI_BEHAVIOR_PEDESTRIAN_1 = 7  AI_BEHAVIOR_MINIBOSS = 15
AI_STATE_IDLE = 2  AI_STATE_MOVE = 64  AI_STATE_PATROL = 4  AI_STATE_FOLLOW = 32  AI_STATE_CATCH_ENEMY = 16
AI_STATE_ATTACK_RANGED = 4096
AI_STANCE_COWARD = 0  AI_STANCE_PASSIVE = 1  AI_STANCE_AGGRESSIVE = 2  AI_STANCE_DEFENSIVE = 3
AI_STANCE_STANDGROUND = 4  AI_STANCE_MISSION = 5
TRANSITION_LINEAR = 0  TRANSITION_EXP = 1  TRANSITION_LOG = 2  TRANSITION_SQUARE = 3
TRAFFIC_NODE_AVAIL = 1  TRAFFIC_NODE_NOSPAWN = 2  SEMAPHORE_STATE_GREEN = 0  SEMAPHORE_STATE_RED = 2
TRAFFIC_SETTING_VEHICLE_MOVING = 1  TRAFFIC_SETTING_VEHICLE_PARKING = 2  TRAFFIC_SETTING_HUMAN_MOVING = 3
TRAFFIC_SETTING_HUMAN_PARKING = 4
VEHICLE_LIGHTS_POSITION = 3  VEHICLE_LIGHTS_LOW_BEAM = 8  VEHICLE_LIGHTS_HIGH_BEAM = 16  VEHICLE_LIGHTS_FOG = 32
VEHICLE_LIGHTS_STOP = 4  VEHICLE_LIGHTS_LEFT = 64  VEHICLE_LIGHTS_RIGHT = 128  VEHICLE_LIGHTS_ALL = -1
IGNORE_CONTROLS_UP = 1  IGNORE_CONTROLS_DOWN = 2  IGNORE_CONTROLS_LEFT = 4  IGNORE_CONTROLS_RIGHT = 8
DEATH_NONE = 0  DEATH_LIGHT = 1  DEATH_HEAVY = 2  DEATH_RANGE = 3  DEATH_SPECIAL = 4  DEATH_EXPLOSION = 7
DEATH_KILL_COLLISION = 8  DEATH_SCRIPTED = 9  DEATH_BATARANG = 10
)";
// CHUDDisplay::m_faces: the portraits ShowMessage takes, in order.
const char* const kFaces[] = {"FACE_NONE", "FACE_MC", "FACE_BATMAN", "FACE_ALFRED", "FACE_MERCENARY", "FACE_MERCENARY2",
                              "FACE_MERCENARY3", "FACE_HOSTAGE", "FACE_HOSTAGE_FEMALE", "FACE_HOSTAGE_FEMALE2",
                              "FACE_HOSTAGE_MALE", "FACE_HOSTAGE_MALE2", "FACE_HOSTAGE_MALE_OLD", "FACE_BANE",
                              "FACE_OBJECTIVE", "FACE_POLICE_OFFICER", "FACE_LUCIUS", "FACE_SELINA", "FACE_GORDON",
                              "FACE_BLAKE", "FACE_JOHNBLAKE", "FACE_MIRANDATATE", "FACE_TURHAN", "FACE_BRUCE", "FACE_CRANE",
                              "FACE_BLINDPRISONER", "FACE_THUG2", "FACE_THUG4"};

struct Entry {
    const char* name;
    lua_CFunction fn;
    int upvalue = -1;   // a second upvalue: an integer (the variants' switch), or -1 for none
};

const Entry kTdkr[] = {
    {"SetQuestDisabled", api_set_quest<Quests::Disabled>},
    {"SetQuestEnabled", api_set_quest<Quests::Enabled>},
    {"SetQuestRunning", api_set_quest<Quests::Running>},
    {"SetQuestCompleted", api_set_quest<Quests::Completed>},
    {"SetQuestFailed", api_set_quest<Quests::Failed>},
    {"SetQuestAccepted", api_set_quest<Quests::Accepted>},
    {"SetQuestCanceled", api_set_quest<Quests::Canceled>},
    {"GetQuestStatus", api_get_quest_status},
    {"GetQuestsWithStatus", api_get_quests_with_status},
    {"RequireLoadLevel", api_require_load_level},
    {"GetCurrentLevel", api_get_current_level},
    {"AddGenericObjectOnMap", api_add_map_icon},
    {"RemoveGenericObjectFromMap", api_remove_map_icon},
    {"IsQuestDisabled", api_is_quest<Quests::Disabled>},
    {"IsQuestEnabled", api_is_quest<Quests::Enabled, Quests::Initialising>},
    {"IsQuestRunning", api_is_quest<Quests::Running, Quests::Accepted>},
    {"IsQuestCompleted", api_is_quest<Quests::Completed>},
    {"IsQuestFailed", api_is_quest<Quests::Failed, Quests::FailedRunning, Quests::FailedDone>},
    {"EnableMainStory", api_enable_main_story},
    {"StartMainStoryFromQuestId", api_start_main_story_from_quest},
    {"GetCurrentMainStoryQuestId", api_current_main_story_quest},
    {"NextMission", api_next_mission},
    {"ObjectiveAdd", api_objective_add},
    {"ObjectiveRemove", api_objective_remove},
    {"EnableControls", api_enable_controls},
    {"SetMoveControlEnable", api_set_control<&LevelLogic::Controls::move>},
    {"SetRotateControlEnable", api_set_control<&LevelLogic::Controls::rotate>},
    {"SetJumpControlEnable", api_set_control<&LevelLogic::Controls::jump>},
    {"SetCombatControlEnable", api_set_control<&LevelLogic::Controls::combat>},
    {"SetGrapnelControlEnable", api_set_control<&LevelLogic::Controls::grapnel>},
    {"SetCounterAttackControlEnable", api_set_control<&LevelLogic::Controls::counter>},
    {"SetGadgetWheelControlEnable", api_set_control<&LevelLogic::Controls::gadgets>},
    {"SetSilentKillControlEnable", api_set_control<&LevelLogic::Controls::silent>},
    {"IsCounterAttackControlEnabled", api_is_counter_enabled},
    {"IgnoreControls", api_ignore_controls},
    {"EnableControlGlow", api_enable_control_glow},
    {"HasCameraMoved", api_player_did<&LevelLogic::PlayerDid::look>},
    {"HasPlayerMoved", api_player_did<&LevelLogic::PlayerDid::move>},
    {"HasPlayerAttacked", api_player_did<&LevelLogic::PlayerDid::attack>},
    {"HasPlayerCounterd", api_player_did<&LevelLogic::PlayerDid::counter>},
    {"GetTime", api_get_time},
    {"GetTimeMs", api_get_time_ms},
    {"Random", api_random},
    {"GetDistFromMC", api_dist_from_mc},
    {"GetDistFromMC_Z", api_dist_from_mc_z},
    {"GetDistBetweenObjects", api_dist_between<false>},
    {"GetGroundDistBetweenObjects", api_dist_between<true>},
    {"ObjectGetPosition", api_object_get_position},
    {"ObjectSetPosition", api_object_set_position},
    {"IsLowEndDevice", api_false},
    {"SetWinScreen", api_set_win_screen},
    {"AddXP", api_add_xp},
    {"AddWayneTech", api_add_wayne_tech},
    {"IsCopAIEnabled", api_false},
    {"getMCWorldHeight", api_mc_world_height},
    {"IsCameraFacing", api_is_camera_facing},
    {"IsSoundPlaying", api_is_sound_playing},
    {"IsSoundIdPlaying", api_is_sound_playing},
    {"StartWeatherEffect", api_weather, 1},
    {"StopWeatherEffect", api_weather, 0},
    {"IsWeatherEffectActive", api_is_weather},
    {"SetGlobalVar", api_set_global_var},
    {"GetGlobalVar", api_get_global_var},
    {"RegisterGlobalVar", api_register_global_var},
    {"Enable", api_enable_object, 1},
    {"Disable", api_enable_object, 0},
    {"SetWeaponAmmo", api_set_weapon_ammo},
    {"GetWeaponAmmo", api_get_weapon_ammo},
    {"SetBatarangControlEnable", api_set_control<&LevelLogic::Controls::batarang>},
    {"ShowObject", api_show_object, 1},
    {"HideObject", api_show_object, 0},
    {"EnableShowObject", api_enable_show, 1},
    {"DisableHideObject", api_enable_show, 0},
    {"EnableInteractionForObject", api_enable_interaction, 1},
    {"DisableInteractionForObject", api_enable_interaction, 0},
    {"SetInteractionParams", api_set_interaction_params},
    {"GetCurrentInteractionObjectID", api_current_interaction},
    {"ActorSetState", api_actor_set_state},
    {"ActorGetState", api_actor_get_state},
    {"ActorIsInStateType", api_actor_is_in_state_type},
    {"ActorLookTo", api_actor_look_to},
    {"IsActorLookingAt", api_is_actor_looking_at},
    {"SpawnObject", api_spawn_object},
    {"KillActor", api_kill_actor},
    {"SetActorHealth", api_set_actor_health},
    {"ActorGrapnelEnable", api_actor_grapnel_enable},
    {"ActorTargetEnable", api_actor_grapnel_enable},
    {"NPCIsKilledByMC", api_npc_killed_by_mc},
    {"ActorIsAttacking", api_actor_is_attacking},
    {"ActorIsAttacked", api_actor_is_attacked},
    {"ActorFadeOut", api_actor_fade_out},
    {"SetOnGargoyle", api_set_on_gargoyle},
    {"SetGlideControlEnable", api_set_control<&LevelLogic::Controls::glide>},
    {"NPCSetSniper", api_npc_set_sniper},
    {"NPCSetCanBePulledAllTheWay", api_nothing},   // (every pull here lands at Batman)
    {"MissionFailedAutoReload", api_mission_failed_reload},
    // Weapons in the actors' hands (models on their hand nodes): not drawn yet.
    {"EnableWeapon", api_nothing},
    {"DisableWeapon", api_nothing},
    {"TakeOutWeapon", api_nothing},
    {"PutBackWeapon", api_nothing},
    {"GrapplingLockTarget", api_grappling_lock_target},
    {"PlayCameraAnim", api_play_camera_anim},
    {"CameraLookAt", api_camera_look_at},
    {"CameraFollowObject", api_camera_follow_object},
    {"CameraSwitchToPlayer", api_camera_to_player, 0},
    {"CameraSwitchToPlayerResetOrientation", api_camera_to_player, 1},
    {"EndCinematic", api_end_cinematic},
    {"StartGadgetZoom", api_start_gadget_zoom},
    {"GadgetZoomTarget", api_gadget_zoom_target},
    {"StopGadgetZoom", api_stop_gadget_zoom},
    {"SetHudState", api_set_hud_state},
    {"CameraSetFOV", api_camera_set_fov},
    // Texture preloads (EngineFunction_1), level-of-detail and collision hints, the dialog's talk
    // mode: nothing to do here.
    {"EngineFunction_1", api_nothing},
    {"SetForceBestLOD", api_nothing},
    {"EnableActorCollision", api_nothing},
    {"CameraShake", api_camera_shake},
    {"NPCAttackTarget", api_npc_attack_target},
    {"NPCSetStance", api_npc_set_stance},
    {"NPCSetAIBehavior", api_nothing},
    {"NPCCombatSet", api_nothing},
    {"SetObjectSlowMotion", api_nothing},
    {"ResetObjectSlowMotion", api_reset_object_slow_motion},
    {"ResetAllIgnoredControls", api_reset_ignored_controls},
    {"QTE_AutoGrapnel", api_auto_grapnel},
    {"NPCGoToWaypoint", api_npc_go_to_waypoint},
    {"NPCGoToPath", api_npc_go_to_waypoint},   // (a path is the chain from its first waypoint)
    {"StartCinematicObject", api_start_cinematic_object},
    {"EndCinematicObject", api_end_cinematic_object},
    {"PlayAnimation", api_play_animation},
    {"ActorPlayWeaponAnimation", api_nothing},
    {"NPCGoToWaypointWithNavmesh", api_npc_go_to_waypoint_navmesh},
    {"SetActorSpeedWalk", api_set_actor_speed, 0},
    {"SetActorSpeedRun", api_set_actor_speed, 1},
    {"SetSlowMotion", api_set_slow_motion},
    {"ResetSlowMotion", api_reset_slow_motion},
    // Tracking events, the tutorial's finger hint, cinematic-skip bookkeeping, streaming
    // hints: nothing to do here.
    {"BIT_AddEvent", api_nothing},
    {"_DesignMessage", api_nothing},
    {"StopTutorialFingerprint", api_nothing},
    {"EnableControlEvent", api_control_event, 1},
    {"DisableControlEvent", api_control_event, 0},
    {"SetObjectUpdateInCinematicSkip", api_nothing},
    {"SetAlwaysVisible", api_nothing},
    {"SetAlwaysUpdate", api_nothing},
    {"NPCSetTarget", api_npc_set_target},
    {"GetEnemiesInRange", api_get_enemies_in_range},
    {"GetFrameTime", api_get_frame_time},
    {"SpawnObjectNearActor", api_spawn_object_near_actor},
    {"EnableTalkMode", api_enable_talk_mode},
    {"PostProcessingEffectAdd", api_post_effect, 1},
    {"PostProcessingEffectRemove", api_post_effect, 0},
    // Gotham's traffic, crowds, patrols, monorail and trains, the side missions' manager, the
    // achievements and the flickering smart lights are not in this port yet: the scripts'
    // settings for them go nowhere.
    {"Traffic_SpawnerEnable", api_nothing},
    {"Traffic_SpawnerDisable", api_nothing},
    {"Traffic_SpawnerSetSetting", api_nothing},
    {"Traffic_StreetPatrolSet", api_nothing},
    {"Traffic_StreetPatrolRanges", api_nothing},
    {"Traffic_CrowdSetBehavior", api_nothing},
    {"Traffic_CrowdSetDensity", api_nothing},
    {"Traffic_PoolReset", api_nothing},
    {"Traffic_FreezeSemaphore", api_nothing},
    {"Traffic_UnfreezeSemaphore", api_nothing},
    {"StartSideMissionManager", api_nothing},
    {"StopSideMissionManager", api_nothing},
    {"SideMissionManagerQuestAdd", api_nothing},
    {"SideMissionManagerQuestRemove", api_nothing},
    {"EnableMonorailManager", api_nothing},
    {"StartMonorailManager", api_nothing},
    {"StopMonorailManager", api_nothing},
    {"AddMonorailLine", api_nothing},
    {"AddTrainLine", api_nothing},
    {"IncrementAchievementValue", api_nothing},
    {"SetAchievementVariable", api_nothing},
    {"GiveAchievement", api_nothing},
    {"SetEnableLight", api_nothing},
    {"IsDead", api_is_dead},
    {"ActorIsDetected", api_actor_is_detected},
    {"NPCGoToActorWithNavmesh", api_npc_go_to_actor},
    {"NPCGoToObject", api_npc_go_to_object},
    {"NPCReturnToSavedPosition", api_npc_return},
    {"NPCSetEnemyFlag", api_npc_set_enemy_flag},
    {"SetInvincible", api_set_invincible},
    {"LinkObject", api_link_object},
    {"UnlinkObject", api_unlink_object},
    {"NoiseEvent", api_noise_event},
    {"QTE_DisableControl", api_qte_disable},
    {"StartIntroStory", api_start_intro_story},
    {"HideObjectNode", api_hide_object_node},
    // Sound switches (footstep variants), muted state sounds, the awareness cone, the forced
    // strike target, a boss's hit limit, laser targeting and the electric turrets: not done here.
    {"AddSoundSwitch", api_nothing},
    {"NpcStateSoundMute", api_nothing},
    {"NPCAwareAngleSet", api_nothing},
    {"SetPlayerTarget", api_nothing},
    {"NPCSetMaxConsecutiveHitsTaken", api_nothing},
    {"ActorWeaponEnableTargeting", api_nothing},
    {"TurretSetActive", api_nothing},
    {"EnterBatPod", api_enter_batpod},
    {"ExitBatPod", api_exit_batpod},
    {"IsPlayerOnTheBatOrBatpod", api_on_batpod},
    {"QTE_StartNavigation", api_qte_navigation, 1},
    {"QTE_EndNavigation", api_qte_navigation, 0},
    {"QTE_IsInNavigation", api_qte_in_navigation},
    {"QTE_GrapplingSetTarget", api_qte_grappling_set_target},
    {"StartCombatMusic", api_start_combat_music},
    {"StopCombatMusic", api_stop_combat_music},
    {"ActorAttackableEnable", api_actor_attackable},
    // Occluders (portal culling helpers), the scripted global illumination, the cops' AI
    // switch, the hot spots and the jeep patrols: nothing to do here yet.
    {"HideOccluder", api_nothing},
    {"ShowOccluder", api_nothing},
    {"SetGlobalIlluminationScripted", api_nothing},
    {"ResetGlobalIllumination", api_nothing},
    {"DisableCopAI", api_nothing},
    {"EnableCopAI", api_nothing},
    {"EnableHotSpotManager", api_nothing},
    {"Traffic_JeepPatrolSet", api_nothing},
    {"Traffic_JeepPatrolRanges", api_nothing},
};

int api_unlock_mission(lua_State* L) { return api_log_mission(L); }

}  // namespace

void register_tdkr_api(LevelLogic& logic) {
    lua_State* L = logic.runtime_.state();
    for (const Entry& e : kTdkr) {
        lua_pushlightuserdata(L, &logic);
        if (e.upvalue >= 0) lua_pushinteger(L, e.upvalue);
        lua_pushcclosure(L, e.fn, e.upvalue >= 0 ? 2 : 1);
        lua_setglobal(L, e.name);
    }
    for (const char* name : {"UnlockMission", "RequireLoadChapterMission"}) {
        lua_pushlightuserdata(L, &logic);
        lua_pushstring(L, name);
        lua_pushcclosure(L, api_unlock_mission, 2);
        lua_setglobal(L, name);
    }
    logic.runtime_.load(kConstants, "script constants");
    for (size_t i = 0; i < std::size(kFaces); ++i) {
        lua_pushinteger(L, lua_Integer(i));
        lua_setglobal(L, kFaces[i]);
    }
}

}  // namespace tdkr
