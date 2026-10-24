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
// Gliding (Batman_02_Flying states): the cape opens from a run or a fall, the glide loops,
// a steep dive plunges; on the ground it folds back.
constexpr const char* kRunToGlide = "_063_run_to_glide_parachute";
constexpr const char* kFallToGlide = "_128_fall_to_glide_parachute";
constexpr const char* kGlide = "_059_glide_parachute_idle";
constexpr const char* kPlunge = "_503_plunge_idle";
constexpr const char* kGlideLand = "_064_glide_parachute_land";
constexpr const char* kGlideToRun = "_127_glide_parachute_to_run";
// The grapnel (Batman_03_Grapnel states): shoot up, fly, climb onto the edge.
constexpr const char* kGrapnelShoot = "_118_idle_to_grapnel_up";
constexpr const char* kGrapnelToFly = "_122_grapnel_up_to_fly";
constexpr const char* kGrapnelFly = "_123_fly_idle";
constexpr const char* kGrapnelClimb = "_158_gp_fly_to_idle_stand2";

glm::vec3 feet_of(const Player& p) { return p.center - glm::vec3(0, 0, p.radius.z); }

// Shortest signed difference b - a in degrees.
float angle_delta(float a, float b) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    return d;
}

}  // namespace

void Hero::start(Scene& scene, Player& player) {
    player.can_glide = true;
    was_gliding_ = was_grappling_ = false;
    grapples_seen_ = player.grapples;
    edges_.clear();
    const std::string& level = scene.level_name();
    const std::string edge_file = to_lower(level.substr(0, level.rfind('.'))) + ".edge";
    if (scene.vfs().exists(edge_file)) {
        try {
            edges_ = load_grapple_edges(scene.vfs().read(edge_file));
        } catch (const FormatError&) {
            edges_.clear();
        }
    }
    // Levels also place Batman as script-driven stand-ins (Gotham: five NPCs and an animated
    // object, for cinematics); the player's Batman replaces them.
    scene.hide_models(model);
    object_ = kNone;
    acting_ = false;
    if (to_lower(level).find("l_menu") != std::string::npos) return;   // the menus' backdrop has no player
    object_ = scene.spawn_model(model, false, animations, kIdle);
    clip_ = kIdle;
    facing_ = player.yaw;
    have_last_ = false;
    if (!active()) return;
    show_cape(scene, false);
    scene.set_object_pose(object_, feet_of(player), facing_);
}

// batman.bdae has two capes: CAPE spread for gliding, CAPE_down hanging on foot.
void Hero::show_cape(Scene& scene, bool spread) {
    scene.set_node_visible(object_, "CAPE", spread);
    scene.set_node_visible(object_, "CAPE_down", !spread);
}

void Hero::play(Scene& scene, const std::string& clip, bool loop) {
    if (clip == clip_) return;
    if (scene.play_clip(object_, clip, loop)) clip_ = clip;
}

bool Hero::act(Scene& scene, const std::string& clip, float yaw) {
    if (!active() || !scene.play_clip(object_, clip, false)) return false;
    clip_ = clip;
    facing_ = yaw;
    acting_ = true;
    return true;
}

