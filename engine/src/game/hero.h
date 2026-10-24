// TDKR's third-person Batman: the player's model, its locomotion clips and the follow camera.
// The movement itself stays Player's (ellipsoid on the collision, camera-relative input).
#pragma once

#include <cstddef>
#include <string>

#include <glm/glm.hpp>

#include "assets/edges.h"
#include "game/player.h"

namespace tdkr {

class Scene;
class CollisionWorld;

class Hero {
public:
    std::string model = "batman.bdae", animations = "batman_anims.bdae";
    // Follow camera: orbits a pivot above the feet at the view's yaw/pitch, pulled in by walls.
    float camera_distance = 3.0f, pivot_height = 1.7f, camera_side = 0.0f;   // close behind, as in the gameplay footage
    // Gliding, the camera eases back and up to keep the spread cape in view (as in the footage).
    float glide_camera_distance = 7.0f, glide_pivot_height = 3.0f, camera_ease = 2.0f;   // ease: per second
    float glide_view_pitch = -20.0f;   // gliding, the view tilts down onto Batman
    // The view's pitch for the player's look pitch (tilted down while gliding).
    float view_pitch(float pitch) const;
    float turn_rate = 720.0f;   // degrees per second the body turns towards where it moves

    // Place the model at the player (after Player::spawn).
    void start(Scene& scene, Player& player);   // also turns on the player's glide
    // Follow the player: pose, facing and locomotion clip from this step's movement.
    void update(Scene& scene, const Player& player, const PlayerInput& in, float dt);
    // The camera position for a view with this yaw/pitch (degrees, game convention).
    glm::vec3 camera_position(const Player& player, float yaw, float pitch, const CollisionWorld& world) const;

    // Fire the grapnel along the view (from `eye`, looking along `forward`) at the best edge:
    // within grapple_range and grapple_cone degrees of the aim, above the feet. Returns whether
    // one was found (then the player flies to it).
    bool try_grapple(Player& player, const glm::vec3& eye, const glm::vec3& forward) const;
    float grapple_range = 50.0f, grapple_cone = 25.0f, grapple_inset = 0.7f;
    size_t grapple_edges() const { return edges_.size(); }

    bool active() const { return object_ != kNone; }
    size_t object() const { return object_; }
    const std::string& clip() const { return clip_; }

private:
    static constexpr size_t kNone = size_t(-1);
    size_t object_ = kNone;
    std::string clip_;
    float facing_ = 0;          // body yaw, degrees
    glm::vec3 last_feet_{0};
    bool have_last_ = false;
    bool was_on_ground_ = true;
    bool was_gliding_ = false, was_grappling_ = false;
    uint32_t grapples_seen_ = 0;
    std::vector<GrappleEdge> edges_;
    void show_cape(Scene& scene, bool spread);
    float air_time_ = 0;
    float glide_blend_ = 0;     // 0 on foot .. 1 gliding, for the camera
    void play(Scene& scene, const std::string& clip, bool loop);
};

}  // namespace tdkr
