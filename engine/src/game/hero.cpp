#include "game/hero.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/physics.h"
#include "game/scene.h"

namespace tdkr {

namespace {

// Locomotion clips of batman_anims.bdae (921 clips; these are the plain moves).
constexpr const char* kIdle = "_001_idle_stand";
constexpr const char* kWalk = "_007_walk";
constexpr const char* kRun = "_008_run";
constexpr const char* kSprint = "_022_sprint";
constexpr const char* kJumpStart = "_010_run_jump_start";
constexpr const char* kJumpLoop = "_011_run_jump_loop";
constexpr const char* kFallLoop = "_044_fall_loop";
constexpr const char* kLand = "_013_run_jump_land";

glm::vec3 feet_of(const Player& p) { return p.center - glm::vec3(0, 0, p.radius.z); }

// Shortest signed difference b - a in degrees.
float angle_delta(float a, float b) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    return d;
}

}  // namespace

void Hero::start(Scene& scene, const Player& player) {
    // Levels also place Batman as script-driven stand-ins (Gotham: five NPCs and an animated
    // object, for cinematics); the player's Batman replaces them.
    scene.hide_models(model);
    object_ = scene.spawn_model(model, false, animations, kIdle);
    clip_ = kIdle;
    facing_ = player.yaw;
    have_last_ = false;
    if (!active()) return;
    // batman.bdae has two capes: CAPE spread for gliding, CAPE_down hanging; on foot only the latter.
    scene.set_node_visible(object_, "CAPE", false);
    scene.set_object_pose(object_, feet_of(player), facing_);
}

void Hero::play(Scene& scene, const std::string& clip, bool loop) {
    if (clip == clip_) return;
    if (scene.play_clip(object_, clip, loop)) clip_ = clip;
}

void Hero::update(Scene& scene, const Player& player, const PlayerInput& in, float dt) {
    if (!active()) return;
    const glm::vec3 feet = feet_of(player);
    const glm::vec2 moved = have_last_ ? glm::vec2(feet - last_feet_) : glm::vec2(0);
    last_feet_ = feet, have_last_ = true;
    const float speed = dt > 0 ? glm::length(moved) / dt : 0.0f;

    // Face the direction of travel (the input's, relative to the view), turning at turn_rate.
    if (in.forward != 0 || in.right != 0) {
        const float target = player.yaw + glm::degrees(std::atan2(-in.right, in.forward));
        const float d = angle_delta(facing_, target);
        facing_ += std::clamp(d, -turn_rate * dt, turn_rate * dt);
    }

    // Clip: airborne (jump up, then fall), landing, else by ground speed.
    if (!player.on_ground) {
        air_time_ += dt;
        if (was_on_ground_) play(scene, player.vertical_speed > 0 ? kJumpStart : kFallLoop, false);
        else if (scene.clip_finished(object_) || air_time_ > 0.6f) play(scene, air_time_ > 0.6f ? kFallLoop : kJumpLoop, true);
    } else {
        if (!was_on_ground_ && air_time_ > 0.4f) {
            play(scene, kLand, false);
        } else if (clip_ != kLand || scene.clip_finished(object_) || speed > 1.0f) {
            if (speed < 0.3f) play(scene, kIdle, true);
            else if (speed < player.walk_speed * 0.6f) play(scene, kWalk, true);
            else if (player.rushing) play(scene, kSprint, true);
            else play(scene, kRun, true);
        }
        air_time_ = 0;
    }
    was_on_ground_ = player.on_ground;
    scene.set_object_pose(object_, feet, facing_);
}

glm::vec3 Hero::camera_position(const Player& player, float yaw, float pitch, const CollisionWorld& world) const {
    const glm::vec3 pivot = feet_of(player) + glm::vec3(0, 0, pivot_height);
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    const glm::vec3 forward{-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
    const glm::vec3 right{std::cos(y), std::sin(y), 0};
    const glm::vec3 wanted = pivot - forward * camera_distance + right * camera_side;
    float t = 1;
    glm::vec3 normal;
    if (world.raycast(pivot, wanted, t, nullptr, &normal)) {   // a wall between: come in front of it
        return pivot + (wanted - pivot) * std::max(t - 0.1f / camera_distance, 0.0f) + normal * 0.05f;
    }
    return wanted;
}

}  // namespace tdkr
