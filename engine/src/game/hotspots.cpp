// The crime hot spots and districts (CQuestManager's hot spots, CDistrictComponent,
// CCrimeHotSpotComponent). A hot spot is a circle with a quest. GenerateDistrict lights some of
// a district's; walking into a lit one starts its quest (CheckActiveHotSpot), whose scripts run
// the fight and MarkCurrentHotspotCleared. Quests like 1101 wait for GetDistrictCompletion.
#include <algorithm>
#include <cstdlib>

#include "game/level_logic.h"
#include "game/player.h"
#include "game/quests.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompDistrict = 0x1963F0C0;    // CDistrictComponent
constexpr int kCompHotspot = 0x1402FD4B;     // CCrimeHotSpotComponent: [0] its quest
constexpr int kCompCylinder = 0x749126C3;    // [0] its radius
constexpr int kStatusEnabled = 1, kStatusCanceled = 9;   // SetQuestStatus's

}  // namespace

// In level order a district is followed by its zones, each followed by its hot spots
// (CTriggerComponent::AddCrimeZone: the zone being loaded takes them to its district).
void LevelLogic::load_hotspots() {
    hotspots_.clear(), districts_.clear();
    active_hotspot_ = -1, crime_marker_ = -1;
    int district = -1;
    for (const LevelObject& o : scene.level().objects) {
        if (!o.base) continue;
        if (o.components.count(kCompDistrict)) district = o.base->id, districts_[district];
        const auto spot = o.components.find(kCompHotspot);
        if (spot == o.components.end() || spot->second.size() < 1 || district < 0) continue;
        HotSpot h;
        h.id = o.base->id, h.quest = spot->second[0].i, h.at = glm::vec2(o.base->position);
        if (auto c = o.components.find(kCompCylinder); c != o.components.end() && c->second.size() > 0)
            h.radius = c->second[0].as_float() * o.base->scale.x;
        const int flags = hotspot_flags_.count(h.id) ? hotspot_flags_[h.id] : 0;
        h.enabled = flags & 1, h.cleared = flags & 2;
        districts_[district].push_back(h.id);
        hotspots_[h.id] = h;
    }
}

void LevelLogic::remember_hotspot(const HotSpot& h) { hotspot_flags_[h.id] = (h.enabled ? 1 : 0) | (h.cleared ? 2 : 0); }

void LevelLogic::restore_hotspots(const std::map<int, int>& flags, bool manager) {
    hotspot_flags_ = flags, hotspot_manager_ = manager;
    active_hotspot_ = -1;
    for (auto& [id, h] : hotspots_) {
        const auto f = flags.find(id);
        h.enabled = f != flags.end() && (f->second & 1), h.cleared = f != flags.end() && (f->second & 2), h.canceled = false;
    }
}

// CDistrictComponent::GenerateDistrictCrimeZones: `count` of its hot spots lit, picked at random
// and (CQuestManager::IsHotSpotOverlaping) not over another lit one if it can be helped.
void LevelLogic::generate_district(int district, int count) {
    const auto d = districts_.find(district);
    if (d == districts_.end()) return;
    for (int id : d->second) {
        HotSpot& h = hotspots_[id];
        h.enabled = h.cleared = false;
        remember_hotspot(h);
    }
    for (int n = 0; n < std::min(count, int(d->second.size())); ++n) {
        std::vector<int> open, free;
        for (int id : d->second) {
            const HotSpot& h = hotspots_[id];
            if (h.enabled) continue;
            open.push_back(id);
            bool over = false;
            for (int other : d->second) {
                const HotSpot& o = hotspots_[other];
                over = over || (o.enabled && glm::distance(o.at, h.at) < o.radius + h.radius);
            }
            if (!over) free.push_back(id);
        }
        const std::vector<int>& from = free.empty() ? open : free;
        if (from.empty()) break;
        HotSpot& h = hotspots_[from[size_t(std::rand()) % from.size()]];
        h.enabled = true, h.cleared = false;
        remember_hotspot(h);
    }
    if (trace) log("district " + std::to_string(district) + ": " + std::to_string(count) + " crime zones");
}

// CDistrictComponent::GetDistrictCompletion: the lit ones cleared, as a percentage; -1: no such district.
int LevelLogic::district_completion(int district) const {
    const auto d = districts_.find(district);
    if (d == districts_.end()) return -1;
    int enabled = 0, cleared = 0;
    for (int id : d->second) {
        const HotSpot& h = hotspots_.at(id);
        enabled += h.enabled, cleared += h.cleared;
    }
    return enabled > 0 ? cleared * 100 / enabled : 0;
}

