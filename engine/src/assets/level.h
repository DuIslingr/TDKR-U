// Levels (.lvc) and the game object library (gol.bin). See tools/lvc.py for the format.
//
// A level is a stream of objects until -1: u32 type, then exactly the fields the game's
// loader reads for that type (no lengths). Level-structure types (zones, portals, spawn
// points...) have their own specs; any other type is a game object whose data follows
// its gol.bin type's own components: each flag-0 component's Load (or template reader),
// plus group objects' builtin data. TDKR identifies components by 32-bit hashes.
// The readers come from the generated specs in level_specs.inc.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/dict_stream.h"

namespace tdkr {

constexpr int kCompBase = 0x0014CA79;       // CComponentBase: c, id, position, rotation (degrees), scale, 3 flags
constexpr int kCompMesh = 0x00152B87;       // CComponentMesh: model name, 4 flags
constexpr int kCompAnimation = 0x0F5F8CFD;  // CAnimationComponent: s(animation file) s(default clip) f(speed) ...
constexpr int kCompCollision = 0x2DB081E2;  // CCollisionComponent: c c s(collision model) ...
constexpr size_t kCollisionModelField = 2;
constexpr int kCompLight = -1;              // N.O.V.A. 3's CLightComponent; TDKR's lights are not mapped yet

struct Transform {
    int id = 0;
    glm::vec3 position{0};
    glm::vec3 rotation{0};   // degrees, applied X then Y then Z (Irrlicht setRotationDegrees)
    glm::vec3 scale{1};
    bool visible = true;     // CComponentBase flags: shown (enabled and visible) when the level starts
    bool enabled = true;     // enabled (it updates, can be used), shown or not
};

struct GolType {
    int id = 0;
    std::string name;
    std::vector<int> parents;
    std::vector<std::pair<int, int>> components;   // (component hash, flag); flag 0 = stored per instance
    std::map<int, Value> templates;                // component id -> template values
    bool templates_ok = false;
};

using Gol = std::unordered_map<int, GolType>;

struct NavMesh {
    std::vector<glm::vec3> vertices;                // world space
    std::vector<std::array<int16_t, 8>> polygons;   // 3 vertex indices, 3 neighbours, 2 flags
    // Links to other nav meshes through doors: polygon, a point in the doorway, the door's id.
    struct Link {
        int polygon = -1;
        glm::vec3 point{0};
        int door = -1;
    };
    std::vector<Link> links;
};

struct LevelObject {
    size_t offset = 0;
    size_t content_end = 0;    // zones: end of the records the game skips with them (inactive mission zones)
    int type = 0;
    std::string kind;          // "object", "zone", "nav_mesh", "spawn_point", ...
    std::string name;          // object type name from gol.bin
    std::optional<Transform> base;
    std::string model;         // per-instance mesh component model
    uint32_t mesh_flags = 0;   // N.O.V.A. 3's fade flags; TDKR's mesh component has 4 bytes, not mapped yet
    std::map<int, Value> components;
    Value fields;              // structure-type fields
    NavMesh nav;
};

struct Level {
    int header_count = 0;
    std::vector<LevelObject> objects;
    std::vector<std::string> strings;
};

Gol load_gol(const Bytes& data);
Level load_level(const Bytes& data, const Gol& gol);

// N.O.V.A. 3 splits levels into sections by zone masks. TDKR has no such masks (it skips
// inactive mission zones instead, which needs the mission state), so this keeps everything.
void keep_section(Level& level, uint32_t section_mask);

// Components including inherited ones (parents first, the type's own entries win). Only the
// type's own components store level data (CGameObjectManager::CreateObject).
std::vector<std::pair<int, int>> resolved_components(const GolType& type, const Gol& gol);
// Template values for `comp` from the type or its nearest parent; nullptr if none.
const Value* resolved_template(const GolType& type, const Gol& gol, int comp);

struct Placement {
    size_t object_index = 0;   // into Level::objects
    std::string model;
    uint32_t mesh_flags = 0;   // LevelObject::mesh_flags, or the template's
    Transform transform;
    std::string object;
    std::string anim_file;     // from the animation component, if any
    std::string anim_clip;
    float anim_speed = 1.0f;
    bool animated = false;     // keep per-node transforms even without clips (runtime models)
};

// Visible models placed in the level: per-instance meshes, or the type's template mesh.
std::vector<Placement> model_placements(const Level& level, const Gol& gol, bool library = true);
// Collision components: collision model name (.bdae; the game looks for .col) per object.
std::vector<Placement> collision_placements(const Level& level, const Gol& gol);

Transform transform_from_values(const Value& v, size_t first = 0);

// Skybox records (level type 0x2661: s model, c, f, i id): the model, the id scripts pass to
// SetSkybox, and whether it is shown when the level starts (LoadNextObject always adds it).
struct SkyboxRef {
    std::string model;
    int id = 0;
    bool initial = false;
};
std::vector<SkyboxRef> level_skyboxes(const Level& level);

// Light records (level type 0x265E: base, then CComponentLight::Load "f f i i f").
struct LevelLight {
    std::string object;
    glm::vec3 position{0};
    glm::vec4 color{1};       // RGBA, bytes in that order
    float intensity = 1, radius = 1;
    bool fake = false;        // registered with its zone as a "fake light"
    size_t index = 0;         // the level object
};
std::vector<LevelLight> level_lights(const Level& level, const Gol& gol);

}  // namespace tdkr
