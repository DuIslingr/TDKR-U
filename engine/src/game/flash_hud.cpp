#include "game/flash_hud.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "core/vfs.h"

namespace tdkr {

namespace {
const char* const kLeftGroup[] = {"mMinimapMask", "minimapBG", "MinimapPivotInstance", "CenterMinimap", "MinimapInstance",
                                  "mPauseButtonInstance"};
}

FlashHud::FlashHud(const Vfs& vfs, Renderer& renderer, const StringTable* strings) : screen_(vfs, renderer, strings) {}

FlashHud::~FlashHud() = default;

bool FlashHud::start(const std::string& minimap, float minimap_scale, float minimap_zoom) {
    minimap_ = minimap, minimap_scale_ = minimap_scale, minimap_zoom_ = minimap_zoom;
    // The HUD's own events: the win screen's button.
    screen_.on_native = [this](const std::string& name, swf::CallArgs& a) {
        if ((name == "registerInGameEvent" || name == "registerInGameEventExtended") && a.size() > 0 &&
            a[0].type == swf::Value::String && a[0].s == "CONTINUE_FROM_WIN")
            close_win();
        return swf::Value();
    };
    win_open_ = false;
    if (!screen_.load("BatmanHUD.swf", width_, height_)) return false;   // 0: the movie's size until the first update
    width_ = height_ = 0;   // the next update sets the viewport and the screen alignment
    left_base_.clear();
    buttons_.clear();
    for (const char* slot : {"bJump", "bAttack", "bGrapnel"}) buttons_[slot] = {"", -1};
    set_up();
    return true;
}

// What CHUDDisplay::Init, SetBatmanInterface(false, ...) and SetMinimapInterface leave visible
// for Batman on foot, as in the gameplay footage: the minimap top left, the pause button, the
// gadget button top right, jump / strike / grapnel on the right. The touch joystick, QTE
// slider, vehicle panels and the popups wait for their events.
void FlashHud::set_up() {
    swf::Player& p = *screen_.player();
    p.goto_and_stop("", "HUD");
    cinematic_ = false, dialog_.clear();
    marker_frames_.clear();
    hide_unused();
    // CHUDDisplay::SetMinimapSwf / initMap: the level's map goes into the pivot's dummy clip,
    // the pivot scaled by the level's zoom about the minimap centre.
    if (!minimap_.empty() && !p.load_movie("MinimapPivotInstance.MinimapDummyyInstance", minimap_))
        std::fprintf(stderr, "hud: could not load %s\n", minimap_.c_str());
    if (swf::Instance* center = p.find("CenterMinimap")) map_center_ = glm::vec2(center->matrix.tx, center->matrix.ty);
    // The map's world origin ("Zero" in the minimap movie).
    if (swf::Instance* zero = p.find("MinimapPivotInstance.MinimapDummyyInstance.Zero"))
        map_zero_ = glm::vec2(zero->matrix.tx, zero->matrix.ty);
}

void FlashHud::hide_unused() {
    swf::Player& p = *screen_.player();
    for (const char* hidden : {"bJoystickBase", "bJoystickStick", "bSliderBack", "bSliderButton", "bSkipCutscene",
                               "CinematicHUDInstance", "mVehicleInterface", "mCarInterfaceInstance", "bUtility",
                               "bCounterAttack", "bInteract", "bSonar", "mGraplingHookTarget", "mGraplingHookOffscreenTarget",
                               "mTapAnimationInstance", "mQTEButtonInstance", "mQTESpamButtonInstance", "mQTESliderInstance",
                               "mTrailCircleInstance", "mSlideAnimationInstance", "MissileLockOn_RocketDir", "MissileLockOn_Target",
                               "MissileLockOn_RocketDirText", "mCircularLock", "mVehicleTarget", "baneHealthBar", "miniMissionHUD",
                               "cameraTutorialAnim", "countdowntimer_instance", "WantedLevel_Instance", "ObjectiveBtn",
                               "objectiveMessage_popin", "mCompletionMessage", "CrimeZoneNoticeInstance", "areaClearence_PopUp",
                               "XPGain_PopUp_Instance", "XPGain_Continous_PopUp_Instance", "CheckpointMessageInstance",
                               "disarmInstance", "mInformationScreenInstance", "mAchievementsIcons", "mShopIcons", "Dialog"})
        p.set_visible(hidden, false);
    // The unnamed pieces placed on the HUD frame (the utility action panel, the map's NPC icon).
    if (swf::Instance* root = p.find("")) {
        for (auto& c : root->children) {
            const std::string link = c->movie ? c->movie->export_name(c->character) : "";
            if (link == "BTUtilityAction" || link == "MapNPCIcon") c->visible = false;
        }
    }
    // The gadget wheel stays closed: only its button (top right) shows.
    for (const char* item : {"gadgetWheel.bG1", "gadgetWheel.bG2", "gadgetWheel.bG3", "gadgetWheel.bG4"}) p.set_visible(item, false);
}

void FlashHud::update(float dt, const State& state, int width, int height) {
    state_ = state;
    if (!screen_.loaded()) return;
    swf::Player& p = *screen_.player();
    if (width != width_ || height != height_) {
        width_ = width, height_ = height;
        p.set_viewport(float(width), float(height));
        // CHUDDisplay's screen matrix (0x520): the minimap group sits at the screen's left edge.
        const float left = -p.offset().x / p.scale() * 20.0f;
        for (const char* name : kLeftGroup)
            if (swf::Instance* i = p.find(name)) {
                auto [it, added] = left_base_.try_emplace(name, i->matrix.tx);
                i->matrix.tx = it->second + left;
                i->accept_moves = false;
            }
        if (auto it = left_base_.find("CenterMinimap"); it != left_base_.end()) map_center_.x = it->second + left;
        // The letterbox bars span the stage; stretched across the whole screen.
        if (swf::Instance* bars = p.find("CinematicHUDInstance"); bars && p.scale() > 0) {
            const float stage = float(width) - 2 * p.offset().x;
            if (stage > 0) bars->matrix.a = float(width) / stage, bars->matrix.tx = left;
            bars->accept_moves = false;
        }
    }
    // CHUDDisplay::SetGrapple: the target icon over the edge, in movie units.
    if (swf::Instance* target = p.find("mGraplingHookTarget")) {
        target->visible = state.grapple && state.visible && !state.cinematic;
        if (state.grapple) {
            const glm::vec2 at = (state.grapple_at - p.offset()) / p.scale();
            target->matrix.tx = at.x * 20.0f;
            target->matrix.ty = at.y * 20.0f;
            target->accept_moves = false;
            if (!grapple_shown_) target->goto_frame(0, true);
        }
        grapple_shown_ = state.grapple;
    }
    if (win_open_) {   // the win screen alone
        screen_.update(dt, width, height);
        return;
    }
    if (state.cinematic != cinematic_) cinematic(state.cinematic);
    update_markers(state.cinematic || !state.visible ? std::vector<State::Marker>{} : state.markers);
    if (state.dialog != dialog_) dialog(state.dialog);
    // CHUDDisplay::updateMap: the map moves under the centre (y up in the world is up on the
    // map), the pivot turns with the camera, the arrow shows Batman's facing against it.
    const float yaw = glm::radians(state.camera_yaw);
    if (swf::Instance* dummy = p.find("MinimapPivotInstance.MinimapDummyyInstance")) {
        dummy->matrix = swf::Matrix{};
        dummy->matrix.tx = -(map_zero_.x + minimap_scale_ * state.position.x * 20.0f);
        dummy->matrix.ty = minimap_scale_ * state.position.y * 20.0f - map_zero_.y;
        dummy->accept_moves = false;
    }
    if (swf::Instance* pivot = p.find("MinimapPivotInstance")) {
        pivot->matrix.set_scale_rotation(minimap_zoom_, minimap_zoom_, yaw);
        pivot->matrix.tx = map_center_.x, pivot->matrix.ty = map_center_.y;
        pivot->accept_moves = false;
    }
    if (swf::Instance* arrow = p.find("CenterMinimap")) {
        arrow->matrix.set_scale_rotation(1, 1, yaw - glm::radians(state.facing));
        arrow->accept_moves = false;
    }
    // refreshBatmanButtons / Update: the action slots follow what Batman can do now.
    using M = State::Move;
    button("bJump", state.move == M::Glide ? "GLIDE" : state.move == M::Plunge ? "PLUNGE" : "JUMP", state.jump_on);
    button("bAttack", state.attack, state.move == M::Ground && state.attack_on);
    button("bGrapnel", "GRAPNEL", state.grapple && state.grapnel_on);
    // CHUDDisplay: the counter button shows while an enemy's attack can be countered.
    p.set_visible("bCounterAttack", state.counter && !state.cinematic);
    screen_.update(dt, width, height);
}

void FlashHud::win_screen(const std::string& title, int xp, int coins, float seconds) {
    swf::Player* p = screen_.player();
    if (!p) return;
    p->goto_and_stop("", "WinScreen");
    p->set_text("mWinScreen.headerText.textB", title);
    p->set_text("mWinScreen.t_xp", std::to_string(xp));
    p->set_text("mWinScreen.t_wayneTech", std::to_string(coins));
    char time[16];
    std::snprintf(time, sizeof time, "%d:%02d", int(seconds) / 60, int(seconds) % 60);
    p->set_text("mWinScreen.missionTime", time);
    p->set_text("mWinScreen.timeText", "");   // (the movie would show "undefined")
    p->set_visible("bBack", true);
    win_open_ = true;
}

void FlashHud::checkpoint_message(const std::string& text) {
    swf::Player* p = screen_.player();
    if (!p || win_open_) return;
    p->set_visible("CheckpointMessageInstance", true);
    p->invoke("CheckpointMessageInstance.CheckpointText", "setText", {swf::Value(text)});
    p->goto_and_play("CheckpointMessageInstance", "Anim");
}

void FlashHud::close_win() {
    if (!win_open_ || !screen_.player()) return;
    win_open_ = false;
    for (auto& [slot, last] : buttons_) last = {"", -1};
    set_up();
    width_ = height_ = 0;   // the next update lays the HUD out again
}

void FlashHud::press_continue() {
    if (swf::Player* p = screen_.player(); p && win_open_)
        if (swf::Instance* b = p->find("bBack")) p->press(b);
    close_win();
}

void FlashHud::update_markers(const std::vector<State::Marker>& markers) {
    swf::Player& p = *screen_.player();
    swf::Instance* icon = p.find("ObjectiveBtn");
    if (!icon) return;
    auto clip_name = [](int id) { return "objectiveMarker" + std::to_string(id); };
    // Gone markers lose their icons.
    for (auto it = marker_frames_.begin(); it != marker_frames_.end();) {
        const bool kept = std::any_of(markers.begin(), markers.end(), [&](const State::Marker& m) { return m.id == it->first; });
        if (!kept) {
            if (swf::Instance* c = p.find(clip_name(it->first))) c->remove_self();
            it = marker_frames_.erase(it);
        } else {
            ++it;
        }
    }
    int depth = 20000;
    for (const State::Marker& m : markers) {
        const std::string name = clip_name(m.id);
        swf::Instance* c = p.find(name);
        if (!c) {
            icon->duplicate(name, depth + int(marker_frames_.size()));
            c = p.find(name);
            if (!c) continue;
        }
        std::string kind = m.kind.empty() ? "objective" : m.kind;
        kind[0] = char(std::tolower(static_cast<unsigned char>(kind[0])));   // "Objective" -> "objective"
        if (c->find_label(kind) < 0) kind = "objective";
        const std::string frame = m.on_screen ? kind : kind + "_arrow";
        if (marker_frames_[m.id] != frame) {
            p.goto_and_stop(name, frame);
            marker_frames_[m.id] = frame;
        }
        const glm::vec2 at = (m.at - p.offset()) / p.scale() * 20.0f;
        c->matrix = swf::Matrix{};
        if (!m.on_screen) c->matrix.set_scale_rotation(1, 1, glm::radians(m.angle));
        c->matrix.tx = at.x, c->matrix.ty = at.y;
        c->accept_moves = false;
        c->visible = true;
        char text[32];
        std::snprintf(text, sizeof text, "%dm", int(m.distance + 0.5f));
        p.set_text(name + ".DistanceToObjectiveText.tf", text);
    }
}

void FlashHud::cinematic(bool on) {
    swf::Player& p = *screen_.player();
    cinematic_ = on;
    // On the HUD frame startCinematic leaves the controls up (its hideEverything is for frame
    // 1); in the footage a cinematic shows only the bars, the subtitle and the skip arrow.
    for (const char* name : kLeftGroup) p.set_visible(name, !on);
    for (const auto& [slot, last] : buttons_) p.set_visible(slot, !on);
    p.set_visible("gadgetWheel", !on);
    p.set_visible("bSkipCutscene", on);
    if (on) {
        p.set_visible("CinematicHUDInstance", true);
        p.invoke("CinematicHUDInstance", "startCinematic");
    } else {
        p.invoke("CinematicHUDInstance", "endCinematic");
        hide_unused();
    }
    const std::string text = dialog_;
    dialog_.clear();
    dialog(text);   // the line moves between the subtitle and the dialog box
}

void FlashHud::dialog(const std::string& text) {
    swf::Player& p = *screen_.player();
    if (cinematic_) {
        // mCinematicHUD.Update shows _global.cinematicInternalText every frame.
        p.set_member("_global", "cinematicInternalText", swf::Value(text));
    } else if (text.empty()) {
        if (!dialog_.empty()) p.invoke("Dialog", "closeDialog");
    } else {
        p.set_visible("Dialog", true);
        p.invoke("Dialog", "setTextDialog", {swf::Value(text)});
        if (dialog_.empty()) p.invoke("Dialog", "openDialog");
    }
    dialog_ = text;
}

void FlashHud::run(float seconds) {
    if (swf::Player* p = screen_.player())
        for (float t = 0; t < seconds; t += 1.0f / 30) p->advance(1.0f / 30);
}

// CHUDDisplay::ShowObjectiveMessage / ClearObjectiveMessage.
void FlashHud::objective(bool shown, const std::string& text) {
    swf::Player* p = screen_.player();
    if (!p) return;
    p->set_visible("objectiveMessage_popin", true);
    if (shown) {
        p->goto_and_play("objectiveMessage_popin", "show");
        p->invoke("objectiveMessage_popin.tObjectiveText", "setText", {swf::Value(text)});
    } else {
        p->goto_and_play("objectiveMessage_popin", "hide");
    }
}

void FlashHud::button(const std::string& slot, const std::string& style, bool enabled) {
    swf::Player* p = screen_.player();
    if (!p) return;
    auto& last = buttons_[slot];
    if (last.first == style && last.second == int(enabled)) return;
    const std::string path = slot + ".button";
    if (last.first != style) p->invoke(path, "switchStyle", {swf::Value(style)});
    if (last.second != int(enabled)) p->invoke(path, enabled ? "enableButton" : "disableButton");
    last = {style, int(enabled)};
}

void FlashHud::draw(ParticleList& out) {
    if (state_.visible) screen_.draw(out);
}

}  // namespace tdkr
