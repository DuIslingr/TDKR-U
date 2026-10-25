// TDKR's exhaust fans (CExhaustFanComponent 0x65DA04ED): an updraft over the fan, a column
// along its up axis ([4] radius in metres, [7] its length) whose push is [6] at the fan and
// [5] less per metre above it (GetEffect). The level sums the pushes where Batman is
// (CLevel +0xb10..+0xb1c); his glide takes them, and a jump in a strong one (2 and up:
// PlayerComponent::CanGlideJump) launches him up the column (GlideJump). EnableExhaustFan
// switches one on or off ([3] set: it starts off).
//
// The push's unit is not known: here it is a lift of kLiftPerForce m/s per unit (130 at a
// fan: 19.5 m/s), which carries Batman some 50 m up a fan before the glide's sink wins.
#include <algorithm>

#include "game/level_logic.h"

namespace tdkr {

namespace {

constexpr int kCompExhaustFan = 0x65DA04ED;
constexpr float kLiftPerForce = 0.15f;

}  // namespace

void LevelLogic::find_fans() {
    fans_.clear();
    for (const auto& [id, o] : objects_)
        if (const Value* f = object_component(o, kCompExhaustFan); f && f->size() >= 8)
            fans_[id] = {std::max((*f)[4].as_float(), 1.0f), (*f)[5].as_float(), (*f)[6].as_float(), (*f)[7].as_float(), (*f)[3].i == 0};
}

void LevelLogic::enable_fan(int id, bool on) {
    if (auto it = fans_.find(id); it != fans_.end()) it->second.on = on;
}

glm::vec3 LevelLogic::updraft(const glm::vec3& feet) const {
    glm::vec3 lift(0);
    for (const auto& [id, fan] : fans_) {
        const auto it = objects_.find(id);
        if (!fan.on || it == objects_.end() || !it->second.loaded || !it->second.enabled) continue;
        const glm::vec3& at = it->second.position;   // (the fans stand upright: the axis is +Z)
        const float up = feet.z - at.z;
        if (up < -1.0f || up > fan.height || glm::distance(glm::vec2(feet), glm::vec2(at)) > fan.radius) continue;
        lift.z += std::max(fan.force - std::max(up, 0.0f) * fan.falloff, 0.0f) * kLiftPerForce;
    }
    return lift;
}

}  // namespace tdkr
