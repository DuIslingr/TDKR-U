// The save file (Application::EncryptAndSave's checkpoint and global data): where the story
// stands, kept between runs in the preferences folder. Every checkpoint the scripts take
// (SaveGame) writes the level and the checkpoint, which holds the story (the quests of every
// level, the main story's place, the script globals); a level change (RequireLoadLevel)
// writes the level entered, its spawn point and the story so far; starting a mission
// (New Game, Chapter Select: GS_BaseMenu::StartChapter) writes the mission. Continue picks it
// up; Reset Progress (Application::ResetAllProgress) removes it.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/combat.h"
#include "game/quests.h"

namespace tdkr {

struct SaveGame {
    std::string level;                 // "levels/<file>.lvc"
    int mission = -1;                  // the mission being played (Game::missions), -1 none
    std::vector<uint8_t> checkpoint;   // LevelLogic::checkpoint_data(); empty: from the level's start
    int spawn = -1;                    // without a checkpoint: the spawn point a level change entered at
    bool has_arsenal = false;          // without a checkpoint: what the player carries in
    Combat::Saved arsenal;
    bool has_story = false;            // without a checkpoint: the story so far (a level change)
    Quests::Saved quests;
    std::map<std::string, int> global_vars;

    static std::string default_path();   // empty if there is no preferences folder
    bool write(const std::string& path) const;
    static bool read(const std::string& path, SaveGame& out);
    static void remove(const std::string& path);
};

// Combat::Saved as bytes and back (also inside a checkpoint).
void write_combat(std::vector<uint8_t>& out, const Combat::Saved& s);
bool read_combat(const std::vector<uint8_t>& in, size_t& at, Combat::Saved& s);

}  // namespace tdkr
