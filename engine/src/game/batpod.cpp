#include "game/batpod.h"

#include <algorithm>
#include <cmath>

#include "game/level_logic.h"
#include "game/physics.h"
#include "game/player.h"

namespace tdkr {

namespace {

constexpr int kCompBatpodHandle = 0x0D1839C4;   // CComponentBatpodHandle
constexpr float kGravity = 20.0f;
constexpr float kRideHeight = 0.6f;     // the probe for walls, above the wheels
constexpr float kGroundUp = 1.5f, kGroundDown = 3.0f;   // the ground under it: from a step up to a drop

glm::vec3 heading(float yaw) {
    const float y = glm::radians(yaw);
    return {-std::sin(y), std::cos(y), 0};
}

}  // namespace

void Batpod::start(LevelLogic& logic, int object) {
    const LevelLogic::Object* o = logic.object(object);
    if (!o) return;
    object_ = object;
    position_ = o->position;
    yaw_ = o->yaw;
    speed_ = steer_ = fall_ = 0;
    if (const Value* h = logic.object_component(*o, kCompBatpodHandle); h && h->size() >= 8) {
        mass = std::max((*h)[0].as_float(), 1.0f);
        reverse_speed = (*h)[3].as_float();
        force = (*h)[6].as_float();
        brake_force = (*h)[7].as_float();
    }
}

void Batpod::place(LevelLogic& logic, const glm::vec3& position, float yaw) {
    LevelLogic::Object* o = logic.object(object_);
    if (!o) return;
    position_ = position, yaw_ = yaw;
    speed_ = fall_ = 0;
    o->yaw = yaw_;
    logic.teleport_object(*o, position_);
}

void Batpod::update(LevelLogic& logic, const CollisionWorld& world, const PlayerInput& in, float dt) {
    LevelLogic::Object* o = logic.object(object_);
    if (!o || dt <= 0) return;
    // Throttle, brakes, reverse (CBatpodHandleComponent::GetNewVelocity: force / mass, the
    // top speed each way).
    const float throttle = std::clamp(in.forward, -1.0f, 1.0f);
    if (throttle > 0) {
        speed_ += (speed_ < 0 ? force + brake_force : force) / mass * throttle * dt;
    } else if (throttle < 0) {
        if (speed_ > 0.5f) speed_ = std::max(speed_ + (force + brake_force) / mass * throttle * dt, 0.0f);
        else speed_ += brake_force / mass * throttle * dt;
    } else {
        speed_ -= std::copysign(std::min(std::abs(speed_), drag * dt), speed_);
    }
    speed_ = std::clamp(speed_, -reverse_speed, max_speed);
    // Steering on a radius that widens with speed (GetHorizontalSteeringBySpeed); backwards it
    // turns the other way.
    steer_ += (std::clamp(in.right, -1.0f, 1.0f) - steer_) * std::min(1.0f, dt * 6.0f);
    const float radius = min_radius + radius_per_speed * std::abs(speed_);
    yaw_ -= glm::degrees(steer_ * speed_ / radius * dt);
    // Forward, sliding along what it runs into (SmoothCollisionBehavior).
    glm::vec3 move = heading(yaw_) * speed_ * dt;
    const glm::vec3 probe = position_ + glm::vec3(0, 0, kRideHeight);
    const float len = glm::length(move);
    if (len > 1e-4f) {
        const glm::vec3 dir = move / len;
        float t = 1;
        glm::vec3 n;
        if (world.raycast(probe, probe + dir * (len + 1.0f), t, nullptr, &n)) {
            const float free = std::max(t * (len + 1.0f) - 1.0f, 0.0f);
            glm::vec3 slide = move - glm::vec3(n.x, n.y, 0) * glm::dot(move, glm::vec3(n.x, n.y, 0));
            float t2 = 1;
            if (world.raycast(probe, probe + slide * 1.5f, t2)) slide = glm::vec3(0);
            move = dir * free + slide;
            const glm::vec3 nh = glm::length(glm::vec2(n)) > 1e-3f ? glm::normalize(glm::vec3(n.x, n.y, 0)) : -dir;
            if (-glm::dot(nh, dir) > 0.7f) speed_ *= 0.3f;              // head on: it nearly stops
            else speed_ *= 1.0f - std::min(1.0f, 2.0f * dt);          // scraping along
        }
    }
    position_ += move;
    // The ground under the wheels (StayOnGround), else a fall.
    float t = 1;
    const glm::vec3 top = position_ + glm::vec3(0, 0, kGroundUp);
    if (world.raycast(top, position_ - glm::vec3(0, 0, kGroundDown), t) && fall_ <= 0.5f) {
        position_.z = top.z - t * (kGroundUp + kGroundDown);
        fall_ = 0;
    } else {
        fall_ += kGravity * dt;
        const glm::vec3 from = position_ + glm::vec3(0, 0, 0.3f), to = position_ - glm::vec3(0, 0, fall_ * dt);
        float tf = 1;
        if (world.raycast(from, to, tf)) position_.z = from.z + (to.z - from.z) * tf, fall_ = 0;   // landed
        else position_ = to;
    }
    o->yaw = yaw_;
    logic.teleport_object(*o, position_);
}

}  // namespace tdkr
