#include "game/brawl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "game/hero.h"
#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kSpecial = 0x800, kAttack = 0x4, kHurt = 0x80;   // STATE_TYPE_SPECIAL, _ATTACK, _HURT
constexpr float kStrikeDamage = 25, kCounterDamage = 40, kEnemyDamage = 8;
constexpr float kCloseIn = 1.3f;        // metres an enemy keeps from Batman to attack
constexpr float kEnemySpeed = 3.5f;     // m/s closing in
constexpr float kWindup = 0.7f;         // seconds an attack can be countered before it lands

// Batman's light strikes, in combo order; his counter and the enemy's hurt for each attack.
const char* const kStrikes[] = {"Batman_Attack_LS1_Punch1_LeftHand_High", "Batman_Attack_LS1_Punch2_RightHand_High",
                                "Batman_Attack_LS2_Kick_RightFoot_High", "Batman_Attack_LS1_Elbow_High",
                                "Batman_Attack_LS3_Punch1_LeftHand_High", "Batman_Attack_LS2_Knee_High"};
const char* const kStrikeHurts[] = {"Enemy_Hurt_01_Jab", "Enemy_Hurt_02_Cross", "Enemy_Hurt_03_Hook"};
struct EnemyAttack {
    const char* attack;
    const char* counter;   // Batman's
    const char* countered; // the enemy's
    const char* batman_hurt;
};
const EnemyAttack kAttacks[] = {
    {"Enemy_Attack_01_Jab", "Combat_Attack_06_JabCounter", "Enemy_Hurt_06_JabCounter", "Batman_Hurt_01_Jab"},
    {"Enemy_Attack_02_Cross", "Combat_Attack_07_CrossCounter", "Enemy_Hurt_07_CrossCounter", "Batman_Hurt_02_Cross"},
    {"Enemy_Attack_03_Kick", "Combat_Attack_08_KickCounter", "Enemy_Hurt_08_KickCounter", "Batman_Hurt_03_Kick"},
};
const EnemyAttack& attack_of(const std::string& name) {
    for (const EnemyAttack& a : kAttacks)
        if (name == a.attack) return a;
    return kAttacks[0];
}

float angle_to(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = to - from;
    return glm::degrees(std::atan2(-d.x, d.y));
}

}  // namespace

void Brawl::reset() {
    fighters_.clear();
    snipers_.clear();
    action_left_ = 0, strike_target_ = -1, strike_impact_ = -1, takedown_ = false, combo_ = 0;
    batman_health = 100;
    logic_.player_action.clear(), logic_.player_action_mask = 0;
}

Brawl::Fighter& Brawl::fighter(int id) { return fighters_[id]; }

glm::vec3 Brawl::feet() const { return player_.center - glm::vec3(0, 0, player_.radius.z); }
float Brawl::yaw_to(const glm::vec3& at) const { return angle_to(feet(), at); }

// Actors that fight: the enemy types (they have the melee hurt states) and anyone told to.
bool Brawl::enemy(int id) const {
    const LevelLogic::Object* o = logic_.object(id);
    if (!o || o->dead || !o->enabled || !o->visible || o->index == std::numeric_limits<size_t>::max()) return false;
    if (logic_.attack_targets.count(id)) return true;
    ActorState st;
    return find_actor_state(logic_.gol, logic_.object_type(*o), "Enemy_Hurt_01_Jab", st);
}

int Brawl::target(float reach, float ahead) const {
    const glm::vec3 f = feet();
    const float yaw = glm::radians(hero_.facing());
    const glm::vec2 forward(-std::sin(yaw), std::cos(yaw));
    int best = -1;
    float best_score = 1e30f;
    for (const auto& [id, o] : logic_.objects_) {
        if (!enemy(id) || std::abs(o.position.z - f.z) > 1.5f) continue;
        const glm::vec2 d = glm::vec2(o.position - f);
        const float dist = glm::length(d);
        if (dist > reach) continue;
        const float facing = dist > 1e-3f ? glm::dot(d / dist, forward) : 1.0f;
        if (dist > ahead && facing < 0.3f) continue;   // far ones only ahead
        const float score = dist * (1.5f - facing);
        if (score < best_score) best_score = score, best = id;
    }
    return best;
}

