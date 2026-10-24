// TDKR's hand-to-hand fighting, simplified from CMeleeComponent / CNpcAIComponent: what the
// tutorial and the street fights need, played with the actors' own state clips.
//
// Batman's strike goes for the nearest living enemy ahead (within 5 m, or 2 m anywhere): he
// closes in, plays one of his light strikes (Batman_Attack_LS*), and the enemy takes the hit
// (an Enemy_Hurt_* state, or Enemy_Cross_Death when its health runs out). An enemy that is not
// fighting and close enough is taken down silently instead (Silent_Takedown_Choke_Attacker /
// _Victim; Batman is in a STATE_TYPE_SPECIAL state meanwhile), if the scripts allow it
// (SetSilentKillControlEnable); one below (3 to 15 m down, ahead) is leapt on from above
// (Gargoyle_Jump_Fall_On_Enemy, Gargoyle_Jump_Land_On_Enemy / Silent_Takedown_From_Gargoyle).
//
// Snipers (NPCSetSniper) sweep their aim around the point they watch (Sniper_Aim_Sweep); Batman
// within a few degrees of the aim, in the open and in range, is locked on (Sniper_Aim_Idle), and
// after a moment shot (Sniper_Shoot, then Sniper_Reload). Out of sight a while, it sweeps again.
// (A simplification of CSniperComponent's sweep / aim / attack states.)
//
// The grapnel at an enemy (aimed at, or locked by GrapplingLockTarget) pulls it to Batman
// (Grapnel_Enemy_Shoot; Enemy_Grapnel_Loopable_Pull_Start / _Fly / _Land): it lands at his
// feet, stays down a moment (Enemy_Grapnel_Pulled_Down_Floor_Idle), gets up and fights.
//
// Batman's health runs out: back to the checkpoint (MissionFailedAutoReload's way).
//
// Enemies told to fight (NPCAttackTarget, NPCSetStance "Aggressive") close in on Batman and
// attack in turns (Enemy_Attack_*). An attack winds up for a moment: then the counter is open
// (IsCounterAttackControlEnabled) and Batman's counter (Combat_Attack_*Counter against
// Enemy_Hurt_*Counter) beats it; otherwise it lands (Batman_Hurt_*).
#pragma once

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

namespace tdkr {

class Hero;
class LevelLogic;
class Player;
class Scene;

class Brawl {
public:
    Brawl(LevelLogic& logic, Scene& scene, Player& player, Hero& hero) : logic_(logic), scene_(scene), player_(player), hero_(hero) {}

    void reset();
    void strike();    // the attack button
    void counter();   // the counter button
    void update(float dt);

    bool busy() const { return action_left_ > 0; }   // Batman is in an action: no walking
    bool counter_open() const;                       // an enemy's attack can be countered now
    bool can_take_down() const;                      // the attack button would take down silently
    // The enemy the grapnel would pull: the locked target if it is one, else one the view aims
    // at (within 40 m, a few degrees, in sight); -1 none.
    int pull_target(const glm::vec3& eye, const glm::vec3& forward) const;
    bool pull(int id);   // fire the grapnel at it
    // The batarang (Batman_05_Batarang: Batarang_Throw) at the enemy the view aims at (or the
    // nearest ahead), within 30 m and in sight, if the scripts allow it and there is ammo
    // ("Batarang_01"); it lands a moment later for a heavy hit.
    int batarang_target(const glm::vec3& eye, const glm::vec3& forward) const;
    bool throw_batarang(const glm::vec3& eye, const glm::vec3& forward);
    float batman_health = 100;
    bool auto_fight = false;   // headless tests: strike at enemies in reach, counter every attack

private:
    struct Sniper {
        enum class Phase { Sweep, Aim, Reload } phase = Phase::Sweep;
        float base = 0, sweep = 0, dir = 1;   // degrees: the watched direction, the sweep's offset
        float timer = 0, lost = 0;
    };
    std::unordered_map<int, Sniper> snipers_;
    void update_snipers(float dt);
    void hurt_batman(float damage, const std::string& state);
    bool in_sight(const glm::vec3& from, const glm::vec3& to) const;
    struct Fighter {
        enum class Phase { Idle, Close, Ready, Windup, Recover, Hurt, Down } phase = Phase::Idle;
        float timer = 0;          // seconds left in the phase
        float cooldown = 0;       // seconds before it may attack
        std::string attack;       // the attack being made (its counter and hurt follow from it)
    };
    Fighter& fighter(int id);
    int target(float reach, float ahead) const;   // the best enemy for a strike, or -1
    int leap_target() const;   // an unaware enemy below to drop on (a takedown from above), or -1
    bool enemy(int id) const;
    void act(const std::string& state, int mask, float yaw, float seconds);
    bool actor_state(int id, const std::string& state, bool loop = false);
    void hit(int id, float damage, const std::string& hurt);
    glm::vec3 feet() const;
    float yaw_to(const glm::vec3& at) const;

    LevelLogic& logic_;
    Scene& scene_;
    Player& player_;
    Hero& hero_;
    std::unordered_map<int, Fighter> fighters_;
    float action_left_ = 0;
    int strike_target_ = -1;
    float strike_impact_ = -1;   // seconds until the strike lands (-1: none pending)
    bool takedown_ = false;
    // A leap onto an enemy below: from, to, seconds in and the whole flight.
    glm::vec3 leap_from_{0}, leap_to_{0};
    float leap_t_ = 0, leap_len_ = 0;
    int leap_victim_ = -1;
    int combo_ = 0;
    int batarang_target_ = -1;
    float batarang_flight_ = -1;   // seconds until it lands
    struct Pull {
        int id = -1;
        glm::vec3 from{0}, to{0};
        float t = 0, length = 1;
    } pull_;
    float auto_timer_ = 0;
};

}  // namespace tdkr
