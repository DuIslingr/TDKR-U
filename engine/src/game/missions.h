// TDKR's missions: each level's properties (field 8) list the missions played in it, with the
// Lua that starts one (teleport, enable the quest), its names and the chapter it belongs to.
// GS_BaseMenu::StartNewGame takes STR_FPS_LEVEL_01_TUTORIAL; the chapter selection lists
// chapters 1-7 (7: free roam) and starts a mission by its index (MISSION_ACTIVATED).
#pragma once

#include <string>
#include <vector>

#include "assets/level.h"

namespace tdkr {

class Vfs;

struct Mission {
    std::string level;           // "GothamCity.lvc"
    std::string script;          // Lua run once the level has started
    std::string name;            // "STR_FPS_LEVEL_01_TUTORIAL"
    std::string menu_background; // "menu_bg_chap1_m1.swf"
    int chapter = 0, number = 0; // 0: not in the chapter selection
    std::string chapter_title;   // "STR_CHAPTER_SELECTION_1"
    std::string picture;         // "chap1_m1"
    std::string selection, index_title, description;   // STR_MISSION_SELECTION_1_1, STR_MISSION_INDEX_1, ...
    bool first = false;          // the new game's mission
};

// Every level's missions, chapters in story order first (chapter, number), then the others.
std::vector<Mission> load_missions(const Vfs& vfs, const Gol& gol);
// The new game's mission (STR_FPS_LEVEL_01_TUTORIAL), or -1.
int first_mission(const std::vector<Mission>& missions);

}  // namespace tdkr