bool Brawl::actor_state(int id, const std::string& state, bool loop) {
    LevelLogic::Object* o = logic_.object(id);
    (void)loop;
    return o && logic_.actor_state(*o, state, true);
}

// Batman plays one of his states: its clip, his facing, and the state the scripts see.
void Brawl::act(const std::string& state, int mask, float yaw, float seconds) {
    int type = -1;
    for (const auto& [id, g] : logic_.gol)
        if (g.name == "Batman") type = id;
    ActorState st;
    if (find_actor_state(logic_.gol, type, state, st)) hero_.act(scene_, st.clips[0], yaw);
    logic_.player_action = state, logic_.player_action_mask = st.mask ? st.mask : mask;
    action_left_ = seconds;
    if (hero_.active()) {
        Scene::ClipState cs;
        if (scene_.clip_state(hero_.object(), cs) && cs.name == (st.clips.empty() ? "" : st.clips[0]) && cs.length > 0)
            action_left_ = std::max(seconds, cs.length / 1000.0f * 0.85f);
    }
}

void Brawl::hit(int id, float damage, const std::string& hurt) {
    LevelLogic::Object* o = logic_.object(id);
    if (!o || o->dead) return;
    o->health -= damage;
    Fighter& f = fighter(id);
    if (o->health <= 0) {
        o->health = 0;
        o->dead = true;
        f.phase = Fighter::Phase::Down;
        logic_.attack_targets.erase(id);
        if (!actor_state(id, "Enemy_Cross_Death")) actor_state(id, "Combat_death");
        std::erase_if(logic_.objective_markers, [id](const LevelLogic::ObjectiveMarker& m) { return m.id == id; });
        logic_.runtime_.send_event("Dead", id);
        if (logic_.trace) logic_.log("brawl: " + std::to_string(id) + " down");
        return;
    }
    actor_state(id, hurt);
    f.phase = Fighter::Phase::Hurt;
    f.timer = 0.9f;
    f.cooldown = std::max(f.cooldown, 1.2f);
    // Hit, an onlooker joins the fight.
    if (!logic_.attack_targets.count(id)) logic_.attack_targets[id] = 0;
}

void Brawl::hurt_batman(float damage, const std::string& state) {
    batman_health = std::max(batman_health - damage, 0.0f);
    if (player_.on_ground && !player_.gliding && !player_.grappling) act(state, kHurt, hero_.facing(), 0.5f);
    if (batman_health <= 0) {
        if (logic_.trace) logic_.log("brawl: Batman is down");
        logic_.respawn_player();
        batman_health = 100;
    }
}

bool Brawl::in_sight(const glm::vec3& from, const glm::vec3& to) const {
    float t = 1;
    return !scene_.collision().raycast(from, to, t);
}

