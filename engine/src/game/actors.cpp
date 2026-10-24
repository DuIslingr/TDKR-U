// Actors moving on their own: TDKR's waypoint goals (LevelLogic::walk_to).
#include <cmath>
#include <limits>

#include "game/level_logic.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompStateSet = 0x5441A117;   // CStateSetComponent: [[name, type mask, ?, loop, ..., [27] clips], ...]

// The first clip of a state in the type's or its parents' state sets ("" if none).
std::string state_clip_of(const Gol& gol, int type, const std::string& name, bool& loop, int depth = 0) {
    auto g = gol.find(type);
    if (g == gol.end() || depth > 8) return "";
    if (auto t = g->second.templates.find(kCompStateSet); t != g->second.templates.end() && t->second.size() > 0)
        for (const Value& s : t->second[0].list)
            if (s.size() >= 28 && s[0].s == name && s[27].size() > 0 && s[27][0].size() > 0) {
                loop = s[3].i != 0;
                return s[27][0][0].s;
            }
    for (int p : g->second.parents)
        if (std::string c = state_clip_of(gol, p, name, loop, depth + 1); !c.empty()) return c;
    return "";
}

}  // namespace

LevelLogic::Walker& LevelLogic::walker(int id) {
    for (Walker& w : walkers_)
        if (w.id == id) return w;
    walkers_.push_back({id});
    return walkers_.back();
}

void LevelLogic::actor_state(Object& o, const std::string& state) {
    if (o.state == state || o.index >= scene.level().objects.size()) return;
    bool loop = true;
    const std::string clip = state_clip_of(gol, scene.level().objects[o.index].type, state, loop);
    if (clip.empty()) return;
    o.state = state;
    scene.play_clip(o.index, clip, loop);
}

void LevelLogic::walk_to(int id, int waypoint, bool run) {
    Object* o = object(id);
    if (!o || !object(waypoint)) return;
    Walker& w = walker(id);
    w.target = waypoint, w.run = run;
    if (trace) log("actor " + std::to_string(id) + " goes to waypoint " + std::to_string(waypoint));
}

void LevelLogic::stop_walking(int id) {
    for (Walker& w : walkers_)
        if (w.id == id) w.target = -1;
}

bool LevelLogic::walking(int id) const {
    for (const Walker& w : walkers_)
        if (w.id == id) return w.target >= 0;
    return false;
}

void LevelLogic::set_actor_speed(int id, float speed, bool run) {
    Walker& w = walker(id);
    (run ? w.run_speed : w.walk_speed) = speed;
}

void LevelLogic::update_walkers(float dt) {
    for (size_t k = 0; k < walkers_.size(); ++k) {
        Walker& w = walkers_[k];
        Object* o = object(w.id);
        if (!o || w.target < 0 || o->dead) continue;
        const Object* wp = object(w.target);
        if (!wp) {
            w.target = -1;
            continue;
        }
        const glm::vec3 to = wp->position - o->position;
        const float dist = glm::length(to), step = (w.run ? w.run_speed : w.walk_speed) * dt;
        if (dist <= std::max(step, 0.05f)) {   // reached: its OnReach, then the next on the chain
            o->position = wp->position;
            const int reached = w.target;
            w.target = next_waypoint(reached);
            if (w.target == reached) w.target = -1;
            start_script("Obj" + std::to_string(reached) + "_OnReach", w.id);
            if (w.target < 0) actor_state(*o, "Idle");
        } else {
            o->position += to * (step / dist);
            if (glm::length(glm::vec2(to)) > 1e-3f) o->yaw = glm::degrees(std::atan2(-to.x, to.y));
            actor_state(*o, w.run ? "Run" : "Walk");
        }
        if (o->index != std::numeric_limits<size_t>::max()) scene.set_object_pose(o->index, o->position, o->yaw);
    }
}

}  // namespace tdkr
