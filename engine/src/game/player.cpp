#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace tdkr {

void Player::spawn(const glm::vec3& feet, float yaw_deg) {
    center = feet + glm::vec3(0, 0, radius.z + 0.05f);
    yaw = yaw_deg;
    pitch = 0;
    ++spawns;
    vertical_speed = 0, landing_speed = 0, fall_speed = 0, jump_left = 0;
    on_ground = false;
    stuck_time = escape_time = 0;
    gliding = plunging = fan_rise = false, air_time = 0;
}

glm::vec3 Player::view_forward() const {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return {-std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p)};
}

void Player::grapple_to(const glm::vec3& feet) {
    grappling = true, gliding = plunging = fan_rise = false;
    grapple_left = grapple_shot;
    grapple_target = feet;
    vertical_speed = fall_speed = jump_left = 0;
}

void Player::update(const CollisionWorld& world, const PlayerInput& in, float dt) {
    if (grappling) {
        // The pull flies straight (the hook's line is clear by construction); landing ends it.
        if (grapple_left > 0) {
            grapple_left -= dt;
            return;
        }
        const glm::vec3 goal = grapple_target + glm::vec3(0, 0, radius.z + 0.05f);
        const glm::vec3 to = goal - center;
        const float dist = glm::length(to), step = grapple_speed * dt;
        if (dist <= step) {
            center = goal, grappling = false, on_ground = true, air_time = 0, ++grapples;
            vertical_speed = fall_speed = 0;
        } else {
            center += to / dist * step;
        }
        return;
    }
    // Horizontal intent from the view yaw (pitch doesn't affect walking).
    const float y = glm::radians(yaw);
    const glm::vec3 fwd(-std::sin(y), std::cos(y), 0);
    const glm::vec3 right(std::cos(y), std::sin(y), 0);
    glm::vec3 wish = fwd * in.forward + right * in.right;
    if (glm::length(wish) > 1.0f) wish = glm::normalize(wish);
    const glm::vec3 step = wish * (in.run ? run_speed : walk_speed) * dt + glm::vec3(push.x, push.y, 0) * dt;

    thrusting = jetpack && in.jump;
    rushing = in.run && glm::length(glm::vec2(in.forward, in.right)) > 0.1f;
    const bool jump_pressed = in.jump && !jump_held;
    jump_held = in.jump;
    air_time = on_ground ? 0.0f : air_time + dt;
    if (can_glide && jump_pressed && lift.z >= 2.0f && !fan_rise) {   // up the fan's column
        gliding = fan_rise = true, on_ground = false, glide_heading = yaw, jump_left = 0;
    }
    if (can_glide && !on_ground && !gliding &&
        ((jump_pressed && jump_left <= 0) || (vertical_speed < 0 && air_time > glide_after_fall))) {
        gliding = true;
        // Carry on where the run was going, else where the view looks.
        const glm::vec2 moving = glm::vec2(wish);
        glide_heading = glm::length(moving) > 0.1f ? glm::degrees(std::atan2(-moving.x, moving.y)) : yaw;
    }
    if (gliding && fan_rise) {
        if (lift.z <= glide_sink + 0.5f) {
            fan_rise = false, glide_heading = yaw;   // the top of the lift: off it glides, where the view looks
        } else {
            vertical_speed = lift.z - glide_sink, fall_speed = 0;
            const auto r = world.move(center, radius, glm::vec3(0), glm::vec3(0, 0, vertical_speed * dt));
            if (!r.falling && r.position.z <= center.z + 1e-4f) fan_rise = false;   // a ceiling
            center = r.position, on_ground = false;
            return;
        }
    }
    if (gliding) {
        float d = std::fmod(yaw - glide_heading, 360.0f);
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        glide_heading += std::clamp(d, -glide_turn_rate * dt, glide_turn_rate * dt);
        const float dive = std::clamp(in.forward, 0.0f, 1.0f);
        plunging = in.run && in.forward > 0.5f;
        const float speed = glide_speed + (dive_speed - glide_speed) * dive;
        const float sink = plunging ? dive_speed : glide_sink + (dive_speed * 0.5f - glide_sink) * dive;
        const float h = glm::radians(glide_heading);
        const glm::vec3 along(-std::sin(h), std::cos(h), 0);
        vertical_speed = lift.z - sink, fall_speed = std::max(sink - lift.z, 0.0f), jump_left = 0;
        const auto r = world.move(center, radius, along * (plunging ? speed * 0.4f : speed) * dt, glm::vec3(0, 0, vertical_speed * dt));
        on_ground = !r.falling;
        if (on_ground) landing_speed = sink, vertical_speed = 0, fall_speed = 0;
        if (on_ground || r.hit) gliding = plunging = fan_rise = false;   // landed, or flew into a wall
        center = r.position;
        return;
    }
    if (jetpack) {
        // Thrust while the jump is held (it beats gravity); otherwise a slow descent.
        if (in.jump) vertical_speed = std::min(vertical_speed + (gravity + 14.0f) * dt, 7.0f), on_ground = false;
        vertical_speed = std::max(vertical_speed - gravity * dt, -6.0f);
        fall_speed = std::max(-vertical_speed, 0.0f), jump_left = 0;
    } else {
        if (on_ground && in.jump) jump_left = jump_time, fall_speed = 0, on_ground = false, ++jumps;
        fall_speed = std::min(fall_speed + gravity * dt, max_fall_speed);
        const float push = jump_left > 0 ? jump_speed * (jump_left / jump_time) * (jump_left / jump_time) : 0.0f;
        jump_left = std::max(jump_left - dt, 0.0f);
        vertical_speed = push - fall_speed;
    }

    glm::vec3 vertical(0, 0, vertical_speed * dt);
    const bool rising = vertical_speed > 0;
    if (on_ground && !rising) vertical.z = std::min(vertical.z, -step_snap);   // stay on stairs and slopes

    const auto r = world.move(center, radius, step, vertical, 0.0005f, escape_time > 0);
    // Stuck detection: asked to move, but went less than a fifth of the way.
    const float wanted = glm::length(glm::vec2(step)), moved = glm::length(glm::vec2(r.position - center));
    if (wanted > 1e-4f && moved < wanted * 0.2f) stuck_time += dt;
    else stuck_time = std::max(stuck_time - dt, 0.0f);
    escape_time = std::max(escape_time - dt, 0.0f);
    if (stuck_time > 0.6f) escape_time = 0.5f, stuck_time = 0;
    if (rising) {
        // A rising sweep that hits something is a ceiling, not a floor.
        if (!r.falling) vertical_speed = 0, fall_speed = 0, jump_left = 0;
        on_ground = false;
    } else {
        const bool was_on_ground = on_ground;
        on_ground = !r.falling;
        if (on_ground && !was_on_ground) landing_speed = jetpack ? -vertical_speed : fall_speed;
        if (on_ground) vertical_speed = 0, fall_speed = 0, jump_left = 0;
    }
    center = r.position;
}

}  // namespace tdkr