// CSniperComponent, simplified: sweep, lock on, aim, shoot, reload.
void Brawl::update_snipers(float dt) {
    constexpr float kRange = 70, kCone = 7, kSweepWidth = 35, kSweepSpeed = 18, kAimTime = 1.6f, kReload = 2.2f;
    constexpr float kTurn = 50;   // degrees per second while aiming (Attack_Update)
    const glm::vec3 chest = feet() + glm::vec3(0, 0, 1.2f);
    for (auto it = logic_.snipers.begin(); it != logic_.snipers.end(); ++it) {
        LevelLogic::Object* o = logic_.object(it->first);
        if (!o || o->dead || !o->enabled || logic_.attack_targets.count(o->id)) continue;
        const LevelLogic::Object* wp = logic_.object(it->second);
        const glm::vec3 watched = wp ? wp->position : feet();   // (0: Batman himself)
        Sniper& s = snipers_[o->id];
        if (s.phase == Sniper::Phase::Sweep && s.timer == 0 && s.lost == 0 && o->state != "Sniper_Aim_Sweep") {
            s.base = angle_to(o->position, watched);
            actor_state(o->id, "Sniper_Aim_Sweep");
        }
        const glm::vec3 eye = o->position + glm::vec3(0, 0, 1.6f);
        const float to_batman = angle_to(o->position, chest);
        const float dist = glm::distance(eye, chest);
        auto off = [](float a, float b) { return std::remainder(a - b, 360.0f); };
        const bool seen = dist < kRange && !logic_.player_hidden && in_sight(eye, chest);
        switch (s.phase) {
            case Sniper::Phase::Sweep:
                s.base = angle_to(o->position, watched);   // (the scripts may change what it watches)
                s.sweep += s.dir * kSweepSpeed * dt;
                if (std::abs(s.sweep) > kSweepWidth) s.dir = -s.dir, s.sweep = std::clamp(s.sweep, -kSweepWidth, kSweepWidth);
                o->yaw = s.base + s.sweep;
                s.timer += dt;   // (marks it started)
                if (seen && std::abs(off(to_batman, o->yaw)) < kCone) {
                    s.phase = Sniper::Phase::Aim, s.timer = kAimTime, s.lost = 0;
                    actor_state(o->id, "Sniper_Aim_Idle");
                    if (logic_.trace) logic_.log("sniper " + std::to_string(o->id) + " has Batman in its sights");
                }
                break;
            case Sniper::Phase::Aim: {
                const float d = off(to_batman, o->yaw);
                o->yaw += std::clamp(d, -kTurn * dt, kTurn * dt);
                if (!seen) {
                    s.lost += dt;
                    if (s.lost > 3) s.phase = Sniper::Phase::Sweep, s.sweep = off(o->yaw, s.base), actor_state(o->id, "Sniper_Aim_Sweep");
                    break;
                }
                s.lost = 0;
                s.timer -= dt;
                if (s.timer <= 0 && std::abs(d) < kCone) {   // the shot
                    actor_state(o->id, "Sniper_Shoot");
                    hurt_batman(25, "Batman_Hurt_02_Cross");
                    if (logic_.trace) logic_.log("sniper " + std::to_string(o->id) + " shoots Batman");
                    s.phase = Sniper::Phase::Reload, s.timer = kReload;
                }
                break;
            }
            case Sniper::Phase::Reload:
                s.timer -= dt;
                if (s.timer < kReload - 0.6f && o->state == "Sniper_Shoot") actor_state(o->id, "Sniper_Reload");
                if (s.timer <= 0) s.phase = Sniper::Phase::Aim, s.timer = kAimTime, actor_state(o->id, "Sniper_Aim_Idle");
                break;
        }
        if (o->index != std::numeric_limits<size_t>::max()) scene_.set_object_pose(o->index, o->position, o->yaw);
    }
}

int Brawl::pull_target(const glm::vec3& eye, const glm::vec3& forward) const {
    if (!player_.on_ground || busy() || pull_.id >= 0) return -1;
    if (logic_.grapple_lock >= 0 && enemy(logic_.grapple_lock)) return logic_.grapple_lock;
    int best = -1;
    float best_dot = std::cos(glm::radians(6.0f));
    for (const auto& [id, o] : logic_.objects_) {
        if (!o.targetable || !enemy(id)) continue;
        const glm::vec3 at = o.position + glm::vec3(0, 0, 1.2f);
        const glm::vec3 d = at - eye;
        const float dist = glm::length(d);
        if (dist < 4 || dist > 40) continue;
        const float dot = glm::dot(d / dist, forward);
        if (dot > best_dot && in_sight(eye, at)) best_dot = dot, best = id;
    }
    return best;
}

bool Brawl::pull(int id) {
    LevelLogic::Object* o = logic_.object(id);
    if (!o || o->dead || pull_.id >= 0) return false;
    const glm::vec3 f = feet();
    glm::vec2 d(o->position - f);
    d = glm::length(d) > 1e-3f ? glm::normalize(d) : glm::vec2(0, 1);
    pull_ = {id, o->position, f + glm::vec3(d * 1.6f, 0.0f), 0, std::clamp(glm::distance(o->position, f) / 18.0f, 0.5f, 1.6f)};
    act("Grapnel_Enemy_Shoot", 0x400000, yaw_to(o->position), 0.6f);
    actor_state(id, "Enemy_Grapnel_Loopable_Pull_Start");
    logic_.snipers.erase(id);
    logic_.attack_targets.erase(id);
    fighter(id).phase = Fighter::Phase::Down;
    if (logic_.grapple_lock == id) logic_.grapple_lock = -1;
    if (logic_.trace) logic_.log("brawl: grapnel pulls " + std::to_string(id));
    return true;
}

bool Brawl::can_take_down() const {
    if (leap_target() >= 0) return true;
    if (!logic_.controls.silent) return false;
    const int t = target(2.2f, 2.2f);
    return t >= 0 && !logic_.attack_targets.count(t);
}

