#include "game/missiles.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <glm/gtc/matrix_transform.hpp>

#include "audio/audio.h"
#include "game/level_logic.h"
#include "game/physics.h"

namespace tdkr {

namespace {

constexpr int kCompMissile = 0x35C5E0A0, kCompCapsule = 0x0651BDAA;
constexpr float kCloseRange = 5;        // CheckTargetCollide: the slow motion starts here
constexpr float kOnFootRange = 20;      // CanBeHitByBazooka: nearer than this Batman on foot is safe
constexpr float kLockWait = 200;        // ms between the lock's changes
constexpr float kMinAngle = 0.0001f;    // UpdateStateFollow: smaller errors are left alone
constexpr float kScanGap = 0.25f;       // seconds between looks for bazookas (there are few)

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

float wrap(float degrees) { return std::remainder(degrees, 360.0f); }

// Turn towards `want` by at most `step`, never past it (UpdateStateFollow).
float turn(float angle, float want, float step) {
    const float error = wrap(want - angle);
    if (std::abs(error) <= kMinAngle) return angle;
    return std::abs(error) <= step ? want : wrap(angle + std::copysign(step, error));
}

}  // namespace

void Missiles::load(LevelLogic& logic) {
    if (loaded_) return;
    loaded_ = true;
    for (const auto& [type, g] : logic.gol) {
        (void)type;
        if (g.name != "Rocket") continue;
        const Value* v = resolved_template(g, logic.gol, kCompMissile);
        if (!v || v->size() < 17) continue;
        const Value& c = *v;
        life = c[0].as_float(), start_speed = c[1].as_float(), top_speed = c[2].as_float(), acceleration = c[3].as_float();
        yaw_rate = c[4].as_float(), pitch_rate = c[5].as_float(), climb = c[6].as_float();
        cone_yaw = c[7].as_float(), cone_pitch = c[8].as_float();
        blast_radius = c[9].as_float(), blast_inner = c[10].as_float();
        damage_edge = c[11].as_float(), damage_middle = c[12].as_float();
        smoke = c[15].s, blast = c[16].s;
    }
    // CWeapon::GetMaxRange: [5] of the launcher's CComponentWeapon (its +0x18).
    for (const auto& [type, g] : logic.gol) {
        (void)type;
        if (g.name != "RocketLauncher") continue;
        for (const auto& [comp, flags] : g.components) {
            (void)flags;
            if (const Value* v = resolved_template(g, logic.gol, comp); v && v->size() == 20 && (*v)[17].i == 20011) weapon_range = (*v)[5].as_float();
        }
    }
}

void Missiles::reset(LevelLogic& logic) {
    for (Missile& m : missiles_)
        if (m.smoke >= 0) logic.scene.particles().stop(m.smoke);
    missiles_.clear();
    bazookas_.clear();
    if (warning_ && logic.audio) logic.audio->stop("sfx_missile_warning_loop");
    warning_ = false;
}

void Missiles::launch(LevelLogic& logic, int owner, const glm::vec3& at) {
    const LevelLogic::Object* o = logic.object(owner);
    if (!o) return;
    Missile m;
    m.owner = owner;
    m.position = m.last = o->position + glm::vec3(0, 0, 1.5f);   // (the muzzle's dummy is the launcher's, not loaded here)
    const glm::vec3 to = at - m.position;
    m.yaw = glm::degrees(std::atan2(to.y, to.x)) - 90, m.pitch = 90;
    m.speed = start_speed, m.life = life;
    // SetTarget: three quarters of the way up to the target, at least 2 m.
    const float up = to.z;
    m.height = up >= 2 ? up * 0.75f : 2;
    m.smoke = logic.scene.particles().start(smoke, glm::translate(glm::mat4(1), m.position));
    missiles_.push_back(m);
    ++fired_;
    if (logic.trace) logic.log("bazooka " + std::to_string(owner) + " fires a rocket");
}

void Missiles::climb_step(Missile& m, float seconds) {
    m.speed = std::min(m.speed + seconds * acceleration, top_speed);
    const float step = m.speed * seconds;
    m.climbed += step;
    if (m.climbed >= m.height) {
        m.state = Missile::State::Follow, m.climbed = 0, m.speed = start_speed, m.lock = 0, m.lock_wait = 0;
        return;
    }
    m.position.z += step;
}

void Missiles::follow(LevelLogic& logic, Missile& m, const glm::vec3& at, float ms) {
    if (m.lock != 0 && m.lock_wait >= 0) m.lock_wait -= ms;
    const float seconds = ms * 0.001f;
    const glm::vec3 to = at - m.position;
    const float want_yaw = glm::degrees(std::atan2(to.y, to.x)) - 90;
    const float want_pitch = glm::degrees(std::atan2(to.z, glm::length(glm::vec2(to))));
    if (m.lock != 2) {
        const float off_yaw = wrap(want_yaw - m.yaw), off_pitch = wrap(want_pitch - m.pitch);
        const bool in_cone = std::abs(off_yaw) < cone_yaw && std::abs(off_pitch) < cone_pitch;
        if (m.lock == 0 && in_cone) {
            m.lock_wait = kLockWait, m.lock = 1;
            if (logic.audio) {
                logic.audio->play("sfx_missile_locked_on");
                if (!warning_) logic.audio->play("sfx_missile_warning_loop");
            }
            warning_ = true;
        }
        if (m.lock_wait < 0 && !in_cone) {
            if (m.lock == 1) {
                m.lock_wait = kLockWait, m.lock = 0;
                if (logic.audio) logic.audio->stop("sfx_missile_warning_loop");
                warning_ = false;
            } else if (m.lock == 0) {
                m.lock = 2;
                if (logic.audio) logic.audio->stop("sfx_missile_warning_loop");
                warning_ = false;
            }
        }
        if (m.lock != 2) {
            m.yaw = turn(m.yaw, want_yaw, seconds * yaw_rate);
            m.pitch = turn(m.pitch, want_pitch, seconds * pitch_rate);
        }
    }
    m.speed = std::min(m.speed + seconds * acceleration, top_speed);
    const float yaw = glm::radians(m.yaw), pitch = glm::radians(m.pitch);
    m.position += glm::vec3(-std::cos(pitch) * std::sin(yaw), std::cos(pitch) * std::cos(yaw), std::sin(pitch)) * m.speed * seconds;
}

void Missiles::destroy(LevelLogic& logic, size_t i, const glm::vec3& at, bool burn, const std::function<void(float, const glm::vec3&)>& hurt) {
    const Missile m = missiles_[i];
    if (m.smoke >= 0) logic.scene.particles().stop(m.smoke);
    logic.scene.particles().start(blast, glm::translate(glm::mat4(1), m.position));
    if (m.close) logic.reset_slow_motion(0);
    missiles_.erase(missiles_.begin() + i);
    if (warning_ && std::none_of(missiles_.begin(), missiles_.end(), [](const Missile& o) { return o.lock == 1; })) {
        if (logic.audio) logic.audio->stop("sfx_missile_warning_loop");
        warning_ = false;
    }
    if (burn) return;
    ++exploded_;
    // CAIController::SplashDamage: the edge's damage at the radius, the middle's inside the inner.
    const float distance = glm::distance(at, m.position);
    if (distance >= blast_radius) return;
    const float share = std::clamp((blast_radius - distance) / std::max(blast_radius - blast_inner, 1e-3f), 0.0f, 1.0f);
    hurt(damage_edge + share * (damage_middle - damage_edge), m.position);
}

void Missiles::update_bazookas(LevelLogic& logic, const CollisionWorld& world, const glm::vec3& at, bool flying, float dt) {
    scan_ -= dt;
    if (scan_ <= 0) {
        scan_ = kScanGap;
        for (const auto& [id, o] : logic.objects()) {
            if (o.dead || !o.enabled || bazookas_.count(id) || logic.snipers.count(id)) continue;
            const auto g = logic.gol.find(logic.object_type(o));
            if (g == logic.gol.end() || g->second.name != "Bazooka_Enemy") continue;
            Bazooka b;
            b.range = weapon_range;
            bazookas_[id] = b;
            if (logic.trace) logic.log("bazooka " + std::to_string(id) + " stands by, sees " + std::to_string(int(b.range)) + " m");
        }
    }
    for (auto it = bazookas_.begin(); it != bazookas_.end();) {
        LevelLogic::Object* o = logic.object(it->first);
        if (!o || o->dead) {
            it = bazookas_.erase(it);
            continue;
        }
        Bazooka& b = it->second;
        ++it;
        if (!o->enabled || logic.attack_targets.count(o->id)) continue;
        const glm::vec3 eye = o->position + glm::vec3(0, 0, 1.5f);
        const float distance = glm::distance(eye, at);
        float blocked = 1;
        // CNpcAIComponent::Attack_TestRanged: in range, a clear line, and CanBeHitByBazooka.
        const bool can_fire = distance <= b.range && (flying || distance > kOnFootRange) && !world.raycast(eye, at, blocked);
        b.timer -= dt;
        const glm::vec3 to = at - eye;
        o->yaw = glm::degrees(std::atan2(to.y, to.x)) - 90;   // (CGameObject::LookAt)
        switch (b.phase) {
            case Bazooka::Phase::Idle:
                if (can_fire) {
                    // Attack_Aim: up to the shoulder, then Wait(4000 + Random(2000)).
                    b.phase = Bazooka::Phase::Aim;
                    if (logic.trace) logic.log("bazooka " + std::to_string(o->id) + " aims");
                    b.timer = (4000.0f + float(rng()() % 2000)) * 0.001f;
                    logic.actor_state(*o, "Enemy_Idle_Aim_Bazooka", true);
                }
                break;
            case Bazooka::Phase::Aim:
                if (!can_fire) {
                    b.phase = Bazooka::Phase::Idle;
                    logic.actor_state(*o, "Enemy_Bazooka_Idle", true);
                } else if (b.timer <= 0 && std::none_of(missiles_.begin(), missiles_.end(), [&](const Missile& m) { return m.owner == o->id; })) {
                    // Attack_Ranged; CanFireBullet: one of its own in the air at a time. (GameEvent 17, frame 0.)
                    b.phase = Bazooka::Phase::Shoot, b.timer = 1;
                    logic.actor_state(*o, "Enemy_Shoot_Bazooka", true);
                    launch(logic, o->id, at);
                }
                break;
            case Bazooka::Phase::Shoot:
                if (b.timer <= 0) b.phase = Bazooka::Phase::Idle;
                break;
        }
    }
}

void Missiles::update(LevelLogic& logic, const CollisionWorld& world, const glm::vec3& at, bool flying, float dt,
                      const std::function<void(float, const glm::vec3&)>& hurt) {
    load(logic);
    if (dt <= 0) return;
    update_bazookas(logic, world, at, flying, dt);
    const float ms = dt * 1000;
    // The target's upright capsule (0x651BDAA: radius, height), its middle near enough here.
    float radius = 0.6f;
    if (const LevelLogic::Object* bat = flying ? logic.object(logic.riding) : nullptr)
        if (const Value* c = logic.object_component(*bat, kCompCapsule); c && c->size() >= 2) radius = (*c)[0].as_float() + (*c)[1].as_float() * 0.5f;
    for (size_t i = 0; i < missiles_.size();) {
        Missile& m = missiles_[i];
        m.life -= ms;
        m.last = m.position;
        if (m.life < 0) {
            destroy(logic, i, at, false, hurt);
            continue;
        }
        if (m.state == Missile::State::Climb) climb_step(m, ms * 0.001f);
        else follow(logic, m, at, ms);
        logic.scene.particles().move(m.smoke, glm::translate(glm::mat4(1), m.position));
        // CheckTargetCollide: the step against the target; CheckGeometryCollide: against the scene.
        const glm::vec3 step = m.position - m.last;
        const float along = std::clamp(glm::dot(at - m.last, step) / std::max(glm::dot(step, step), 1e-6f), 0.0f, 1.0f);
        float t = 1;
        if (glm::distance(m.last + step * along, at) <= radius || world.raycast(m.last, m.position, t)) {
            destroy(logic, i, at, false, hurt);
            continue;
        }
        m.range = glm::distance(m.position, at);
        if (!m.close && m.range < kCloseRange) m.close = true, logic.slow_motion(0.1f, 0, 5000, 0);
        ++i;
    }
}

bool Missiles::can_counter() const {
    const float near = nearest();
    return near >= 0 && near < flare_range;
}

bool Missiles::counter(LevelLogic& logic) {
    if (!can_counter()) return false;
    size_t best = 0;
    for (size_t i = 0; i < missiles_.size(); ++i)
        if (missiles_[i].range >= 0 && (missiles_[best].range < 0 || missiles_[i].range < missiles_[best].range)) best = i;
    destroy(logic, best, glm::vec3(0), true, [](float, const glm::vec3&) {});
    ++countered_;
    if (logic.trace) logic.log("flares: a rocket is destroyed");
    return true;
}

int Missiles::inbound() const {
    return int(std::count_if(missiles_.begin(), missiles_.end(), [](const Missile& m) { return m.range >= 0; }));
}

float Missiles::nearest() const {
    float best = -1;
    for (const Missile& m : missiles_)
        if (m.range >= 0 && (best < 0 || m.range < best)) best = m.range;
    return best;
}

glm::vec3 Missiles::nearest_position() const {
    glm::vec3 at(0);
    float best = -1;
    for (const Missile& m : missiles_)
        if (m.range >= 0 && (best < 0 || m.range < best)) best = m.range, at = m.position;
    return at;
}

int Missiles::nearest_lock() const {
    int lock = -1;
    float best = -1;
    for (const Missile& m : missiles_)
        if (m.range >= 0 && (best < 0 || m.range < best)) best = m.range, lock = m.lock;
    return lock;
}

bool Missiles::locked() const {
    return std::any_of(missiles_.begin(), missiles_.end(), [](const Missile& m) { return m.lock == 1; });
}

}  // namespace tdkr
