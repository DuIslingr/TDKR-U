#include "game/thebat.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "game/level_logic.h"
#include "game/physics.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompTheBatHandle = 0x222500F6;   // CComponentTheBatHandle
constexpr float kGravity = 9.81f;               // (the friction's force: mass * [7] * g)
constexpr float kSteerRate = 0.06f, kSteerBack = 0.18f;   // per ms, stick over and let go
constexpr float kBodyRate = 0.03f;              // degrees per ms the roll and pitch follow
constexpr float kNoSpeed = 0.001f;              // under it the nose stays where it is
constexpr float kStuckSpeed = 0.6f, kStuckKeep = 0.3f;   // m/s (the original's 2 cm a frame); what is left of the speed then
constexpr float kHalfWidth = 2.75f, kHalfHeight = 1.75f, kProbe = 4.5f;   // the hull (0x67C82E62: 5.5, 9, 3.5)

glm::vec2 heading(float yaw) {
    const float y = glm::radians(yaw);
    return {-std::sin(y), std::cos(y)};
}

}  // namespace

void TheBat::start(LevelLogic& logic, int object) {
    const LevelLogic::Object* o = logic.object(object);
    if (!o) return;
    object_ = object;
    position_ = o->position, yaw_ = o->yaw;
    velocity_ = thrust_ = drag_ = glm::vec3(0);
    roll_ = pitch_ = steer_ = steer_limit_ = turn_ = 0;
    steer_kick_ = 0, reversing_ = false;
    if (const Value* h = logic.object_component(*o, kCompTheBatHandle); h && h->size() >= 16) {
        mass = std::max((*h)[0].as_float(), 1.0f);
        max_speed = (*h)[1].as_float(), reverse_speed = (*h)[2].as_float();
        steer_units = std::max((*h)[3].as_float(), 1.0f);
        engine = (*h)[4].as_float(), reverse = (*h)[5].as_float(), lift = (*h)[6].as_float();
        friction = (*h)[7].as_float(), handling = (*h)[9].as_float(), climb_speed = (*h)[10].as_float();
        ceiling = (*h)[12].as_float(), floor = (*h)[13].as_float();
        roll_limit = (*h)[14].as_float(), pitch_limit = (*h)[15].as_float();
        if (h->size() >= 33) {
            lock_cone = (*h)[18].as_float(), rocket_gap = (*h)[20].as_float();
            blast_radius = (*h)[21].as_float(), blast_damage = (*h)[22].as_float(), blast_edge = (*h)[23].as_float();
            rocket_speed = (*h)[24].as_float(), rocket_range = (*h)[25].as_float();
            blast_effect_ = (*h)[26].s, trail_effect_ = (*h)[27].s;
            muzzles_[0] = (*h)[30].s, muzzles_[1] = (*h)[31].s;
        }
    }
    rockets_.assign(20, {});
    next_rocket_ = 0, right_muzzle_ = false, target_ = -1;
    rocket_wait_ = 0;
}

void TheBat::place(LevelLogic& logic, const glm::vec3& position) {
    LevelLogic::Object* o = logic.object(object_);
    if (!o) return;
    position_ = position;
    velocity_ = thrust_ = drag_ = glm::vec3(0);
    logic.teleport_object(*o, position_);
}