int Brawl::leap_target() const {
    if (!player_.on_ground || busy()) return -1;
    const glm::vec3 f = feet();
    const float yaw = glm::radians(player_.yaw);   // where the view looks (the scripts turn it there)
    const glm::vec2 forward(-std::sin(yaw), std::cos(yaw));
    int best = -1;
    float best_d = 1e30f;
    for (const auto& [id, o] : logic_.objects_) {
        const float drop = f.z - o.position.z;
        if (drop < 3.0f || drop > 15.0f || !o.targetable || !enemy(id) || logic_.attack_targets.count(id)) continue;
        const glm::vec2 d = glm::vec2(o.position - f);
        const float dist = glm::length(d);
        if (dist > 15.0f || (dist > 1e-3f && glm::dot(d / dist, forward) < -0.2f)) continue;
        if (dist < best_d) best_d = dist, best = id;
    }
    return best;
}

void Brawl::strike() {
    if (!player_.on_ground || player_.gliding || player_.grappling) return;
    if (busy() && action_left_ > 0.15f) return;   // the next strike chains once this one has landed
    // A leap from above onto an enemy below (one close at hand comes first).
    if (const int t = leap_target(); t >= 0 && !(logic_.controls.silent && target(2.2f, 2.2f) >= 0)) {
        LevelLogic::Object& o = *logic_.object(t);
        leap_from_ = feet();
        const glm::vec3 d = o.position - leap_from_;
        const glm::vec2 back = glm::length(glm::vec2(d)) > 1e-3f ? glm::normalize(glm::vec2(d)) : glm::vec2(0, 1);
        leap_to_ = o.position - glm::vec3(back * 0.6f, 0);
        leap_len_ = std::clamp(glm::length(d) / 9.0f, 0.6f, 1.4f), leap_t_ = 0, leap_victim_ = t;
        act("Gargoyle_Jump_Fall_On_Enemy_Start", kSpecial, yaw_to(o.position), leap_len_ + 1.2f);
        logic_.attack_targets.erase(t);
        fighter(t).phase = Fighter::Phase::Down;
        if (logic_.trace) logic_.log("brawl: leap takedown on " + std::to_string(t));
        return;
    }
    // A silent takedown on an enemy that is not fighting.
    if (can_take_down()) {
        const int t = target(2.2f, 2.2f);
        LevelLogic::Object& o = *logic_.object(t);
        // Batman behind the victim, both facing its way (a paired animation).
        const float yaw = glm::radians(o.yaw);
        const glm::vec3 behind = o.position - glm::vec3(-std::sin(yaw), std::cos(yaw), 0) * 0.7f;
        player_.center = glm::vec3(behind.x, behind.y, player_.center.z);
        act("Silent_Takedown_Choke_Attacker", kSpecial, o.yaw, 2.5f);
        actor_state(t, "Silent_Takedown_Choke_Victim");
        logic_.attack_targets.erase(t);
        fighter(t).phase = Fighter::Phase::Down;
        strike_target_ = t, strike_impact_ = action_left_ * 0.8f, takedown_ = true;
        if (logic_.trace) logic_.log("brawl: silent takedown on " + std::to_string(t));
        return;
    }
    if (!logic_.controls.combat) return;
    logic_.player_did.attack = true;   // (a takedown is not an attack: HasPlayerAttacked)
    const int t = target(5.0f, 2.0f);
    const std::string& strike = kStrikes[size_t(combo_++) % std::size(kStrikes)];
    if (t < 0) {   // a swing at the air
        act(strike, kAttack, hero_.facing(), 0.4f);
        return;
    }
    LevelLogic::Object& o = *logic_.object(t);
    // Close in: Batman ends up an arm's length from the enemy.
    const glm::vec3 to = o.position - feet();
    const float dist = glm::length(glm::vec2(to));
    if (dist > 1.1f) {
        const glm::vec2 step = glm::vec2(to) / dist * (dist - 1.1f);
        player_.center += glm::vec3(step, 0);
    }
    act(strike, kAttack, yaw_to(o.position), 0.45f);
    strike_target_ = t, strike_impact_ = 0.2f, takedown_ = false;
    // The enemy turns to take it; a wind-up is broken off.
    Fighter& f = fighter(t);
    if (f.phase == Fighter::Phase::Windup) f.phase = Fighter::Phase::Ready, f.cooldown = 1.0f;
}

