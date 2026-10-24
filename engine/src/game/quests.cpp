#include "game/quests.h"

#include <algorithm>

#include "game/level_logic.h"

namespace tdkr {

namespace {

constexpr int kCompQuest = 0x55A6393B;       // CQuestComponent: id, title, debrief, name, ?, ?, script, ...
constexpr int kCompMainStory = 0x5863BD59;   // CMainStoryQuestsComponent: [[quest, ?, level, InitQuest,
                                             //   Briefing, SkipMission, reminder s, OnScriptEvent, ?], ...]

}  // namespace

void Quests::load(const Level& level, const Gol& gol) {
    std::vector<Quest> before;
    if (carry_over) before = quests_;
    quests_.clear();
    if (!carry_over) last_completed_ = -1;
    for (const LevelObject& o : level.objects) {
        auto c = o.components.find(kCompQuest);
        if (c == o.components.end() || !o.base || c->second.size() < 3) continue;
        Quest q;
        q.id = c->second[0].i;
        q.object = o.base->id;
        q.title = c->second[1].s, q.debrief = c->second[2].s;
        for (const Quest& b : before)
            if (b.id == q.id) q.status = b.status, q.ended = b.ended;
        quests_.push_back(q);
    }
    // The MainStory type's story quests; its scripts are Templ<type>_<n>_<event>
    // (CMainStoryQuestsComponent::Load).
    story_.clear();
    story_type_ = -1;
    if (!carry_over) {
        story_on_ = false;
        story_index_ = story_started_ = -1;
        briefed_ = finished_ = advance_ = false;
        reminder_ms_ = -1;
    }
    carry_over = false;
    for (const auto& [id, type] : gol) {
        auto t = type.templates.find(kCompMainStory);
        if (t == type.templates.end() || t->second.size() < 1) continue;
        story_type_ = id;
        for (const Value& e : t->second[0].list) {
            if (e.size() < 9 || e[0].i < 0) continue;
            story_.push_back({e[0].i, e[6].as_float()});
            for (size_t k : {3, 4, 5, 7})
                if (e[k].type == Value::String && !e[k].s.empty()) logic_.runtime_.load(e[k].s, type.name);
        }
        break;
    }
    logic_.log("quests: " + std::to_string(quests_.size()) + ", main story " + std::to_string(story_.size()));
}

Quests::Quest* Quests::quest(int id) {
    for (Quest& q : quests_)
        if (q.id == id) return &q;
    return nullptr;
}

const Quests::Quest* Quests::find(int id) const {
    for (const Quest& q : quests_)
        if (q.id == id) return &q;
    return nullptr;
}

int Quests::status(int id) const {
    const Quest* q = find(id);
    return q ? q->status : Disabled;
}

void Quests::run(const Quest& q, const char* event) {
    logic_.start_script("Obj" + std::to_string(q.object) + "_" + event, q.object);
}

bool Quests::running(const Quest& q, const char* event) const {
    return logic_.runtime_.running("Obj" + std::to_string(q.object) + "_" + event);
}

// CQuestManager::SetQuestStatus: the quest's object is enabled; a completed story quest
// hands over to the next.
void Quests::set_status(int id, int st) {
    Quest* q = quest(id);
    if (!q) {
        logic_.log("quest " + std::to_string(id) + ": no such quest");
        return;
    }
    if (LevelLogic::Object* o = logic_.object(q->object)) logic_.enable(*o, true);
    if (st == Completed) last_completed_ = id;
    if (q->status == st) return;
    if (logic_.trace) logic_.log("quest " + std::to_string(id) + ": status " + std::to_string(q->status) + " -> " + std::to_string(st));
    q->status = st;
    q->ended = false;
    if (story_on_ && st == Completed) story_completed(id);
}

// CQuestManager::Update: each quest (CQuestComponent::Update), then the main story.
void Quests::update(float dt) {
    for (Quest& q : quests_) {
        if (q.ended) continue;
        switch (q.status) {
            case Enabled:
                run(q, "InitQuest");
                set_status(q.id, Initialising);
                break;
            case Initialising:
                if (!running(q, "InitQuest")) run(q, "Enabled");
                break;
            case Running:
                if (!running(q, "Accepted")) run(q, "Running");
                break;
            case Failed:
                set_status(q.id, FailedRunning);
                run(q, "Failed");
                break;
            case FailedRunning:
                if (!running(q, "Failed")) set_status(q.id, FailedDone);
                break;
            case Completed:
                run(q, "Completed");
                q.ended = true;
                break;
            case Canceled:
                set_status(q.id, CanceledRunning);
                run(q, "Canceled");
                break;
            case CanceledRunning:
                if (!running(q, "Canceled")) set_status(q.id, CanceledDone);
                break;
            case Accepted:
                run(q, "Accepted");
                set_status(q.id, Running);
                break;
            default:
                break;
        }
    }
    if (story_on_) update_story(dt * 1000.0f);
}

std::string Quests::story_function(size_t index, const char* event) const {
    return "Templ" + std::to_string(story_type_) + "_" + std::to_string(index) + "_" + event;
}

// CQuestManager::EnableMainStory / StartStoryFromQuestId.
void Quests::start_story(int quest_id) {
    story_on_ = true;
    if (story_.empty()) return;
    if (quest_id >= 0) {
        auto it = std::find_if(story_.begin(), story_.end(), [&](const StoryQuest& s) { return s.quest == quest_id; });
        if (it == story_.end()) return;
        story_index_ = int(it - story_.begin());
        story_started_ = -1, briefed_ = false, finished_ = false;   // (ResetComponent)
    }
    if (story_index_ < 0) story_index_ = 0;
    story_entered(quest_id >= 0);
}

void Quests::respawned() {
    if (!story_on_ || story_index_ < 0 || size_t(story_index_) >= story_.size()) return;
    Quest* q = quest(story_[size_t(story_index_)].quest);
    const int st = q ? q->status : Disabled;
    if (st != Disabled && st != Failed && st != FailedRunning && st != FailedDone && st != Canceled && st != CanceledRunning &&
        st != CanceledDone)
        return;
    if (q) q->status = Disabled, q->ended = false;
    story_started_ = -1, finished_ = false;
    story_entered(true);
}

int Quests::story_quest() const {
    return story_index_ >= 0 && size_t(story_index_) < story_.size() ? story_[size_t(story_index_)].quest : -1;
}

// CMainStoryQuestsComponent::OnNewStoryQuestEnabled: the story quest's InitQuest and, once,
// its Briefing; its reminder starts counting.
void Quests::story_entered(bool) {
    const size_t i = size_t(story_index_);
    if (story_started_ != story_index_) {
        logic_.start_script(story_function(i, "InitQuest"), -1);
        if (!briefed_) logic_.start_script(story_function(i, "Briefing"), -1), briefed_ = true;
    }
    if (!finished_) reminder_ms_ = story_[i].reminder * 1000.0f;
    if (logic_.trace) logic_.log("main story: quest " + std::to_string(story_[i].quest));
}

// CMainStoryQuestsComponent::OnQuestCompleted.
void Quests::story_completed(int id) {
    if (std::none_of(story_.begin(), story_.end(), [&](const StoryQuest& s) { return s.quest == id; })) return;
    briefed_ = false;
    advance_ = true;
    reminder_ms_ = -1;
}

// CMainStoryQuestsComponent::Update.
void Quests::update_story(float ms) {
    if (story_index_ < 0 || story_.empty()) return;
    if (reminder_ms_ > 0) {
        reminder_ms_ -= ms;
        if (reminder_ms_ <= 0) logic_.start_script(story_function(size_t(story_index_), "OnScriptEvent"), -1);
    }
    if (advance_) {
        if (size_t(story_index_) + 1 < story_.size()) {
            ++story_index_;
            story_entered(true);
        } else {
            finished_ = true;
        }
        advance_ = false;
    }
    if (story_started_ != story_index_ && !logic_.runtime_.running(story_function(size_t(story_index_), "InitQuest")))
        story_started_ = story_index_;
}

}  // namespace tdkr
