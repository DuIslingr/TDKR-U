// The Bat (CTheBatHandleComponent): Batman flies it after EnterTheBat(object) until ExitTheBat()
// or the use button. It hovers, with no gravity: a force along its nose against friction, the
// velocity's own direction turned by the steering, the body following the velocity, and a
// separate force for climbing inside a band of heights. LevelLogic::riding says which object is
// flown (riding_bat that it is The Bat); the game flies it, with Batman inside.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace tdkr {

class CollisionWorld;
class LevelLogic;

class TheBat {
public:
    // CComponentTheBatHandle (0x222500F6). The forward top speed is the upgrades'
    // (UpgradesManager::GetTheBatMaxSpeed, item 22, whose table is not in the data we read):
    // [1] here, the speed UpdateNavigate takes as the engine's flat out.
    float mass = 200, max_speed = 24, reverse_speed = 15;
    float steer_units = 7;                // [3] the steering's full scale
    float engine = 11000, reverse = 8000; // [4], [5] N along the nose, ahead and astern
    float lift = 8000;                    // [6] N up or down
    float friction = 1;                   // [7] in g
    float handling = 3;                   // [9] (GetVehicleHandlingParam, no upgrades)
    float climb_speed = 10;               // [10] m/s up or down
    float ceiling = 85, floor = 4;        // [12], [13] world heights
    float roll_limit = 30;                // [14] degrees at full steer and speed
    float pitch_limit = 15;               // [15] degrees under full thrust
    float lock_cone = 45;                 // [18] degrees: a target is taken within it
    float rocket_gap = 500;               // [20] ms between rockets
    float blast_radius = 10, blast_damage = 50, blast_edge = 5;   // [21], [22], [23]
    float rocket_speed = 100, rocket_range = 300;                 // [24], [25] (the range is also the lock's:
                                          // GetVehicleMaxWeaponRange is the upgrades' item 25)

    struct Input {
        float forward = 0, right = 0;     // the stick, -1..1
        bool up = false, down = false;
        bool fire = false;
    };

    void start(LevelLogic& logic, int object);   // take the object over, at rest
    void stop() { object_ = -1; }
    void place(LevelLogic& logic, const glm::vec3& position);   // put it there, at rest
    int object() const { return object_; }
    void update(LevelLogic& logic, const CollisionWorld& world, const Input& in, float dt);
    // The lock and the rockets (UpdateTarget, CBulletRocket): `hurt` takes what a blast does to an
    // object, id and damage.
    void update_weapons(LevelLogic& logic, bool fire, float dt, const std::function<void(int, float)>& hurt);
    void clear_rockets(LevelLogic& logic);   // none left in the air (a level going away)
    void deploy_flares(LevelLogic& logic);   // DeployFlares: the show of it; Missiles takes the rocket
    int flares_deployed() const { return flares_; }
    int target() const { return target_; }   // what the rockets would go for, -1 none
    int rockets_fired() const { return fired_; }

    glm::vec3 position() const { return position_; }
    glm::vec3 velocity() const { return velocity_; }
    float speed() const { return glm::length(glm::vec2(velocity_)); }   // m/s over the ground
    float yaw() const { return yaw_; }         // degrees, game convention (0 along +Y)
    float roll() const { return roll_; }
    float pitch() const { return pitch_; }
    bool reversing() const { return reversing_; }
    float steer() const { return steer_; }
    float steer_limit() const { return steer_limit_; }
    int hits() const { return hits_; }         // steps that met something
    glm::vec3 last_normal() const { return last_normal_; }

private:
    void set_input(const Input& in, float ms);   // SetInputControlDir
    void turn_body(float ms);                    // UpdateRotation
    void physics(float dt);                      // UpdatePhysics
    void fly(const CollisionWorld& world, float dt);   // UpdateNavigate, SmoothCollisionBehavior
    glm::vec3 nose() const;                      // the way it points, pitch and all
    int find_target(LevelLogic& logic) const;    // UpdateTarget, its cone and range
    void launch(LevelLogic& logic);              // onEvent 0xB1, CBulletRocket::Start
    void explode(LevelLogic& logic, size_t rocket, const std::function<void(int, float)>& hurt);   // Explode, SplashDamage

    int object_ = -1;
    glm::vec3 position_{0}, velocity_{0};
    glm::vec3 thrust_{0}, drag_{0};   // the forces (N)
    float yaw_ = 0, roll_ = 0, pitch_ = 0;
    float steer_ = 0;         // +0x40: builds up to +- limit while the stick is over
    float steer_limit_ = 0;   // +0xB4: how far the stick lets it go
    int steer_kick_ = 0;      // +0x44: a wall turns the nose away
    float turn_ = 0;          // +0x3C: degrees the velocity turns this step
    bool reversing_ = false;  // +0xE4
    int hits_ = 0;
    glm::vec3 last_normal_{0};
    struct Rocket {
        bool flying = false;
        glm::vec3 position{0}, from{0}, direction{0};
        int target = -1;     // +0x14: it homes on it while it is there
        int trail = -1;      // the emp_trail effect
    };
    std::vector<Rocket> rockets_;   // the original's 20
    size_t next_rocket_ = 0;        // +0x19C: with none free, the next goes round
    std::string blast_effect_ = "emp", trail_effect_ = "emp_trail";   // [26], [27]
    std::string muzzles_[2] = {"gun2_muzzle_01", "gun2_muzzle_02"};   // [31], [32]
    bool right_muzzle_ = false;     // +0x1F4
    float rocket_wait_ = 0;         // +0x12C, ms
    int target_ = -1;               // +0xC4
    int fired_ = 0;
    int flares_ = 0;
};

}  // namespace tdkr
