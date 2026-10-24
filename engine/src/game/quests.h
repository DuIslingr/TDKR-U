// TDKR's quests (CQuestManager, CQuestComponent, CMainStoryQuestsComponent).
//
// A quest is a level object with a quest component: its id, title and debrief strings, and a
// script defining Obj<object>_InitQuest / _Enabled / _Accepted / _Running / _Completed /
// _Failed / _Canceled. Scripts move a quest through its statuses (SetQuestEnabled,
// SetQuestAccepted, ...) and each update runs the function of the status it is in
// (CQuestComponent::Update):
//   Enabled   InitQuest, then Initialising
//   Initialising  once InitQuest has finished, Enabled (again whenever it is not running)
//   Accepted  Accepted, then Running
//   Running   once Accepted has finished, Running (again whenever it is not running)
//   Failed    Failed, then FailedRunning; once that has finished, FailedDone
//   Completed Completed (once)
//   Canceled  Canceled, then CanceledRunning; once that has finished, CanceledDone
//
// The main story (the MainStory type's CMainStoryQuestsComponent, started by EnableMainStory)
// is the list of story quests in order: entering one runs its Templ<type>_<n>_InitQuest (and
// _Briefing), which enables the quest; when that quest completes, the next one's turn comes.
// A story quest left too long runs its reminder (_OnScriptEvent) after its time.
#pragma once

#include <string>
#include <vector>

#include "assets/level.h"

namespace tdkr {

class LevelLogic;

class Quests {
public:
    enum Status : int {
        Disabled = 0, Enabled = 1, Initialising = 2, Running = 3, Failed = 4, FailedRunning = 5, FailedDone = 6,
        Completed = 7, Canceled = 9, CanceledRunning = 10, CanceledDone = 11, Accepted = 12,
    };
    struct Quest {
        int id = 0, object = 0;          // quest id; the quest object's id (its script's Obj<id>_)
        std::string title, debrief;      // string ids
        int status = Disabled;
        bool ended = false;              // Completed has run: no more updates until its status changes
    };

    explicit Quests(LevelLogic& logic) : logic_(logic) {}
    // The level's quest objects and the main story's quests (their script chunks are already
    // loaded with the level; the story's are loaded here).
    void load(const Level& level, const Gol& gol);
    // CQuestManager outlives levels: with `carry_over` set, the next load keeps the statuses of
    // the quests it finds again and the main story where it was (RequireLoadLevel); a new game
    // or mission clears it.
    bool carry_over = false;
    void update(float dt);

    // CQuestManager::SetQuestStatus / GetQuestStatus.
    void set_status(int id, int status);
    int status(int id) const;
    const Quest* find(int id) const;
    const std::vector<Quest>& all() const { return quests_; }
    int last_completed() const { return last_completed_; }

    // EnableMainStory / StartMainStoryFromQuestId (-1: where it stands).
    void start_story(int quest_id = -1);
    int story_quest() const;   // GetCurrentMainStoryQuestId: -1 when none
    bool story_on() const { return story_on_; }

private:
    Quest* quest(int id);
    void run(const Quest& q, const char* event);
    bool running(const Quest& q, const char* event) const;
    void story_entered(bool tracked);
    void story_completed(int id);
    void update_story(float ms);
    std::string story_function(size_t index, const char* event) const;

    LevelLogic& logic_;
    std::vector<Quest> quests_;
    int last_completed_ = -1;
    // CMainStoryQuestsComponent: its story quests (quest id, reminder after seconds), the
    // current one, the one whose InitQuest last started, and its timers.
    struct StoryQuest {
        int quest = -1;
        float reminder = 0;   // seconds
    };
    std::vector<StoryQuest> story_;
    int story_type_ = -1;
    bool story_on_ = false;
    int story_index_ = -1, story_started_ = -1;
    bool briefed_ = false, finished_ = false, advance_ = false;
    float reminder_ms_ = -1;
};

}  // namespace tdkr
