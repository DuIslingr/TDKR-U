// Actors moving on their own: TDKR's waypoint goals (LevelLogic::walk_to).
#include <algorithm>
#include <cmath>
#include <vector>
#include <limits>

#include "game/level_logic.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompStateSet = 0x5441A117;   // CStateSetComponent: [[name, type mask, ?, loop, ..., [27] clips], ...]

}  // namespace

bool find_actor_state(const Gol& gol, int type, const std::string& name, ActorState& out) {
    // Breadth by depth: the type's own set, then its parents' in order (and theirs).
    std::vector<int> level{type};
    for (int depth = 0; depth < 8 && !level.empty(); ++depth) {
        std::vector<int> next;
        for (int t : level) {
            auto g = gol.find(t);
            if (g == gol.end()) continue;
            if (auto st = g->second.templates.find(kCompStateSet); st != g->second.templates.end() && st->second.size() > 0)
                for (const Value& s : st->second[0].list) {
                    if (s.size() < 28 || s[0].s != name) continue;
                    out.mask = s[1].i;
                    out.loop = s[3].i != 0;
                    out.clips.clear();
                    for (const Value& clip : s[27].list)
                        if (clip.size() > 0 && clip[0].type == Value::String) out.clips.push_back(clip[0].s);
                    return !out.clips.empty();
                }
            next.insert(next.end(), g->second.parents.begin(), g->second.parents.end());
        }
        level = std::move(next);
    }
    return false;
}

LevelLogic::Walker& LevelLogic::walker(int id) {
    for (Walker& w : walkers_)
        if (w.id == id) return w;
    walkers_.push_back({id});
    return walkers_.back();
}

bool LevelLogic::actor_state(Object& o, const std::string& state, bool restart) {
    if ((o.state == state && !restart) || o.index >= scene.level().objects.size()) return o.state == state;
    ActorState st;
    if (!find_actor_state(gol, object_type(o), state, st)) return false;
    o.state = state;
    static uint32_t pick = 0;
    scene.play_clip(o.index, st.clips[(pick++) % st.clips.size()], st.loop);
    return true;
}

void LevelLogic::walk_to(int id, int waypoint, bool run) {
    Object* o = object(id);
    if (!o || !object(waypoint)) return;
    Walker& w = walker(id);
    w.target = waypoint, w.run = run, w.patrol = -1, w.spawned = false;
    if (trace) log("actor " + std::to_string(id) + " goes to waypoint " + std::to_string(waypoint));
}

void LevelLogic::start_patrol(int id, int waypoint) {
    if (!object(id) || !object(waypoint)) return;
    Walker& w = walker(id);
    w.target = waypoint, w.run = false, w.to_point = false, w.patrol = -1, w.spawned = true;
}

void LevelLogic::stop_walking(int id) {
    for (Walker& w : walkers_)
        if (w.id == id) w.target = -1, w.to_point = false, w.patrol = -1, w.spawned = false;
}

void LevelLogic::walk_to_point(int id, const glm::vec3& point, bool run) {
    if (!object(id)) return;
    Walker& w = walker(id);
    w.target = -1, w.to_point = true, w.point = point, w.run = run, w.patrol = -1, w.spawned = false;
}

void LevelLogic::patrol_area(int id, int center, float radius) {
    if (!object(id) || !object(center)) return;
    Walker& w = walker(id);
    w.target = -1, w.to_point = false, w.run = false;
    w.patrol = center, w.patrol_radius = std::max(radius, 0.5f), w.patrol_wait = 0;
    if (trace) log("actor " + std::to_string(id) + " patrols around " + std::to_string(center));
}

bool LevelLogic::walking(int id) const {
    for (const Walker& w : walkers_)
        if (w.id == id) return w.target >= 0 || w.to_point;
    return false;
}

void LevelLogic::set_actor_speed(int id, float speed, bool run) {
    Walker& w = walker(id);
    (run ? w.run_speed : w.walk_speed) = speed;
}

// A walker's state: its type's own (Ranged_Walk, ...), else the plain one.
void LevelLogic::walk_state(Object& o, size_t which, const char* fallback) {
    const std::string own = default_state(o, which);
    if (own.empty() || !actor_state(o, own)) actor_state(o, fallback);
}

void LevelLogic::update_walkers(float dt) {
    for (size_t k = 0; k < walkers_.size(); ++k) {
        Walker& w = walkers_[k];
        Object* o = object(w.id);
        if (!o || o->dead) continue;
        // Patrolling: a pause, then the next point within the area (none while it fights).
        if (w.patrol >= 0 && w.target < 0 && !w.to_point && !attack_targets.count(w.id))
            if (const Object* c = object(w.patrol); c && (w.patrol_wait -= dt) <= 0) {
                static uint32_t seed = 12345;
                auto rnd = [] { return float((seed = seed * 1103515245u + 12345u) >> 8 & 0xffff) / 65535.0f; };
                const float a = rnd() * 6.2832f, r = std::sqrt(rnd()) * w.patrol_radius;
                w.to_point = true, w.point = glm::vec3(c->position.x + std::cos(a) * r, c->position.y + std::sin(a) * r, o->position.z);
                w.patrol_wait = 2.0f + 3.0f * rnd();
            }
        if (w.spawned && attack_targets.count(w.id)) w.target = -1, w.to_point = w.spawned = false;
        if (w.target < 0 && !w.to_point) continue;
        const Object* wp = w.to_point ? nullptr : object(w.target);
        if (!w.to_point && !wp) {
            w.target = -1;
            continue;
        }
        const glm::vec3 to = (w.to_point ? w.point : wp->position) - o->position;
        const float dist = glm::length(to), step = (w.run ? w.run_speed : w.walk_speed) * dt;
        if (w.to_point && dist <= std::max(step, 0.05f)) {   // there: it stands
            o->position = w.point;
            w.to_point = false;
            walk_state(*o, 1, "Idle");
        } else if (dist <= std::max(step, 0.05f)) {   // reached: its OnReach, then the next on the chain
            o->position = wp->position;
            const int reached = w.target;
            w.target = next_waypoint(reached);
            if (w.target == reached) w.target = -1;
            start_script("Obj" + std::to_string(reached) + "_OnReach", w.id);
            if (w.target < 0) walk_state(*o, 1, "Idle");
        } else {
            o->position += to * (step / dist);
            if (glm::length(glm::vec2(to)) > 1e-3f) o->yaw = glm::degrees(std::atan2(-to.x, to.y));
            walk_state(*o, w.run ? 6 : 5, w.run ? "Run" : "Walk");
        }
        if (o->index != std::numeric_limits<size_t>::max()) scene.set_object_pose(o->index, o->position, o->yaw);
    }
}

}  // namespace tdkr
