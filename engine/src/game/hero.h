// TDKR's third-person Batman: the player's model, its locomotion clips and the follow camera.
// The movement itself stays Player's (ellipsoid on the collision, camera-relative input).
#pragma once

#include <cstddef>
#include <string>

#include <glm/glm.hpp>

#include "game/player.h"

namespace tdkr {

class Scene;
class CollisionWorld;

class Hero {
public:
    std::string model = "batman.bdae", animations = "batman_anims.bdae";
    // Follow camera: orbits a pivot above the feet at the view's yaw/pitch, pulled in by walls.
    float camera_distance = 3.0f, pivot_height = 1.7f, camera_side = 0.0f;   // close behind, as in the gameplay footage
    float turn_rate = 720.0f;   // degrees per second the body turns towards where it moves

    // Place the model at the player (after Player::spawn).
    void start(Scene& scene, const Player& player);
    // Follow the player: pose, facing and locomotion clip from this step's movement.
    void update(Scene& scene, const Player& player, const PlayerInput& in, float dt);
    // The camera position for a view with this yaw/pitch (degrees, game convention).
    glm::vec3 camera_position(const Player& player, float yaw, float pitch, const CollisionWorld& world) const;

    bool active() const { return object_ != kNone; }
    const std::string& clip() const { return clip_; }

private:
    static constexpr size_t kNone = size_t(-1);
    size_t object_ = kNone;
    std::string clip_;
    float facing_ = 0;          // body yaw, degrees
    glm::vec3 last_feet_{0};
    bool have_last_ = false;
    bool was_on_ground_ = true;
    float air_time_ = 0;
    void play(Scene& scene, const std::string& clip, bool loop);
};

}  // namespace tdkr
