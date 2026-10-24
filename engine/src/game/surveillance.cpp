// TDKR's surveillance cameras (CSurveillanceCameraComponent 0x62353C58): a camera that sees
// Batman long enough runs its Obj<id>_OnCamDetectTarget (the power plant's call the van with
// thugs), and Obj<id>_OnCamLostTarget when he is gone again. DisableSurveillanceCamera switches
// one off; a batarang breaks it (it is a destroyable with 1 health).
//
// The component ([0] its sweep in degrees, [6] its range in metres, [7] the ms to spot, then
// timings and the OnCamDetectTarget script): the camera looks along its model's +X, turned by
// its yaw. Its sweep (the `camera` node turning by up to [0] either way) is not done: it
// watches straight ahead, where its light cone points.
#include <cmath>

#include "game/level_logic.h"
#include "game/physics.h"
#include "game/player.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompSurveillanceCam = 0x62353C58;
constexpr float kHalfAngle = 25.0f;   // degrees either side of the lens's axis
constexpr float kLensHeight = 0.3f;

}  // namespace

void LevelLogic::find_cameras() {
    cameras_.clear();
    for (const auto& [id, o] : objects_)
        if (const Value* c = object_component(o, kCompSurveillanceCam); c && c->size() >= 8)
            cameras_[id] = {(*c)[6].as_float(), float((*c)[7].i) / 1000.0f};
}

void LevelLogic::disable_camera(int id, bool off) {
    if (auto it = cameras_.find(id); it != cameras_.end()) {
        it->second.off = off, it->second.seen = 0;
        if (trace) log("surveillance camera " + std::to_string(id) + (off ? " off" : " on"));
    }
}

void LevelLogic::update_cameras(float dt) {
    const glm::vec3 chest = player.center + glm::vec3(0, 0, 0.4f);
    for (auto& [id, cam] : cameras_) {
        const Object* o = object(id);
        if (!o || o->dead || !o->enabled || cam.off) {
            cam.seen = 0;
            continue;
        }
        const glm::vec3 lens = o->position + glm::vec3(0, 0, kLensHeight);
        const glm::vec3 to = chest - lens;
        const float dist = glm::length(to);
        const float y = glm::radians(o->yaw);
        const glm::vec2 axis(std::cos(y), std::sin(y));
        bool sees = dist < cam.range && dist > 0.1f &&
                    glm::dot(axis, glm::normalize(glm::vec2(to))) >= std::cos(glm::radians(kHalfAngle));
        if (sees) {
            float t = 1;
            sees = !scene.collision().raycast(lens + to / dist * 0.5f, chest, t);
        }
        if (sees) {
            if ((cam.seen += dt) >= cam.spot && !cam.detected) {
                cam.detected = true;
                if (trace) log("surveillance camera " + std::to_string(id) + " spots Batman");
                start_script("Obj" + std::to_string(id) + "_OnCamDetectTarget", id);
            }
        } else {
            cam.seen = 0;
            if (cam.detected) {
                cam.detected = false;
                start_script("Obj" + std::to_string(id) + "_OnCamLostTarget", id);
            }
        }
    }
}

}  // namespace tdkr
