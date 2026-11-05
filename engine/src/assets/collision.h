// Collision geometry (.col files). See tools/col.py.
//
// A .col is a DICT stream: u32 group count; per group a name, u32 triangle count,
// and per triangle 9 big-endian floats (three vertices) plus a material byte.
// CCollisionComponent names "<model>.bdae" and the game loads "<model>.col"; when
// there is no .col, the named model's own triangles are used instead.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "core/bytes.h"
#include "core/vfs.h"

namespace tdkr {

constexpr uint8_t kMaterialNone = 255;           // no collision type
// GPMaterial::GetCollisionTypeFromMaterialName: a collision model's material named exactly one of
// GConcrete, GWetConcrete, GMetal, GWood, GDirt, GWater, Solid, Box, WalkNoNavMesh, Fence, Stop,
// SolidNoEdge, BoxNoEdge is that type, 0..12 (TDKR's collision is all models; "Ground" is none).
uint8_t collision_type(const std::string& material);
inline bool is_invisible_material(uint8_t m) {   // CPhysicsManager::isColMatInvisible
    return m == 18 || m == 19;
}

struct CollisionMesh {
    std::vector<glm::vec3> vertices;   // 3 per triangle, in the model's space
    std::vector<uint8_t> materials;    // 1 per triangle
    size_t triangle_count() const { return materials.size(); }
};

CollisionMesh load_col(const Bytes& data);

// Resolve a collision component's model name the way the game does.
std::optional<CollisionMesh> load_collision(const Vfs& vfs, const std::string& name);

}  // namespace tdkr
