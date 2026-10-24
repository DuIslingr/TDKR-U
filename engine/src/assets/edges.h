// TDKR's grapnel edges (<level>.edge): the roof and ledge edges Batman can grapple to
// (CActorBaseComponent::SearchGrapplingHookEdge). Big-endian: u32 count, then per edge two
// end points, the up vector and the outward normal (12 floats); a spatial grid follows,
// which this does not need.
#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "core/bytes.h"

namespace tdkr {

struct GrappleEdge {
    glm::vec3 a{0}, b{0};
    glm::vec3 up{0, 0, 1};
    glm::vec3 normal{0};    // outward, away from the roof
};

std::vector<GrappleEdge> load_grapple_edges(const Bytes& data);

}  // namespace tdkr
