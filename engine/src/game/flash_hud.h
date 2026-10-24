// TDKR's in-game HUD: BatmanHUD.swf played by the Flash player and set up the way
// CHUDDisplay::Init / SetBatmanInterface / SetMinimapInterface leave it on foot (action
// buttons, minimap with the level's <level>_minimap.swf loaded into it, pause button), with
// the grapnel target icon over the edge the grapnel would take.
#pragma once

#include <map>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "game/flash_screen.h"

namespace tdkr {

class Vfs;
class StringTable;

class FlashHud {
public:
    FlashHud(const Vfs& vfs, Renderer& renderer, const StringTable* strings);
    ~FlashHud();

    // Load the HUD for a level with its minimap (level properties: the movie, pixels per metre,
    // zoom). False if BatmanHUD.swf is missing.
    bool start(const std::string& minimap, float minimap_scale, float minimap_zoom);
    bool active() const { return screen_.loaded(); }

    struct State {
        bool visible = true;
        bool cinematic = false;
        bool grapple = false;            // a grapnel edge in reach: the target icon at `grapple_at`
        glm::vec2 grapple_at{0};         // screen pixels
        float health = 1;                // 0..1
        glm::vec2 position{0};           // Batman, world metres (the minimap follows him)
        float camera_yaw = 0, facing = 0;   // degrees about Z (the map turns with the camera)
        enum class Move { Ground, Air, Glide, Plunge } move = Move::Ground;   // the jump slot's style
        const char* attack = "STRIKE";   // the attack slot's style (STRIKE, TAKEDOWN, COUNTERATTACK)
    };
    void update(float dt, const State& state, int width, int height);
    // The objective popup (objectiveMessage_popin): show with text, or hide.
    void objective(bool shown, const std::string& text);
    void run(float seconds);   // play the movie on (headless screenshots)
    void draw(ParticleList& out);
    swf::Player* player() { return screen_.player(); }

private:
    void set_up();
    // CHUDDisplay::SetButtonStyle: the slot's button switches style and (dis)ables, once per change.
    void button(const std::string& slot, const std::string& style, bool enabled);
    std::map<std::string, std::pair<std::string, int>> buttons_;   // slot -> (style, enabled)
    FlashScreen screen_;
    State state_;
    int width_ = 0, height_ = 0;
    std::string minimap_;
    float minimap_scale_ = 1, minimap_zoom_ = 1;
    glm::vec2 map_center_{0}, map_zero_{0};   // twips
    bool grapple_shown_ = false;
    std::map<std::string, float> left_base_;   // the left group's authored x (twips)
};

}  // namespace tdkr
