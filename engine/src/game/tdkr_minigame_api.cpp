// TDKR's minigames as the scripts start them: the hacking game (StartPipeGame) and the
// button over an object (QTE_EnableControl, EnableMiniGameObject) that has Batman's grapnel
// pull it down. The game (app/game.cpp) shows them through the HUD and reports back.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "audio/audio.h"
#include "game/level_logic.h"

namespace tdkr {

namespace {

LevelLogic& ctx(lua_State* L) { return *static_cast<LevelLogic*>(lua_touserdata(L, lua_upvalueindex(1))); }
int int_arg(lua_State* L, int i) { return int(lua_tointeger(L, i)); }
std::string str_arg(lua_State* L, int i) { return lua_isstring(L, i) ? lua_tostring(L, i) : ""; }

// StartPipeGame(win, lose, grid, size, skip price, ms, start row, end row): the grid is a
// digit per cell, size x size (CHUDDisplay::StartPipeMinigame: miniGame.init(grid, size, size,
// start row, end row, ms) in the Hacking frame, its music m_hack_minigame).
int api_start_pipe_game(lua_State* L) {
    LevelLogic& c = ctx(L);
    LevelLogic::Hack& h = c.hack;
    h.active = true;
    ++h.serial;
    h.win = str_arg(L, 1), h.lose = str_arg(L, 2), h.grid = str_arg(L, 3);
    h.size = int_arg(L, 4), h.price = int_arg(L, 5), h.ms = int_arg(L, 6);
    h.start_row = int_arg(L, 7), h.end_row = int_arg(L, 8);
    c.hud_state = "Hacking";
    c.enable_controls(false);   // (CHUDDisplay +0x4c8 0x100: the game takes no input; ResetAfterHack gives it back)
    if (c.audio) {
        c.audio->play("sfx_minigame_screen_appear");
        c.audio->play("sfx_minigame_load_bar");
    }
    if (c.trace) c.log("hack: " + h.grid + " (" + std::to_string(h.size) + "x" + std::to_string(h.size) + ", " + std::to_string(h.ms) + " ms)");
    return 0;
}

// QTE_EnableControl(object, ms, win, lose).
int api_qte_enable_control(lua_State* L) {
    LevelLogic& c = ctx(L);
    if (!c.object(int_arg(L, 1))) return 0;
    c.button_qte = {int_arg(L, 1), float(lua_tonumber(L, 2)) / 1000.0f, str_arg(L, 3), str_arg(L, 4)};
    if (c.audio) c.audio->play("sfx_qte_appear");
    if (c.trace) c.log("qte: button at " + std::to_string(c.button_qte.object));
    return 0;
}

// EnableMiniGameObject(object[, on]): the object the next QTE's grapnel pulls
// (PullableObjectComponent::Activated, PlayerComponent's minigame target); -1: all of them.
int api_enable_minigame_object(lua_State* L) {
    LevelLogic& c = ctx(L);
    const int id = int_arg(L, 1);
    const bool on = lua_gettop(L) < 2 || lua_toboolean(L, 2);
    if (id == -1) {
        if (!on) c.minigame_object = -1;
    } else if (on) {
        c.minigame_object = id;
    } else if (c.minigame_object == id) {
        c.minigame_object = -1;
    }
    return 0;
}

}  // namespace

void register_tdkr_minigame_api(LevelLogic& logic) {
    for (auto [name, fn] : {std::pair{"StartPipeGame", api_start_pipe_game}, std::pair{"QTE_EnableControl", api_qte_enable_control},
                            std::pair{"EnableMiniGameObject", api_enable_minigame_object}})
        logic.runtime_.register_function(name, fn, &logic);
}

}  // namespace tdkr