void TheBat::set_input(const Input& in, float ms) {
    const glm::vec2 stick(std::clamp(in.right, -1.0f, 1.0f), std::clamp(in.forward, -1.0f, 1.0f));
    const float length = glm::length(stick);
    const glm::vec2 dir = length > 0 ? stick / length : glm::vec2(0);
    // Along the nose, at the stick's share of the engine; any stick at all at least balances the
    // friction, so that a turn with the throttle off keeps its speed.
    const float full = reversing_ ? reverse : engine;
    const glm::vec2 nose = heading(yaw_);
    float force = full * dir.y * std::abs(stick.y);
    if (stick != glm::vec2(0)) {
        const float hold = mass * friction * kGravity;
        if (std::abs(force) < hold) force = stick.y < 0 ? -hold : hold;
    }
    force = std::clamp(force, -full, full);
    thrust_ = glm::vec3(nose * force, in.up == in.down ? 0.0f : in.up ? lift : -lift);
    // The steering builds up while the stick is over and the craft moves, and runs back faster.
    // (The nose is left of a positive turn; astern the stick works the other way.)
    const float side = (reversing_ ? 1.0f : -1.0f) * dir.x;
    const bool moving = speed() > 0;
    if (side > 0.01f && moving && steer_kick_ >= 0) steer_ += ms * kSteerRate;
    else if (side < -0.01f && moving && steer_kick_ < 1) steer_ -= ms * kSteerRate;
    else if (steer_ > 0) steer_ = std::max(steer_ - ms * kSteerBack, 0.0f);
    else if (steer_ < 0) steer_ = std::min(steer_ + ms * kSteerBack, 0.0f);
    steer_ += float(steer_kick_);
    steer_kick_ = 0;
    steer_limit_ = std::max(std::min(steer_limit_, std::abs(steer_)), std::abs(stick.x) * steer_units);
    steer_ = std::clamp(steer_, -steer_limit_, steer_limit_);
    turn_ = ms * kSteerRate * std::abs(steer_) * side * (handling / steer_units);
}

void TheBat::turn_body(float ms) {
    // The body follows the velocity; going astern it keeps its nose against it.
    const glm::vec2 flat(velocity_);
    const float flat_speed = glm::length(flat);
    reversing_ = flat_speed >= kNoSpeed && glm::dot(flat, heading(yaw_)) < 0;
    if (flat_speed >= kNoSpeed) {
        const glm::vec2 to = reversing_ ? -flat : flat;
        yaw_ = glm::degrees(std::atan2(-to.x, to.y));
    }
    // It banks into a turn with its speed, and pitches with the thrust (the forces' share of
    // the nose); a third of the bank astern.
    const float top = reversing_ ? reverse_speed : max_speed;
    float roll = -steer_ * flat_speed / top * (reversing_ ? roll_limit / steer_units / 3 : roll_limit / steer_units);
    roll = std::clamp(roll, -roll_limit, roll_limit);
    const float push = glm::length(glm::vec2(thrust_));
    const float along = push > 0 ? -glm::dot(glm::vec2(thrust_) / push, heading(yaw_)) : 0.0f;
    float pitch = glm::length(velocity_) < 1.0f ? 0.0f : std::clamp(along * pitch_limit, -pitch_limit, pitch_limit);
    if (glm::length(velocity_) < 1.0f) roll = 0;
    auto follow = [&](float& value, float to) {
        const float step = ms * kBodyRate, gap = to - value;
        value = std::abs(gap) > 1 ? value + std::copysign(step, gap) : to;
    };
    follow(pitch_, pitch);
    follow(roll_, roll);
}

void TheBat::physics(float dt) {
    const float a = dt / mass;
    // Friction cannot push it backwards; then the thrust.
    glm::vec3 v = velocity_ + drag_ * a;
    if (glm::dot(v, velocity_) < 0) v = glm::vec3(0);
    v += thrust_ * a;
    glm::vec2 flat(v);
    const float top = reversing_ ? reverse_speed : max_speed;
    if (const float s = glm::length(flat); s > top) flat *= top / s;
    const float z = std::clamp(v.z, -climb_speed, climb_speed);
    // The turn: the velocity itself swings round (the body then follows it).
    const float t = glm::radians(turn_), c = std::cos(t), s = std::sin(t);
    velocity_ = glm::vec3(c * flat.x - s * flat.y, s * flat.x + c * flat.y, z);
    const float speed3 = glm::length(velocity_);
    drag_ = speed3 > 0 ? -velocity_ / speed3 * (mass * friction * kGravity) : glm::vec3(0);
}

