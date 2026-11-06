// The weapons actors carry: CActorBaseComponent's last field lists [weapon type, equipped];
// the equipped one's model (its CComponentMesh) hangs on the actor's node its
// CDummiesComponent names, [0] in the hand (the rifles' and the launcher's Dummy_HK, the
// knives' Dummy_Gun_hand), [1] sheathed (Dummy_gun). Here the equipped weapon stays in the
// hand while its actor shows and lives (a killed actor's is detached, gone); the sheathing
// and the civilians' umbrellas (equipped too, for the rain) are not done.
#include <algorithm>

#include "game/level_logic.h"
#include "game/player.h"
#include "game/scene.h"

namespace tdkr {

namespace {

constexpr int kCompActorBase = 0x54989cb1, kCompDummies = 0x33702f4a;   // (kCompMesh: assets/level.h)

}  // namespace

const LevelLogic::NpcWeapon* LevelLogic::npc_weapon(int type) {
    auto [it, added] = npc_weapon_types_.try_emplace(type);
    if (!added) return it->second.model.empty() ? nullptr : &it->second;
    const auto g = gol.find(type);
    if (g == gol.end()) return nullptr;
    const Value* actor = resolved_template(g->second, gol, kCompActorBase);
    if (!actor || actor->size() == 0) return nullptr;
    for (const Value& item : (*actor)[actor->size() - 1].list) {
        if (item.size() < 2 || item[1].i != 1) continue;
        const auto w = gol.find(item[0].i);
        if (w == gol.end()) continue;
        // Not an actor's hand weapon: the vehicles' guns, and the umbrellas of the rain.
        if (w->second.name == "Umbrella" || w->second.name == "BatpodWeapon" || w->second.name == "HelicopterWeapon") continue;
        const Value* mesh = resolved_template(w->second, gol, kCompMesh);
        const Value* dummies = resolved_template(w->second, gol, kCompDummies);
        if (!mesh || mesh->size() == 0 || mesh->list[0].s.empty() || !dummies || dummies->size() == 0) continue;
        const Value& d = (*dummies)[0];
        if (d.size() == 0 || d[0].size() == 0 || d[0][0].s.empty()) continue;
        it->second.model = mesh->list[0].s, it->second.node = d[0][0].s;
        return &it->second;
    }
    return nullptr;
}

void LevelLogic::update_npc_weapons(float dt) {
    // New actors (spawned, or a zone's) are looked for twice a second.
    npc_weapon_scan_ -= dt;
    if (npc_weapon_scan_ <= 0) {
        npc_weapon_scan_ = 0.5f;
        for (const auto& [id, o] : objects_) {
            if (npc_weapons_.count(id) || o.index == std::numeric_limits<size_t>::max() || !o.loaded) continue;
            const NpcWeapon* w = npc_weapon(object_type(o));
            if (!w) continue;
            const std::vector<std::string> nodes = scene.node_names(o.index);
            const bool has_node = std::find(nodes.begin(), nodes.end(), w->node) != nodes.end();
            if (trace && warned_.insert("weapon " + o.type).second)
                log(o.type + " holds " + w->model + (has_node ? " at " + w->node : ": its model has no " + w->node));
            const size_t body = has_node ? scene.spawn_model(w->model, false) : size_t(-1);
            // The guns' laser (laser, its billboards) and bullets show only while targeting
            // (CWeapon::SetLaserDummyVisible) or firing (EnableBulletDummy).
            float base = 0;
            if (body != size_t(-1)) {
                const std::vector<std::string> parts = scene.node_names(body);
                for (const std::string& n : parts)
                    if (n.rfind("laser", 0) == 0 || n.rfind("bullet", 0) == 0) scene.set_node_visible(body, n, false);
                // CWeapon's constructor: the laser's length as modelled, Dummy_targetend's offset.
                auto has = [&](const char* n) { return std::find(parts.begin(), parts.end(), n) != parts.end(); };
                if (has("laser") && has("Dummy_targetend"))
                    base = glm::distance(glm::vec3(scene.object_matrix(body, "laser")[3]), glm::vec3(scene.object_matrix(body, "Dummy_targetend")[3]));
            }
            npc_weapons_[id] = {body, w->node, base, false};
        }
    }
    const auto& level_objects = scene.level().objects;
    for (auto& [id, held] : npc_weapons_) {
        if (held.body >= level_objects.size()) continue;
        const auto it = objects_.find(id);
        const Object* o = it == objects_.end() ? nullptr : &it->second;
        // (CGameObject::Kill -> OnObjectKilled -> CWeapon::Detach: a dead actor's weapon goes.)
        const bool shown = o && o->visible && o->enabled && o->loaded && !o->dead && !scene.object_hidden(o->index);
        scene.set_object_visible(held.body, shown);
        if (shown) scene.set_object_transform(held.body, scene.object_matrix(o->index, held.node));
        // CWeapon::UpdateTargeting: while targeting, the laser runs from the weapon's
        // Dummy_target to the target (or what is in the way), its length the modelled one
        // scaled (SetLaserDummyLength); it is modelled down -Y. Its billboards stay hidden.
        const auto target = weapon_targets.find(id);
        glm::vec3 at{0};
        bool aiming = shown && held.laser_base > 0 && target != weapon_targets.end();
        if (aiming) {
            if (target->second == 0 || target->second == kPlayerId) at = player.center;
            else if (const auto t = objects_.find(target->second); t != objects_.end()) at = t->second.position + glm::vec3(0, 0, 1.2f);
            else aiming = false;
        }
        if (aiming) {
            const glm::vec3 from(scene.object_matrix(held.body, "Dummy_target")[3]);
            float length = glm::distance(from, at), hit = 1;
            if (length > 0.05f && scene.collision().raycast(from, at, hit)) length *= hit;
            const glm::vec3 y = length > 0.05f ? (from - at) / glm::distance(from, at) : glm::vec3(0, -1, 0);
            glm::vec3 x = glm::cross(y, glm::vec3(0, 0, 1));
            x = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3(1, 0, 0);
            const glm::vec3 z = glm::cross(x, y);
            const glm::mat4 beam(glm::vec4(x, 0), glm::vec4(y * (length / held.laser_base), 0), glm::vec4(z, 0), glm::vec4(from, 1));
            scene.set_node_world(held.body, "laser", &beam);
        }
        if (aiming != held.laser_on) {
            held.laser_on = aiming;
            scene.set_node_visible(held.body, "laser", aiming);
            scene.set_node_visible(held.body, "laser_PIVOT", aiming);
            if (!aiming) scene.set_node_world(held.body, "laser", nullptr);
        }
    }
}

}  // namespace tdkr