bool Brawl::counter_open() const {
    for (const auto& [id, f] : fighters_)
        if (f.phase == Fighter::Phase::Windup && f.timer > 0.05f) {
            const LevelLogic::Object* o = logic_.object(id);
            if (o && !o->dead && glm::distance(glm::vec2(o->position), glm::vec2(feet())) < 3.0f) return true;
        }
    return false;
}

void Brawl::counter() {
    if (!logic_.controls.counter || !player_.on_ground) return;
    // The attacker whose wind-up is furthest along.
    int best = -1;
    float least = 1e30f;
    for (auto& [id, f] : fighters_) {
        const LevelLogic::Object* o = logic_.object(id);
        if (f.phase != Fighter::Phase::Windup || !o || o->dead) continue;
        if (glm::distance(glm::vec2(o->position), glm::vec2(feet())) > 3.0f) continue;
        if (f.timer < least) least = f.timer, best = id;
    }
    if (best < 0) return;
    logic_.player_did.counter = true;
    Fighter& f = fighter(best);
    const EnemyAttack& a = attack_of(f.attack);
    LevelLogic::Object& o = *logic_.object(best);
    act(a.counter, kAttack, yaw_to(o.position), 0.6f);
    f.phase = Fighter::Phase::Ready, f.cooldown = 2.0f;
    hit(best, kCounterDamage, a.countered);
    if (logic_.trace) logic_.log("brawl: countered " + std::to_string(best));
}