void TheBat::fly(const CollisionWorld& world, float dt) {
    glm::vec3 move = velocity_ * dt;
    const float length = glm::length(move);
    if (length < 1e-5f) return;
    const glm::vec3 dir = move / length;
    // The hull's middle and corners along the way (CActorBaseComponent::CheckForCollisions).
    const glm::vec3 side = glm::length(glm::vec2(dir)) > 1e-3f ? glm::normalize(glm::vec3(-dir.y, dir.x, 0)) : glm::vec3(1, 0, 0);
    const glm::vec3 offsets[] = {glm::vec3(0), side * kHalfWidth, -side * kHalfWidth, glm::vec3(0, 0, kHalfHeight),
                                 glm::vec3(0, 0, -kHalfHeight)};
    float nearest = 2;
    glm::vec3 normal(0);
    for (const glm::vec3& at : offsets) {
        float t = 1;
        glm::vec3 n(0);
        if (world.raycast(position_ + at, position_ + at + dir * (length + kProbe), t, nullptr, &n) && t < nearest)
            nearest = t, normal = n;
    }
    if (nearest <= 1 && nearest * (length + kProbe) - kProbe < length) {
        ++hits_, last_normal_ = normal;
        // SmoothCollisionBehavior: along the wall, slowed by how square it was hit, the nose
        // turned away from it.
        const float free = std::max(nearest * (length + kProbe) - kProbe, 0.0f);
        const glm::vec3 slide = move - normal * glm::dot(move, normal) * 1.01f;
        move = dir * free + slide * (1 - free / length);
        const float moved = glm::length(move);
        if (moved < kStuckSpeed * dt) {
            velocity_ *= kStuckKeep;
        } else {
            const glm::vec2 before = glm::normalize(glm::vec2(dir) + glm::vec2(1e-5f, 0));
            const glm::vec2 after = glm::normalize(glm::vec2(move) + glm::vec2(1e-5f, 0));
            const float cosine = glm::dot(before, after);
            if (cosine < 0.99f) {
                const float cross = before.x * after.y - before.y * after.x;
                const float kick = (dt * 1000) * kSteerRate * steer_units * (1 - cosine) * (reversing_ ? -1.0f : 1.0f);
                steer_kick_ = int(cross > 0 ? kick : -kick);
                velocity_ = glm::normalize(move) * glm::length(velocity_) * ((cosine + 2) / 3);
            }
        }
    }
    position_ += move;
    position_.z = std::clamp(position_.z, floor, ceiling);
}

glm::vec3 TheBat::nose() const {
    const float p = glm::radians(pitch_);
    return glm::vec3(heading(yaw_) * std::cos(p), std::sin(p));
}

int TheBat::find_target(LevelLogic& logic) const {
    // The nearest enemy within the cone and the range. (The original also takes a few object
    // types and the closest destroyable along the nose; the missions so far need neither.)
    int best = -1;
    float best_distance = rocket_range;
    const glm::vec3 forward = nose();
    const float cone = std::cos(glm::radians(lock_cone * 0.5f));
    for (const auto& [id, o] : logic.objects()) {
        if (id == object_ || !o.targetable || !o.attackable || !logic.is_enemy(id)) continue;
        const glm::vec3 to = o.position - position_;
        const float distance = glm::length(to);
        if (distance < best_distance && distance > 0 && glm::dot(to / distance, forward) > cone) best = id, best_distance = distance;
    }
    return best;
}

void TheBat::launch(LevelLogic& logic) {
    const LevelLogic::Object* bat = logic.object(object_);
    const LevelLogic::Object* aim = logic.object(target_);
    if (!bat || !aim || rockets_.empty()) return;
    size_t slot = rockets_.size();
    for (size_t i = 0; i < rockets_.size(); ++i)
        if (!rockets_[i].flying) {
            slot = i;
            break;
        }
    if (slot == rockets_.size()) slot = next_rocket_ = (next_rocket_ + 1) % rockets_.size();
    Rocket& r = rockets_[slot];
    if (r.trail >= 0) logic.scene.particles().stop(r.trail);
    glm::vec3 muzzle = position_;
    if (bat->index != std::numeric_limits<size_t>::max())
        muzzle = glm::vec3(logic.scene.object_matrix(bat->index, muzzles_[right_muzzle_ ? 1 : 0])[3]);
    const glm::vec3 to = aim->position - muzzle;
    r.flying = true, r.position = r.from = muzzle, r.target = target_;
    r.direction = glm::length(to) > 1e-3f ? glm::normalize(to) : nose();
    r.trail = logic.scene.particles().start(trail_effect_, glm::translate(glm::mat4(1), muzzle));
    rocket_wait_ = rocket_gap;
    right_muzzle_ = !right_muzzle_;
    ++fired_;
}

