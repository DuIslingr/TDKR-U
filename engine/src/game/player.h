// First-person player movement on a level's collision.
#pragma once

#include <glm/glm.hpp>

#include "game/physics.h"

namespace tdkr {

struct PlayerInput {
    float forward = 0, right = 0;   // -1..1
    bool jump = false, run = false;
    bool grapple = false;           // TDKR: fire the grapnel at the aimed edge
};

class Player {
public:
    // Ellipsoid radii (metres): 0.8 m wide, 1.8 m tall. Eye 0.75 m above the centre.
    glm::vec3 radius{0.4f, 0.4f, 0.9f};
    float eye_height = 0.75f;
    float walk_speed = 5.0f, run_speed = 9.0f;
    // CComponentPlayerComponent (gol 0x2a, set by Combat): gravity [13], the fall speed's
    // limit [6], and the jump: an upward push of jump_speed * (time left / jump_time)^2
    // for jump_time seconds ([14], [15]) while gravity builds up (CCharacterController::Move).
    float gravity = 9.8f, jump_speed = 8.0f, jump_time = 0.8f, max_fall_speed = 30.0f;
    float step_snap = 0.3f;         // when grounded, follow the floor down this far per step

    glm::vec3 center{0};
    glm::vec3 push{0};              // an outside pull, m/s (SetAttractionDirection), added to the walk
    float yaw = 0, pitch = 0;       // degrees; yaw 0 looks along +Y, positive turns left
    float vertical_speed = 0;
    float fall_speed = 0;           // what gravity has built up since leaving the ground
    float jump_left = 0;            // seconds of the jump's push left
    uint32_t jumps = 0;             // bumped by each jump off the ground
    bool on_ground = false;
    float landing_speed = 0;   // gravity's speed at the last landing (taken by Combat)
    bool jetpack = false;      // the jetpack power: jumping thrusts upwards, falls are slowed
    bool thrusting = false;    // the jetpack firing (the jump held), as of the last update
    bool rushing = false;      // sprinting (run held while moving), as of the last update
    // TDKR's cape glide (CActorBaseComponent::UpdateNavGlide, CComponentPlayerComponent template
    // fields +0x4c..+0x54): opened by jumping in the air or after falling a while, it carries the
    // player along `glide_heading` (turning towards the view) at glide_speed, sinking at
    // glide_sink. Pushing forward dives, up to dive_speed; with run held it is a plunge. (The
    // game dives past -20 and plunges past -40 degrees of stick pitch; here the stick is WASD.)
    bool can_glide = false;
    bool gliding = false, plunging = false;
    float glide_speed = 17.0f, dive_speed = 25.0f, glide_sink = 2.5f;
    float glide_turn_rate = 90.0f;      // degrees per second the heading turns towards the view
    float glide_after_fall = 0.5f;      // seconds of falling before the cape opens by itself
    float glide_heading = 0;            // degrees, game convention
    // An updraft's lift (the exhaust fans, LevelLogic::updraft), m/s up, set before each update:
    // the glide rises on it, and a jump where it is 2 and up launches up the column
    // (PlayerComponent::GlideJump), rising until the lift no longer beats the sink; a glide
    // through a column rises by its lift meanwhile.
    glm::vec3 lift{0};
    bool fan_rise = false;
    float air_time = 0;                 // seconds since leaving the ground
    bool jump_held = false;             // last update's jump input (to see a fresh press)
    // TDKR's grapnel (CActorBaseComponent::StartGrapplingHook / UpdateNavGrapplingHook): a
    // shot, then a straight flight to the target, which ends standing on it.
    bool grappling = false;
    float grapple_shot = 0.4f;          // seconds the shot takes before the pull
    float grapple_speed = 25.0f;        // metres per second of the pull
    float grapple_left = 0;             // seconds of the shot left
    glm::vec3 grapple_target{0};        // feet position to land on
    uint32_t grapples = 0;              // bumped by each landing (for the body's climb clip)
    void grapple_to(const glm::vec3& feet);
    // Wedged (pushing but hardly moving): move by Irrlicht's one-sided rule for a while.
    float stuck_time = 0, escape_time = 0;

    // Place the player standing on the floor at `feet`.
    void spawn(const glm::vec3& feet, float yaw_deg);
    uint32_t spawns = 0;   // bumped by spawn(): the view should take the player's yaw/pitch
    void update(const CollisionWorld& world, const PlayerInput& in, float dt);

    glm::vec3 eye() const { return center + glm::vec3(0, 0, eye_height); }
    glm::vec3 view_forward() const;
};

}  // namespace tdkr