void Brawl::update(float dt) {
    logic_.counter_open = counter_open();
    // The leap: an arc down onto the victim, then the landing that takes it down.
    if (leap_victim_ >= 0) {
        leap_t_ += dt;
        const float k = std::min(leap_t_ / leap_len_, 1.0f);
        glm::vec3 p = glm::mix(leap_from_, leap_to_, k);
        p.z += std::sin(k * 3.14159f) * 1.5f;
        player_.center = p + glm::vec3(0, 0, player_.radius.z);
        player_.vertical_speed = 0, player_.fall_speed = 0;
        if (k >= 1) {
            const int t = leap_victim_;
            leap_victim_ = -1;
            act("Gargoyle_Jump_Land_On_Enemy", kSpecial, hero_.facing(), 1.0f);
            actor_state(t, "Silent_Takedown_From_Gargoyle");
            strike_target_ = t, strike_impact_ = 0.3f, takedown_ = true;
        }
    }
    if (action_left_ > 0) {
        action_left_ -= dt;
        if (action_left_ <= 0) logic_.player_action.clear(), logic_.player_action_mask = 0;
    }
    if (strike_impact_ >= 0) {
        strike_impact_ -= dt;
        if (strike_impact_ < 0 && strike_target_ >= 0) {
            if (takedown_) {
                if (LevelLogic::Object* o = logic_.object(strike_target_)) {
                    o->dead = true, o->health = 0;
                    std::erase_if(logic_.objective_markers, [&](const LevelLogic::ObjectiveMarker& m) { return m.id == o->id; });
                    logic_.runtime_.send_event("Dead", o->id);
                }
            } else {
                hit(strike_target_, kStrikeDamage, kStrikeHurts[size_t(combo_) % std::size(kStrikeHurts)]);
            }
            strike_target_ = -1;
        }
    }
    const glm::vec3 f = feet();
    for (const auto& [id, who] : logic_.attack_targets) {
        LevelLogic::Object* o = logic_.object(id);
        if (!o || o->dead || who != 0 || !o->enabled) continue;
        Fighter& fi = fighter(id);
        fi.timer -= dt, fi.cooldown -= dt;
        const glm::vec3 to = f - o->position;
        const float dist = glm::length(glm::vec2(to));
        if (fi.phase == Fighter::Phase::Hurt) {
            if (fi.timer <= 0) fi.phase = Fighter::Phase::Ready, actor_state(id, "Enemy_Combat_Idle");
            continue;
        }
        if (fi.phase == Fighter::Phase::Windup) {
            if (fi.timer <= 0) {   // it lands
                const EnemyAttack& a = attack_of(fi.attack);
                if (dist < 2.2f && !(busy() && logic_.player_action_mask & kSpecial)) {
                    hurt_batman(kEnemyDamage, a.batman_hurt);
                    if (logic_.trace) logic_.log("brawl: Batman hit by " + std::to_string(id));
                }
                fi.phase = Fighter::Phase::Ready;
                fi.cooldown = 2.0f + float(id % 3) * 0.7f;
            }
            continue;
        }
        // Face Batman; close in to striking distance, then take turns attacking.
        if (dist > 1e-3f) o->yaw = glm::degrees(std::atan2(-to.x, to.y));
        if (dist > kCloseIn + 0.3f) {
            const float step = std::min(kEnemySpeed * dt, dist - kCloseIn);
            o->position += glm::vec3(glm::vec2(to) / dist * step, 0);
            if (fi.phase != Fighter::Phase::Close) actor_state(id, "Run");
            fi.phase = Fighter::Phase::Close;
        } else {
            if (fi.phase != Fighter::Phase::Ready) actor_state(id, "Enemy_Combat_Idle"), fi.phase = Fighter::Phase::Ready;
            const bool other_attacking = std::any_of(fighters_.begin(), fighters_.end(), [&](const auto& kv) {
                return kv.first != id && kv.second.phase == Fighter::Phase::Windup;
            });
            if (fi.cooldown <= 0 && !other_attacking && !busy()) {
                const EnemyAttack& a = kAttacks[size_t(id) % std::size(kAttacks)];
                fi.attack = a.attack;
                actor_state(id, a.attack);
                fi.phase = Fighter::Phase::Windup;
                fi.timer = kWindup;
            }
        }
        // Fighters keep apart (they spread around Batman instead of standing in one another).
        for (const auto& [other, w] : logic_.attack_targets) {
            const LevelLogic::Object* q = other != id ? logic_.object(other) : nullptr;
            if (!q || q->dead) continue;
            glm::vec2 away(o->position - q->position);
            const float d = glm::length(away);
            if (d < 1.3f) {
                away = d > 1e-3f ? away / d : glm::vec2(std::cos(float(id)), std::sin(float(id)));
                o->position += glm::vec3(away * std::min((1.3f - d) * 0.5f, 2.0f * dt + 0.01f), 0);
            }
        }
        if (o->index != std::numeric_limits<size_t>::max()) scene_.set_object_pose(o->index, o->position, o->yaw);
    }
    update_snipers(dt);
    // A pulled enemy flies to Batman's feet, lies there a moment, then gets up to fight.
    if (pull_.id >= 0) {
        LevelLogic::Object* o = logic_.object(pull_.id);
        if (!o || o->dead) {
            pull_.id = -1;
        } else {
            const float before = pull_.t;
            pull_.t += dt;
            const float k = std::min(pull_.t / pull_.length, 1.0f);
            if (before < 0.15f && pull_.t >= 0.15f) actor_state(o->id, "Enemy_Grapnel_Loopable_Pull_Fly");
            o->position = glm::mix(pull_.from, pull_.to, k) + glm::vec3(0, 0, std::sin(k * 3.14159f) * 1.0f);
            if (k >= 1 && before < pull_.length) actor_state(o->id, "Enemy_Grapnel_Loopable_Pull_Land");
            if (pull_.t > pull_.length + 0.5f && o->state == "Enemy_Grapnel_Loopable_Pull_Land")
                actor_state(o->id, "Enemy_Grapnel_Pulled_Down_Floor_Idle");
            if (pull_.t > pull_.length + 2.5f) {   // up, and at Batman
                actor_state(o->id, "Enemy_Grapnel_Pulled_Down_Floor_Get_Up");
                Fighter& fi = fighter(o->id);
                fi.phase = Fighter::Phase::Hurt, fi.timer = 1.2f, fi.cooldown = 2.0f;
                logic_.attack_targets[o->id] = 0;
                pull_.id = -1;
            }
            if (o->index != std::numeric_limits<size_t>::max()) scene_.set_object_pose(o->index, o->position, o->yaw);
        }
    }
    // Headless tests fight by themselves.
    if (auto_fight) {
        if (counter_open()) counter();
        auto_timer_ -= dt;
        if (auto_timer_ <= 0 && !busy() && (target(5.0f, 2.0f) >= 0 || can_take_down())) strike(), auto_timer_ = 0.5f;
    }
}

}  // namespace tdkr
