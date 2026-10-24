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
    float batman_health = 100;
    bool auto_fight = false;   // headless tests: strike at enemies in reach, counter every attack

private:
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
    float auto_timer_ = 0;
};

}  // namespace tdkr
