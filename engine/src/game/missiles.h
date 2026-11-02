// The bazooka's rockets (CMissileComponent, gol 20011 "Rocket"): they climb, then turn after their
// target and blow up on it, on the scenery, or when their time is up. The player sees them coming
// (PlayerComponent's inbound missiles); The Bat's flares (the counter button) destroy the nearest.
// Bazooka_Enemy actors fire them here too: CNpcAIComponent's ranged attack with Templ9801_2's
// Attack_Aim / Attack_Ranged.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace tdkr {

class CollisionWorld;
class LevelLogic;

class Missiles {
public:
    // CComponentMissile (0x35C5E0A0): read from "Rocket" by load().
    float life = 10000;                          // [0] ms
    float start_speed = 10, top_speed = 25, acceleration = 10;   // [1], [2], [3]
    float yaw_rate = 200, pitch_rate = 400;      // [4], [5] degrees a second
    float climb = 20;                            // [6] m, with no target
    float cone_yaw = 45, cone_pitch = 45;        // [7], [8] degrees: the lock holds within them
    float blast_radius = 6, blast_inner = 0.5f;  // [9], [10]
    float damage_edge = 10, damage_middle = 35;  // [11], [12] (a vehicle takes [13] / [14], also 35)
    std::string smoke = "car_exhaust_smoke", blast = "grenade_exp";   // [15], [16]

    // UpgradesManager::GetTheBatCountermeasuresRange, item 26: its table is not in the data we read.
    float flare_range = 30;
    float weapon_range = 100;                    // the RocketLauncher's reach

    void load(LevelLogic& logic);
    void reset(LevelLogic& logic);   // none in the air, and the bazookas start over

    // Everything in one step. `at` is where the rockets go for (The Bat, or Batman's middle);
    // `hurt` takes what a blast does to the player; `flying` says The Bat is flown.
    void update(LevelLogic& logic, const CollisionWorld& world, const glm::vec3& at, bool flying, float dt,
                const std::function<void(float, const glm::vec3&)>& hurt);

    // PlayerComponent::CanUseCounterMeasures: the nearest inbound rocket is within reach.
    bool can_counter() const;
    // onEvent 0x44: CMissileComponent::Destroy on it, harmlessly here: it burns up in the flares.
    bool counter(LevelLogic& logic);

    int inbound() const;                 // rockets homing in
    float nearest() const;               // how far the nearest of them is, -1 with none
    glm::vec3 nearest_position() const;  // and where (CHUDDisplay::SetRocket)
    bool locked() const;                 // it has the lock (the warning, CHUDDisplay::SetRocketLock)
    int fired() const { return fired_; }
    int exploded() const { return exploded_; }
    int countered() const { return countered_; }

private:
    struct Missile {
        glm::vec3 position{0}, last{0};
        float yaw = 0, pitch = 0;     // degrees: the way it flies, 0 along +Y, up positive
        float speed = 0, climbed = 0, height = 0, life = 0;   // +0x34, +0x28, +0x3C, +0x40
        enum class State { Climb = 1, Follow = 2 } state = State::Climb;
        int lock = 0;                 // +0x44: 0 lost, 1 locked on
        float lock_wait = 0;          // +0x48, ms
        bool close = false;           // +0x4D: within 5 m, the slow motion is on
        float range = -1;             // PlayerComponent's distance, set from its first HUD update
        int smoke = -1;
        int owner = -1;               // who fired it (CWeapon::CanFireBullet: one each)
    };
    struct Bazooka {
        enum class Phase { Idle, Aim, Shoot } phase = Phase::Idle;
        float timer = 0;
        float range = 100;
    };

    void climb_step(Missile& m, float seconds);        // UpdateStateGoUp
    void follow(LevelLogic& logic, Missile& m, const glm::vec3& at, float ms);   // UpdateStateFollow
    void destroy(LevelLogic& logic, size_t i, const glm::vec3& at, bool burn,
                 const std::function<void(float, const glm::vec3&)>& hurt);      // Destroy (burn: in the flares)
    void launch(LevelLogic& logic, int owner, const glm::vec3& at);              // CWeapon::FireBullet
    void update_bazookas(LevelLogic& logic, const CollisionWorld& world, const glm::vec3& at, bool flying, float dt);

    std::vector<Missile> missiles_;
    std::unordered_map<int, Bazooka> bazookas_;
    bool loaded_ = false, warning_ = false;
    float scan_ = 0;
    int fired_ = 0, exploded_ = 0, countered_ = 0;
};

}  // namespace tdkr
