// The Batpod (CBatpodHandleComponent): Batman rides it after EnterBatPod(object) until
// ExitBatPod(). The stick drives it: forward accelerates (force / mass), back brakes and then
// reverses (slowly), left and right steer on a radius that grows with speed. It keeps to the
// ground under it, slides along walls, and falls off edges. LevelLogic::riding says which
// object is ridden; the game drives it, with Batman on its seat.
#pragma once

#include <glm/glm.hpp>

namespace tdkr {

class CollisionWorld;
class LevelLogic;
struct PlayerInput;

class Batpod {
public:
    // CComponentBatpodHandle (0x0D1839C4): [0] mass, [3] top speed backwards, [6] the engine's
    // force, [7] the brakes' / reverse force. The forward top speed is the upgrades'
    // (UpgradesManager::GetBatPodMaxSpeed, item 16, not in Upgrades.bin): an estimate here.
    float mass = 100, max_speed = 40, reverse_speed = 4, force = 1000, brake_force = 500;
    float drag = 3;              // m/s^2 slowing with the stick let go
    float min_radius = 6.5f;     // steering radius at a standstill, metres ([10])
    float radius_per_speed = 0.35f;

    // Take the object over (its position and heading, its handling from the component).
    void start(LevelLogic& logic, int object);
    void stop() { object_ = -1; }
    void place(LevelLogic& logic, const glm::vec3& position, float yaw);   // put it there, standing
    int object() const { return object_; }
    // One step: `in.forward` throttle (< 0 brake / reverse), `in.right` steer.
    void update(LevelLogic& logic, const CollisionWorld& world, const PlayerInput& in, float dt);

    glm::vec3 position() const { return position_; }
    float yaw() const { return yaw_; }         // degrees, game convention (0 along +Y)
    float speed() const { return speed_; }     // m/s along the heading, < 0 backwards
    float steer() const { return steer_; }     // -1 .. 1, eased

private:
    int object_ = -1;
    glm::vec3 position_{0};
    float yaw_ = 0, speed_ = 0, steer_ = 0, fall_ = 0;
};

}  // namespace tdkr