void Hero::update(Scene& scene, const Player& player, const PlayerInput& in, float dt) {
    if (!active()) return;
    const glm::vec3 feet = feet_of(player);
    const glm::vec2 moved = have_last_ ? glm::vec2(feet - last_feet_) : glm::vec2(0);
    last_feet_ = feet, have_last_ = true;
    const float speed = dt > 0 ? glm::length(moved) / dt : 0.0f;
    glide_blend_ = std::clamp(glide_blend_ + (player.gliding ? dt : -dt) * camera_ease, 0.0f, 1.0f);

    if (player.grappling) {
        const glm::vec2 to = glm::vec2(player.grapple_target - feet);
        if (glm::length(to) > 0.1f) facing_ = glm::degrees(std::atan2(-to.x, to.y));
        if (!was_grappling_) {
            show_cape(scene, false);
            play(scene, kGrapnelShoot, false);
        } else if (player.grapple_left <= 0 && clip_ == kGrapnelShoot) {
            play(scene, kGrapnelToFly, false);
        } else if (clip_ == kGrapnelToFly && scene.clip_finished(object_)) {
            play(scene, kGrapnelFly, true);
        }
        was_grappling_ = true, was_gliding_ = false, was_on_ground_ = false, air_time_ = 0;
        scene.set_object_pose(object_, feet, facing_);
        return;
    }
    if (was_grappling_) {
        was_grappling_ = false;
        if (player.grapples != grapples_seen_) {   // arrived: climb up onto the edge
            grapples_seen_ = player.grapples;
            play(scene, kGrapnelClimb, false);
            was_on_ground_ = true;
            scene.set_object_pose(object_, feet, facing_);
            return;
        }
    }
    if (player.gliding) {
        if (!was_gliding_) {
            show_cape(scene, true);
            play(scene, speed > 2.0f && was_on_ground_ ? kRunToGlide : kFallToGlide, false);
        } else if (player.plunging) {
            play(scene, kPlunge, true);
        } else if (clip_ == kPlunge || scene.clip_finished(object_)) {
            play(scene, kGlide, true);
        }
        facing_ = player.glide_heading;
        was_gliding_ = true, was_on_ground_ = false, air_time_ = 0;
        scene.set_object_pose(object_, feet, facing_);
        return;
    }
    if (was_gliding_) {   // landed (or hit a wall): fold the cape
        show_cape(scene, false);
        was_gliding_ = false;
        if (player.on_ground) {
            play(scene, in.forward != 0 || in.right != 0 ? kGlideToRun : kGlideLand, false);
            was_on_ground_ = true;
            scene.set_object_pose(object_, feet, facing_);
            return;
        }
    }
    // An action plays to its end where it began.
    if (acting_) {
        if (!player.gliding && !player.grappling && !scene.clip_finished(object_)) {
            scene.set_object_pose(object_, feet, facing_);
            return;
        }
        acting_ = false;
    }
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
        const bool landing = clip_ == kLand || clip_ == kGlideLand || clip_ == kGlideToRun || clip_ == kGrapnelClimb;
        if (!was_on_ground_ && air_time_ > 0.4f) {
            play(scene, kLand, false);
        } else if (!landing || scene.clip_finished(object_) || (speed > 1.0f && clip_ != kGlideToRun)) {
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

float Hero::view_pitch(float pitch) const {
    const float k = glide_blend_ * glide_blend_ * (3 - 2 * glide_blend_);
    return pitch + glide_view_pitch * k;
}

bool Hero::try_grapple(Player& player, const glm::vec3& eye, const glm::vec3& forward) const {
    glm::vec3 point, landing;
    if (player.grappling || !grapple_target(player, eye, forward, point, landing)) return false;
    player.grapple_to(landing);
    return true;
}

bool Hero::grapple_target(const Player& player, const glm::vec3& eye, const glm::vec3& forward, glm::vec3& point,
                          glm::vec3& landing) const {
    if (edges_.empty()) return false;
    const glm::vec3 feet = feet_of(player);
    const glm::vec3 aim = glm::normalize(forward);
    const float min_cos = std::cos(glm::radians(grapple_cone));
    float best = -1;
    for (const GrappleEdge& e : edges_) {
        // The edge point nearest the aim line.
        const glm::vec3 ab = e.b - e.a;
        const float len2 = glm::dot(ab, ab);
        float t = 0.5f;
        if (len2 > 1e-6f) {
            // Closest points of the segment and the ray eye + s * aim.
            const glm::vec3 w = e.a - eye;
            const float b = glm::dot(ab, aim), c = glm::dot(ab, w), d = glm::dot(aim, w);
            const float den = len2 - b * b;
            t = den > 1e-6f ? std::clamp((b * d - c) / den, 0.0f, 1.0f) : 0.5f;
        }
        const glm::vec3 p = e.a + ab * t;
        if (p.z < feet.z + 1.0f || glm::distance(p, feet) > grapple_range) continue;
        const glm::vec3 dir = p - eye;
        const float cosang = glm::dot(glm::normalize(dir), aim);
        if (cosang < min_cos || cosang <= best) continue;
        glm::vec3 inward = -glm::vec3(e.normal.x, e.normal.y, 0);
        if (glm::length(inward) > 1e-3f) inward = glm::normalize(inward);
        best = cosang, point = p, landing = p + inward * grapple_inset;
    }
    return best >= 0;
}

glm::vec3 Hero::camera_position(const Player& player, float yaw, float pitch, const CollisionWorld& world) const {
    const float k = glide_blend_ * glide_blend_ * (3 - 2 * glide_blend_);   // smoothstep
    const float distance = camera_distance + (glide_camera_distance - camera_distance) * k;
    const glm::vec3 pivot = feet_of(player) + glm::vec3(0, 0, pivot_height + (glide_pivot_height - pivot_height) * k);
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    const glm::vec3 forward{-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
    const glm::vec3 right{std::cos(y), std::sin(y), 0};
    const glm::vec3 wanted = pivot - forward * distance + right * camera_side;
    float t = 1;
    glm::vec3 normal;
    if (world.raycast(pivot, wanted, t, nullptr, &normal)) {   // a wall between: come in front of it
        return pivot + (wanted - pivot) * std::max(t - 0.1f / distance, 0.0f) + normal * 0.05f;
    }
    return wanted;
}

}  // namespace tdkr