void TheBat::explode(LevelLogic& logic, size_t rocket, const std::function<void(int, float)>& hurt) {
    Rocket& r = rockets_[rocket];
    r.flying = false;
    if (r.trail >= 0) logic.scene.particles().stop(r.trail);
    r.trail = -1;
    logic.scene.particles().start(blast_effect_, glm::translate(glm::mat4(1), r.position));
    // CAIController::SplashDamage: from the edge's damage at the radius to the full at the middle.
    std::vector<std::pair<int, float>> hit;
    for (const auto& [id, o] : logic.objects()) {
        if (id == object_ || o.dead || !(logic.is_enemy(id) || logic.is_helicopter(id)) || !o.attackable) continue;
        const float distance = glm::distance(o.position, r.position);
        if (distance < blast_radius) hit.push_back({id, blast_edge + (blast_damage - blast_edge) * (1 - distance / blast_radius)});
    }
    for (const auto& [id, damage] : hit) hurt(id, damage);
}

void TheBat::update_weapons(LevelLogic& logic, bool fire, float dt, const std::function<void(int, float)>& hurt) {
    if (object_ < 0) return;
    rocket_wait_ = std::max(rocket_wait_ - dt * 1000, 0.0f);
    target_ = find_target(logic);
    if (fire && target_ >= 0 && rocket_wait_ <= 0) launch(logic);
    for (size_t i = 0; i < rockets_.size(); ++i) {
        Rocket& r = rockets_[i];
        if (!r.flying) continue;
        const glm::vec3 before = r.position;
        r.position += r.direction * rocket_speed * dt;
        const LevelLogic::Object* aim = r.target >= 0 ? logic.object(r.target) : nullptr;
        if (aim && aim->dead) aim = nullptr;
        if (aim) {
            const glm::vec3 to = aim->position - r.position;
            if (glm::length(to) > 1e-3f) r.direction = glm::normalize(to);
        }
        logic.scene.particles().move(r.trail, glm::translate(glm::mat4(1), r.position));
        // The step against the target's upright capsule (0x651BDAA: radius, height), its middle
        // being near enough here.
        if (aim) {
            float radius = 2, height = 2;
            if (const Value* c = logic.object_component(*aim, 0x0651BDAA); c && c->size() >= 2) radius = (*c)[0].as_float(), height = (*c)[1].as_float();
            const glm::vec3 middle = aim->position + glm::vec3(0, 0, height * 0.5f), step = r.position - before;
            const float along = std::clamp(glm::dot(middle - before, step) / std::max(glm::dot(step, step), 1e-6f), 0.0f, 1.0f);
            if (glm::distance(before + step * along, middle) <= radius + height * 0.5f) {
                explode(logic, i, hurt);
                continue;
            }
        }
        if (glm::distance(r.position, r.from) > rocket_range) explode(logic, i, hurt);
    }
}

void TheBat::clear_rockets(LevelLogic& logic) {
    for (Rocket& r : rockets_) {
        if (r.flying && r.trail >= 0) logic.scene.particles().stop(r.trail);
        r = {};
    }
    target_ = -1;
}

void TheBat::update(LevelLogic& logic, const CollisionWorld& world, const Input& in, float dt) {
    LevelLogic::Object* o = logic.object(object_);
    if (!o || dt <= 0) return;
    const float ms = dt * 1000;
    set_input(in, ms);
    turn_body(ms);
    physics(dt);
    fly(world, dt);
    o->yaw = yaw_;
    logic.teleport_object(*o, position_);
    if (o->index != std::numeric_limits<size_t>::max()) logic.scene.set_object_pose(o->index, position_, yaw_, pitch_, roll_);
}

}  // namespace tdkr
