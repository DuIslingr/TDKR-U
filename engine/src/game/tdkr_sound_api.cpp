// TDKR's sound script functions (VoxSoundManager through libKRHP.so's natives): music events,
// sounds with interactive states, 3D sounds and events at objects, voice-over lines.
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "audio/audio.h"
#include "game/level_logic.h"
#include "game/player.h"

namespace tdkr {

namespace {

LevelLogic& ctx(lua_State* L) { return *static_cast<LevelLogic*>(lua_touserdata(L, lua_upvalueindex(1))); }
std::string str_arg(lua_State* L, int i) { return lua_isstring(L, i) ? lua_tostring(L, i) : ""; }
float num_arg(lua_State* L, int i) { return float(lua_tonumber(L, i)); }

// Where an object is for its sounds: Batman (0) or the object.
bool where(LevelLogic& c, int id, glm::vec3& at) {
    if (id == 0) {
        at = c.player.center;
        return true;
    }
    const LevelLogic::Object* o = c.object(id);
    if (o) at = o->position;
    return o != nullptr;
}

// StartMusicEvent(event[, loop, fade ms]): a random track of a music event (PlayMusicEvent);
// the music playing gives way.
int api_start_music_event(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    const std::string name = str_arg(L, 1);
    if (c.audio->event_playing(name)) return 0;
    c.audio->stop_music(1000);
    Audio::Params p;
    p.fade_in_ms = num_arg(L, 3);
    if (lua_gettop(L) >= 2) p.loop = lua_toboolean(L, 2);
    c.audio->play_event(name, p);
    return 0;
}
// StartSoundWithState(sound, state[, loop, fade ms, ...]): an interactive sound in a state
// (PlaySoundWithState); one already playing just changes state.
int api_start_sound_with_state(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    const std::string name = str_arg(L, 1), state = str_arg(L, 2);
    if (!c.audio->playing(name)) {
        Audio::Params p;
        p.fade_in_ms = num_arg(L, 4);
        if (c.audio->play(name, p) < 0) return 0;
    }
    c.audio->set_state(name, state);
    return 0;
}
int api_set_sound_state(lua_State* L) {   // SetSoundState(sound, state)
    if (LevelLogic& c = ctx(L); c.audio) c.audio->set_state(str_arg(L, 1), str_arg(L, 2));
    return 0;
}
// StartSoundAtObjectNode(sound, object, max distance[, fade ms, loop, ...]): a 3D sound at an
// object (a dynamic emitter that follows it). StartEventAtObjectNode(event, object, ...): an
// event's random sound there.
template <bool Event>
int api_sound_at_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    glm::vec3 at;
    const int id = int(lua_tointeger(L, 2));
    if (!c.audio || !where(c, id, at)) return 0;
    Audio::Params p;
    p.position = &at;
    if (!Event) {
        p.fade_in_ms = num_arg(L, 4);
        if (lua_gettop(L) >= 5) p.loop = lua_toboolean(L, 5);
    }
    const int h = Event ? c.audio->play_event(str_arg(L, 1), p) : c.audio->play(str_arg(L, 1), p);
    if (h >= 0) c.object_sounds[id].push_back(h);
    return 0;
}
// StopCurrentSFXAtObject(object, ms): the sounds started at it stop.
int api_stop_sounds_at_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.audio) return 0;
    auto it = c.object_sounds.find(int(lua_tointeger(L, 1)));
    if (it == c.object_sounds.end()) return 0;
    for (int h : it->second) c.audio->stop(h, num_arg(L, 2));
    c.object_sounds.erase(it);
    return 0;
}
int api_stop_named(lua_State* L) {   // StopSoundAtWaypoint(sound, waypoint, ms)
    if (LevelLogic& c = ctx(L); c.audio) c.audio->stop(str_arg(L, 1), num_arg(L, 3));
    return 0;
}
int api_kill_music(lua_State* L) {   // KillMusic(ms)
    if (LevelLogic& c = ctx(L); c.audio) c.audio->stop_music(num_arg(L, 1));
    return 0;
}
// ShowMessage(id, face, ms): the line on screen (the dialog box or the subtitle) and its
// voice-over: the sound named after the string without "STR_" (CHUDDisplay::AddDialogLine).
int api_show_message(lua_State* L) {
    LevelLogic& c = ctx(L);
    const std::string id = str_arg(L, 1);
    float seconds = 3;
    if (lua_gettop(L) >= 3 && lua_isnumber(L, 3)) seconds = num_arg(L, 3) / 1000.0f;
    c.message(id, seconds);
    c.send_event_later("MessageEnd", seconds);
    if (c.audio && id.rfind("STR_", 0) == 0 && c.audio->table().sound(id.substr(4))) {
        Audio::Params p;
        p.loop = 0;
        c.audio->play(id.substr(4), p);
    }
    return 0;
}
int api_play_ambience(lua_State* L) {   // PlayAmbience(preset)
    ctx(L).play_ambience(str_arg(L, 1));
    return 0;
}
int api_nothing(lua_State*) { return 0; }

struct Entry {
    const char* name;
    lua_CFunction fn;
};
const Entry kSound[] = {
    {"StartMusicEvent", api_start_music_event},
    {"StartSoundWithState", api_start_sound_with_state},
    {"SetSoundState", api_set_sound_state},
    {"StartSoundAtObjectNode", api_sound_at_object<false>},
    {"StartEventAtObjectNode", api_sound_at_object<true>},
    {"StopCurrentSFXAtObject", api_stop_sounds_at_object},
    {"StopSoundAtWaypoint", api_stop_named},
    {"KillMusic", api_kill_music},
    {"ShowMessage", api_show_message},
    {"PlayAmbience", api_play_ambience},
    // Reverb presets, footstep surfaces, the combat music switch, the sound switches' reset,
    // the debug sound log: not here yet.
    {"ChangeReverbPreset", api_nothing},
    {"SetWalkingOnWetSurface", api_nothing},
    {"SetCombatMusicEnabled", api_nothing},
    {"ResetSoundSwitches", api_nothing},
    {"DebugSound", api_nothing},
};

}  // namespace

void register_tdkr_sound_api(LevelLogic& logic) {
    lua_State* L = logic.runtime_.state();
    for (const Entry& e : kSound) {
        lua_pushlightuserdata(L, &logic);
        lua_pushcclosure(L, e.fn, 1);
        lua_setglobal(L, e.name);
    }
}

}  // namespace tdkr