int LevelLogic::current_district() const {   // (the original's zones' triggers: here the nearest hot spot's)
    int best = -1;
    float nearest = 1e30f;
    for (const auto& [district, spots] : districts_)
        for (int id : spots)
            if (const float d = glm::distance(hotspots_.at(id).at, glm::vec2(player.center)); d < nearest) nearest = d, best = district;
    return best;
}

void LevelLogic::enable_hotspot(int id, bool on) {
    if (auto h = hotspots_.find(id); h != hotspots_.end()) {
        h->second.enabled = on;
        remember_hotspot(h->second);
    }
}

// CQuestManager::EnableHotSpotManager: off, the one going is called off.
void LevelLogic::enable_hotspot_manager(bool on) {
    hotspot_manager_ = on;
    if (!on && active_hotspot_ >= 0) {
        HotSpot& h = hotspots_[active_hotspot_];
        quests.set_status(h.quest, kStatusCanceled);
        h.canceled = true;
        active_hotspot_ = -1;
    }
}

void LevelLogic::hotspot_cleared() {   // MarkCurrentHotspotCleared
    if (active_hotspot_ < 0) return;
    HotSpot& h = hotspots_[active_hotspot_];
    h.cleared = true;
    ++hotspots_cleared;
    remember_hotspot(h);
    const int id = h.id;
    std::erase_if(objective_markers, [id](const ObjectiveMarker& m) { return m.id == id; });
    if (crime_marker_ == id) crime_marker_ = -1;
    if (trace) log("hot spot " + std::to_string(id) + " cleared");
    active_hotspot_ = -1;
}

void LevelLogic::hotspot_canceled() {   // MarkCurrentHotspotCanceled: not again until he has left it
    if (active_hotspot_ >= 0) {
        HotSpot& h = hotspots_[active_hotspot_];
        h.canceled = true;
        // --auto-play cannot sneak: a test's shortcut past the stealth missions, logged.
        if (auto_skip_failed_hotspots && ++h.failures >= 2) {
            h.cleared = true;
            remember_hotspot(h);
            std::erase_if(objective_markers, [&h](const ObjectiveMarker& m) { return m.id == h.id; });
            if (crime_marker_ == h.id) crime_marker_ = -1;
            log("[auto-play] hot spot " + std::to_string(h.id) + " SKIPPED after 2 failures (quest " + std::to_string(h.quest) + ")");
        }
    }
    active_hotspot_ = -1;
}

// AddObjectiveOnCrimeArea(_, kind): the marker on the nearest one still to do.
void LevelLogic::mark_nearest_hotspot(const std::string& kind) {
    const HotSpot* best = nullptr;
    float nearest = 1e30f;
    for (const auto& [id, h] : hotspots_)
        if (const float d = glm::distance(h.at, glm::vec2(player.center)); h.enabled && !h.cleared && d < nearest) nearest = d, best = &h;
    if (!best) return;
    const int id = best->id;
    std::erase_if(objective_markers, [this](const ObjectiveMarker& m) { return m.id == crime_marker_; });
    objective_markers.push_back({id, kind, ""});
    crime_marker_ = id;
}

void LevelLogic::unmark_hotspot() {   // RemoveObjectiveOnCrimeArea
    std::erase_if(objective_markers, [this](const ObjectiveMarker& m) { return m.id == crime_marker_; });
    crime_marker_ = -1;
}

// CQuestManager::CheckActiveHotSpot: he is in a lit one (one of them at random): its quest starts.
void LevelLogic::update_hotspots() {
    if (!hotspot_manager_ || active_hotspot_ >= 0) return;
    std::vector<int> inside;
    const glm::vec2 at(player.center);
    for (auto& [id, h] : hotspots_) {
        const bool in = glm::distance(h.at, at) < h.radius;
        if (h.canceled && !in) h.canceled = false;
        if (in && h.enabled && !h.cleared && !h.canceled) inside.push_back(id);
    }
    if (inside.empty()) return;
    const HotSpot& h = hotspots_[inside[size_t(std::rand()) % inside.size()]];
    active_hotspot_ = h.id;
    quests.set_status(h.quest, kStatusEnabled);
    if (trace) log("hot spot " + std::to_string(h.id) + ": quest " + std::to_string(h.quest));
}

}  // namespace tdkr
