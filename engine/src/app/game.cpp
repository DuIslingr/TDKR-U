#include "app/game.h"

#include <cmath>

#include "swf/steps.h"

#include <cstdio>
#include <stdexcept>

#include <SDL3/SDL.h>

namespace tdkr {

namespace {

// A viewpoint looking at the Nth placement whose object type contains `spec` (NAME[#N]),
// from whichever side has the most open space. Returns false if there is no such object.
bool focus_camera(const Scene& scene, const Gol& gol, const std::string& spec, glm::vec3& eye, float& yaw, float& pitch) {
    const size_t hash = spec.find('#');
    const std::string name = to_lower(spec.substr(0, hash));
    int index = hash == std::string::npos ? 0 : std::stoi(spec.substr(hash + 1));
    for (const Placement& p : model_placements(scene.level(), gol)) {
        if (to_lower(p.object).find(name) == std::string::npos || index-- > 0) continue;
        const glm::vec3 target = p.transform.position + glm::vec3(0, 0, 1.0f);
        constexpr float kWant = 4.0f;
        float best = -1;
        for (int i = 0; i < 16; ++i) {
            // Start from the object's facing (its local +Y) and go round.
            const float a = glm::radians(p.transform.rotation.z + i * 22.5f);
            const glm::vec3 dir(-std::sin(a), std::cos(a), 0.3f);
            float t = 1;
            scene.collision().raycast(target, target + dir * kWant, t);
            const float clear = t * kWant - 0.3f;
            if (clear > best + 0.25f) {
                best = clear;
                eye = target + dir * std::max(clear, 0.5f);
            }
        }
        const glm::vec3 d = target - eye;
        yaw = glm::degrees(std::atan2(-d.x, d.y));
        pitch = glm::degrees(std::asin(d.z / glm::length(d)));
        std::printf("focus: %s (%s, anim %s:%s) at (%.2f %.2f %.2f), eye %.2f,%.2f,%.2f,%.1f,%.1f\n", p.object.c_str(), p.model.c_str(), p.anim_file.c_str(), p.anim_clip.c_str(),
                    p.transform.position.x, p.transform.position.y, p.transform.position.z, eye.x, eye.y, eye.z, yaw,
                    pitch);
        return true;
    }
    return false;
}

}  // namespace

Game::Game(const Options& options, const Vfs& vfs_, const Gol& gol_, SDL_Window* window_, const std::string& shader_dir)
    : opt(options),
      vfs(vfs_),
      gol(gol_),
      window(window_),
      headless(!options.screenshot.empty()),
      renderer(window_, shader_dir),
      scene(vfs_, renderer, gol_),
      campaign(campaign_levels(vfs_, gol_)),
      loading_hud(scene),
      loading_title(vfs_, renderer, "Days.ttf", std::max(14.0f, float(options.height) * 0.03f)),
      loading_text(vfs_, renderer, "Days.ttf", std::max(12.0f, float(options.height) * 0.022f)),
      loading_fonts_for(options.height),
      clock(options.time),
      logic(scene, player, gol_),
      combat(scene, player, logic, gol_),
      viewmodel(scene, gol_),
      subtitles(vfs_, renderer, "Days.ttf", std::max(16.0f, float(options.height) * 0.032f)),
      hud(scene),
      flash_hud(vfs_, renderer, &main_strings),
      missions(load_missions(vfs_, gol_)),
      front_end(vfs_, renderer, &main_strings, &missions),
      touch(hud),
      settings_path(headless ? std::string() : Settings::default_path()),
      settings(Settings::load(settings_path)),
      // The save file (Application::Save): written while playing the furthest chapter
      // reached (replaying an earlier one saves nothing, as the original's NoSave).
      save_path(!options.save_file.empty() ? options.save_file : headless ? std::string() : SaveGame::default_path()),
      flares(scene, logic),
      menu_title(vfs_, renderer, "Days.ttf", std::max(20.0f, float(options.height) * 0.06f)),
      objective_text(vfs_, renderer, "Days.ttf", std::max(13.0f, float(options.height) * 0.026f)),
      dialogue(vfs_, renderer, "Days.ttf", std::max(13.0f, float(options.height) * 0.024f)),
      fonts_for(options.height) {
    main_strings.load(vfs, "MAIN");
    loading.touch = opt.touch;
    // The main menu first, unless a level was asked for.
    main_menu.campaign = &campaign;
    main_menu.text = [this](const std::string& id) {
        const std::string t = main_strings.find(id);
        return t.empty() ? id : t;
    };
    flash_menus = vfs.exists("MainMenu.swf") && vfs.exists("MenuPressKey.swf");
    logic.gamepad = opt.gamepad;
    front_end.play_sound = [this](const std::string& s) {
        if (logic.audio) logic.audio->play(s);
    };
    if (opt.list_missions)
        for (size_t i = 0; i < missions.size(); ++i)
            std::printf("mission %2zu  chapter %d.%d  %-40s %s\n", i, missions[i].chapter, missions[i].number,
                        missions[i].name.c_str(), missions[i].level.c_str());
    if (!opt.mission.empty()) {   // a mission's level; its start script runs once the level has started
        const bool number = opt.mission.find_first_not_of("0123456789") == std::string::npos;
        for (size_t i = 0; i < missions.size() && mission < 0; ++i)
            if (number ? int(i) == std::stoi(opt.mission) : to_lower(missions[i].name).find(to_lower(opt.mission)) != std::string::npos)
                mission = int(i);
        if (mission < 0) throw std::runtime_error("no mission " + opt.mission);
        opt.level = missions[size_t(mission)].level;
    }
    if (!headless && !opt.level_given) {
        if (flash_menus && front_end.show(FrontEnd::Screen::Title)) main_menu.open = true;
        else main_menu.show();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        SDL_PumpEvents();
        renderer.render_to_window({}, draw_main_menu(w, h));
    } else if (!headless) {
        show_loading(opt.level);
    }
    scene.load_level(opt.level, 1u << (std::max(opt.section, 1) - 1));
    loading.loaded = true;
    if (opt.builtin_shaders) scene.set_game_shaders(false);
    scene.zones().enabled = opt.zones;
    if (opt.skybox >= 0 && !scene.set_skybox(opt.skybox)) throw std::runtime_error("no skybox with that id");
    const SceneStats& st = scene.stats();
    const MaterialStats& ms = scene.material_stats();
    std::printf("materials: %zu with game shaders, %zu fallback; %zu programs (%zu failed), %zu effects missing; "
                "%zu shader stages compiled, %zu from cache\n",
                ms.materials, ms.fallback, ms.programs, ms.failed_programs, ms.missing_effects,
                scene.shader_compiler().compiled, scene.shader_compiler().cache_hits);
    std::printf("%s: %zu placements, %zu models (%zu missing), %zu textures (%zu missing), %zu draws, "
                "%zu collision objects (%zu triangles, %zu missing), %zu animated (%zu animations missing), "
                "%zu draws hidden by LOD, %zu irradiance volumes, %zu lights, %zu skyboxes, %zu Beast lightmapped, loaded in %.2fs\n",
                opt.level.c_str(), st.placements, st.models, st.missing_models, st.textures,
                st.missing_textures, st.draws, st.collision_objects, st.collision_triangles,
                st.missing_collision, st.animated, st.missing_animations, st.lod_hidden, st.irradiance_volumes, st.lights, st.skyboxes,
                st.beast_lightmapped, st.load_seconds);
    player.spawn(scene.start_position, scene.start_yaw);
    if (opt.touch) logic.input = InputScheme::Touch;
    if ((!headless && !opt.mute) || !opt.sound_render.empty()) {
        audio = std::make_unique<Audio>(vfs, opt.sound_render.empty());
        if (audio->enabled()) logic.audio = audio.get();
        audio->trace = !opt.sound_render.empty();
        scene.particles().on_sound = [this](const std::vector<std::string>& names, const glm::vec3& at) {
            if (!logic.audio) return;
            Audio::Params p;
            p.position = &at;
            logic.audio->play_one_of(names, p);
        };
    }
    if (opt.scripts) post_params = &logic.post, zfar = &logic.zfar;
    logic.auto_qte = headless;   // nobody to press the QTE buttons
    logic.trace = opt.trace_scripts;
    logic.skip_intro = opt.skip_intro;
    logic.log = [this](const std::string& line) {
        if (line.rfind("print: ", 0) == 0 && !opt.trace_scripts) return;   // scripts print a lot
        if (opt.trace_scripts) std::printf("[script %7.2f] %s\n", logic.play_time, line.c_str());   // game seconds into the level
        else std::printf("[script] %s\n", line.c_str());
    };
    std::printf("zones: %zu zones, %zu portals\n", scene.zones().zones().size(), scene.zones().portals().size());
    combat.log = [](const std::string& line) { std::printf("[combat] %s\n", line.c_str()); };
    combat.player_invulnerable = opt.god;
    combat.death_screen = !headless;
    logic.combat = &combat;
    if (opt.credits >= 0) settings.credits = opt.credits;
    if (opt.difficulty >= 0) settings.difficulty = opt.difficulty;
    shop.load(gol);
    combat.set_difficulty(settings.difficulty), shop.difficulty = settings.difficulty;
    logic.on_checkpoint = [this] {
        if (!saving(opt.level)) return;
        SaveGame g;
        g.level = opt.level, g.checkpoint = logic.checkpoint_data();
        if (!g.write(save_path)) std::fprintf(stderr, "could not write %s\n", save_path.c_str());
    };
    if (!shop.items().empty()) menu.shop = &shop, menu.combat = &combat;
    shop.audio = logic.audio;
    end_screen.text = [this](const std::string& id) {
        const std::string t = main_strings.find(id);
        return t.empty() ? id : t;
    };
    death.text = [this](const std::string& id) {
        const std::string t = main_strings.find(id);
        return t.empty() ? id : t;
    };
    menu.text = [this](const std::string& id) {
        const std::string t = logic.strings.find(id);
        return t.empty() ? id : t;
    };
    menu.touch = logic.input == InputScheme::Touch;
    if (opt.scripts) {
        logic.start();
        brawl.reset();
        brawl.auto_fight = opt.fight_test;
        const auto& ls = logic.stats();
        std::printf("scripts: %zu chunks (%zu with errors), %zu objects, %zu triggers, %zu doors, %zu usable\n",
                    ls.chunks, ls.chunk_errors, ls.objects, ls.triggers, ls.doors, ls.interactive);
        combat.start();
        combat.campaign_loadout(opt.level);
        shop.apply_owned(combat, settings);
        viewmodel.start();
        hud.start();
        flares.start();
        if (opt.list_actors)
            for (const std::string& line : combat.describe()) std::printf("  %s\n", line.c_str());
    }
    if (!opt.spawn_at.empty()) {
        float v[4] = {0, 0, 0, 0};
        if (std::sscanf(opt.spawn_at.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) >= 3) {
            player.spawn({v[0], v[1], v[2]}, v[3]);
        } else {
            const LevelLogic::Object* o = logic.object(std::stoi(opt.spawn_at));
            if (!o) throw std::runtime_error("no object with id " + opt.spawn_at);
            player.spawn(o->position, o->yaw);
        }
    }
    hero.start(scene, player);
    logic.gamepad = opt.gamepad || pad != nullptr;
    if (mission >= 0 && opt.scripts && !missions[size_t(mission)].script.empty()) {
        std::printf("mission %d: %s\n", mission, missions[size_t(mission)].name.c_str());
        logic.exec(missions[size_t(mission)].script);
    }
    for (const std::string& code : opt.exec) logic.exec(code);
    for (const std::string& h : opt.hide_models) std::printf("hid %zu placements of *%s*\n", scene.hide_models(h), h.c_str());
    if (!opt.continue_file.empty() && opt.scripts) {
        SaveGame g;
        if (!SaveGame::read(opt.continue_file, g)) throw std::runtime_error("cannot read the save " + opt.continue_file);
        resume(g);
    }
}

Game::~Game() = default;

// The level after `level` in the campaign's story order (gol "GameLevels"); empty after the last.
std::string Game::next_level(const std::string& level) const {
    const size_t slash = level.find_last_of('/');
    std::string base = level.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".lvc") == 0) base.resize(base.size() - 4);
    for (size_t i = 0; i + 1 < campaign.size(); ++i)
        if (base == campaign[i].file) return level.substr(0, slash == std::string::npos ? 0 : slash + 1) + campaign[i + 1].file + ".lvc";
    return {};
}

// A screen-filling quad in the overlay (fades).
void Game::fullscreen(Overlay& o, int w, int h, const glm::vec4& c) {
    const uint32_t rgba = pack_rgba(c);
    const ColorVertex v[4] = {{{0, 0, 0}, rgba}, {{float(w), 0, 0}, rgba}, {{float(w), float(h), 0}, rgba}, {{0, float(h), 0}, rgba}};
    o.screen.insert(o.screen.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

const CampaignLevel* Game::campaign_level(const std::string& level) const {
    for (const CampaignLevel& c : campaign)
        if (level.find("/" + c.file + ".lvc") != std::string::npos || level == c.file + ".lvc") return &c;
    return nullptr;
}

FrameParams Game::draw_loading(int w, int h) {
    loading_hud.set_screen(w, h);
    if (h != loading_fonts_for) {
        loading_fonts_for = h;
        loading_title.set_size(std::max(14.0f, float(h) * 0.03f));
        loading_text.set_size(std::max(12.0f, float(h) * 0.022f));
    }
    loading_overlay.clear();
    loading_sprites.vertices.clear();
    loading_sprites.batches.clear();
    loading.draw(loading_overlay, loading_sprites, loading_hud, loading_title, loading_text, main_strings, w, h);
    FrameParams f;
    f.overlay = &loading_overlay;
    f.sprites = &loading_sprites;
    return f;
}

// A frame of it before a level is read (the window shows nothing new until then).
void Game::show_loading(const std::string& level, bool section) {
    loading.begin(section ? nullptr : campaign_level(level));
    if (!window) return;
    SDL_PumpEvents();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    renderer.render_to_window({}, draw_loading(w, h));
}

// The Flash menus' step: real time (the game may be paused behind them).
float Game::menu_dt() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = menu_clock.time_since_epoch().count() == 0 ? 0.0f : std::min(std::chrono::duration<float>(now - menu_clock).count(), 0.25f);
    menu_clock = now;
    return dt;
}

void Game::show_main_menu() {
    if (flash_menus && front_end.show(FrontEnd::Screen::Main)) main_menu.open = true;
    else main_menu.show();
}

// A mission from the menus: its level, then its start script (TeleportPlayerToZone, quests).
void Game::start_mission(int index) {
    if (index < 0 || index >= int(missions.size())) return;
    const Mission& m = missions[size_t(index)];
    logic.quests.carry_over = false;   // a mission starts the story afresh
    std::fprintf(stderr, "mission %d: %s in %s\n", index, m.name.c_str(), m.level.c_str());
    mission = index;
    load_level(m.level);
    if (opt.scripts && !m.script.empty()) logic.exec(m.script);
}

// The main menu's backdrop: the menu level (l_menu: the bat symbol shattering over the
// city) seen through its animated camera (mm_city_camera.bdae's Camera001), as the original.
bool Game::menu_scene_camera(Camera& cam) {
    if (menu_camera_level != opt.level) {
        menu_camera_level = opt.level;
        menu_camera_object = -1;
        Scene::CameraView v;
        for (size_t i = 0; i < scene.level().objects.size() && menu_camera_object < 0; ++i)
            if (scene.camera_view(i, v)) menu_camera_object = int(i);
    }
    Scene::CameraView cv;
    if (menu_camera_object < 0 || !scene.camera_view(size_t(menu_camera_object), cv)) return false;
    cam.position = cv.position, cam.directed = true, cam.direction = cv.forward, cam.up = cv.up;
    cam.fov = glm::degrees(cv.fov), cam.game_fov = false;
    return true;
}

FrameParams Game::draw_main_menu(int w, int h) {
    if (flash_menus && front_end.open()) {
        const float dt = menu_dt();
        loading_overlay.clear();
        loading_sprites.vertices.clear();
        loading_sprites.batches.clear();
        FrameParams f;
        menu_scene_shown = false;
        if (front_end.screen() == FrontEnd::Screen::Main && to_lower(opt.level).find("l_menu") != std::string::npos) {
            scene.update(dt, menu_eye);
            Camera cam;
            if (menu_scene_camera(cam)) {
                menu_eye = cam.position;
                f = frame(cam, w, h);
                scene.cull(f.camera, f.proj * f.view, ZoneSystem::kNone);
                menu_scene_shown = true;
            }
        }
        front_end.update(dt, w, h);
        front_end.draw(loading_sprites);
        f.overlay = &loading_overlay;
        f.sprites = &loading_sprites;
        return f;
    }
    loading_hud.set_screen(w, h);
    if (h != loading_fonts_for) {
        loading_fonts_for = h;
        loading_title.set_size(std::max(14.0f, float(h) * 0.03f));
        loading_text.set_size(std::max(12.0f, float(h) * 0.022f));
    }
    loading_overlay.clear();
    loading_sprites.vertices.clear();
    loading_sprites.batches.clear();
    main_menu.draw(loading_overlay, loading_sprites, loading_hud, loading_title, loading_text, w, h);
    FrameParams f;
    f.overlay = &loading_overlay;
    f.sprites = &loading_sprites;
    return f;
}

FrameParams Game::frame(const Camera& cam_in, int w, int h) {
    Camera cam = cam_in;
    if (zfar) cam.zfar = *zfar;
    FrameParams f = cam.frame(w, h);
    scene.build_particles(f.camera, particle_list);
    if (opt.scripts) combat.build_beams(f.camera, particle_list);
    f.particles = &particle_list;
    f.fog_color = scene.fog_color;
    f.fog_start = scene.fog_start;
    f.fog_end = scene.fog_end;
    f.fog_density = scene.fog_density;
    f.time = clock;
    f.lights = scene.lights();
    f.post = post_params;
    return f;
}

int Game::chapter_of(const std::string& level) const {
    for (size_t c = 0; c < campaign.size(); ++c)
        if (level.find("/" + campaign[c].file + ".lvc") != std::string::npos) return int(c);
    return -1;
}

bool Game::saving(const std::string& level) const {
    const int c = chapter_of(level);
    return !save_path.empty() && c >= 0 && (c + 1 >= settings.chapters || !opt.save_file.empty());
}

EndLevelScreen::Stats Game::end_stats() {
    const Combat::Stats& cs = combat.stats();
    EndLevelScreen::Stats st;
    st.headshots = int(cs.headshot_kills), st.blast_kills = int(cs.repel_kills), st.paralyze_kills = int(cs.paralyze_kills);
    st.slow_kills = int(cs.slow_kills), st.kills = int(cs.kills), st.retries = int(cs.deaths);
    st.criticals = int(cs.criticals);
    st.accuracy = cs.shots ? std::min(float(cs.hits) / float(cs.shots), 1.0f) : 1.0f;
    st.play_time = cs.play_time;
    return st;
}

EndLevelScreen::Rules Game::end_rules() {
    return EndLevelScreen::read_rules(gol, logic.play_time_limit(0), logic.play_time_limit(1));
}

// Settings that act on the game: sound levels (the menu's Sound page).
void Game::apply_sound() {
    if (!logic.audio) return;
    logic.audio->master = settings.sound ? 0.8f : 0.0f;
    logic.audio->sfx = logic.audio->voices = settings.sfx_volume;
    logic.audio->music = 0.6f * settings.music_volume;
}

// HUD_StartFrost / HUD_StopFrost: the frost spreads or melts over time; and the hits' HUD effects.
void Game::update_frost(float dt) {
    if (logic.frost.serial != frost_serial) {
        frost_serial = logic.frost.serial;
        if (logic.frost.on > 0) hud.start_frost(logic.frost.ms);
        else hud.stop_frost(logic.frost.ms);
    }
    hud.frost_max = logic.frost.max;
    hud.update_frost(dt);
    // The shield's ripple and the blood (CLevel::StartHudEffect, CHud::ShowBlood).
    if (combat.take_hud_damage()) hud.damage_effect();
    if (const float blood = combat.take_hud_blood(); blood >= 0) hud.show_blood(blood);
    if (combat.take_blood_fade()) hud.fade_blood();
    hud.update_effects(dt);
}

void Game::draw_text(FrameParams& f, int w, int h) {
    hud.set_screen(w, h);   // which sprite sheets (1x or 2x) suit this screen
    if (h != fonts_for) {   // resized: the text keeps its share of the screen
        fonts_for = h;
        subtitles.set_size(std::max(16.0f, float(h) * 0.032f));
        menu_title.set_size(std::max(20.0f, float(h) * 0.06f));
        objective_text.set_size(std::max(13.0f, float(h) * 0.026f));
        dialogue.set_size(std::max(13.0f, float(h) * 0.024f));
    }
    hud_sprites.vertices.clear();
    hud_sprites.batches.clear();
    if (opt.scripts) {   // lens flares, under the HUD
        const float flare_dt = flare_clock < 0 ? 10.0f : clock - flare_clock;   // the first frame: settled
        flare_clock = clock;
        flares.frame(flare_dt, f.camera, f.proj * f.view, w, h, hud_sprites);
    }
    // TDKR: the Flash HUD over Batman (the N.O.V.A. 3 HUD below stays off).
    const bool flash = hero.active();
    if (flash) {
        if (flash_hud_level != opt.level) {
            flash_hud_level = opt.level;
            flash_hud.start(scene.minimap, scene.minimap_scale, scene.minimap_zoom);
        }
        if (opt.scripts && logic.win.pending) {   // SetWinScreen: the mission's end
            logic.win.pending = false;
            flash_hud.win_screen(logic.win.title, logic.win.xp, logic.win.coins, float(logic.play_time));
            if (logic.audio) logic.audio->play("m_mission_complete");
        }
        FlashHud::State fs;
        fs.visible = !menu.open && !death.open && !end_screen.open && !storyboard.active();
        fs.cinematic = opt.scripts && logic.player_locked();
        if (opt.scripts) fs.frame = logic.hud_state;
        if (opt.scripts) {
            fs.hack = {logic.hack.active ? logic.hack.serial : 0, logic.hack.grid, logic.hack.size, logic.hack.start_row,
                       logic.hack.end_row, logic.hack.ms, logic.hack.price};
            if (const LevelLogic::Object* q = logic.object(logic.button_qte.object)) {
                const glm::vec4 c = f.proj * f.view * glm::vec4(q->position + glm::vec3(0, 0, 1.0f), 1);
                if (c.w > 1e-3f) {
                    fs.qte = true;
                    fs.qte_at = glm::vec2((c.x / c.w * 0.5f + 0.5f) * float(w), (0.5f - c.y / c.w * 0.5f) * float(h));
                }
            }
        }
        // Spoken lines go to the dialog box; "Progress Saved" is the checkpoint notice.
        if (opt.scripts)
            for (const LevelLogic::Message& m : logic.messages())
                if (m.id != "FPS_GAME_SAVED") {
                    fs.dialog = m.text;
                    break;
                }
        if (opt.scripts && logic.checkpoint() != flash_hud_checkpoint) {
            flash_hud_checkpoint = logic.checkpoint();
            if (flash_hud_checkpoint >= 0) {
                const std::string t = logic.strings.find("FPS_GAME_SAVED");
                flash_hud.checkpoint_message(t.empty() ? "Progress Saved" : t);
            }
        }
        fs.position = glm::vec2(player.center);
        fs.facing = hero.facing();
        using HM = FlashHud::State::Move;
        fs.move = player.plunging ? HM::Plunge : player.gliding ? HM::Glide : player.on_ground ? HM::Ground : HM::Air;
        if (opt.scripts) {
            fs.attack = brawl.can_take_down() ? "TAKEDOWN" : "STRIKE";
            fs.counter = logic.controls.counter && brawl.counter_open();
            fs.jump_on = logic.controls.jump;
            fs.attack_on = logic.controls.combat || brawl.can_take_down();
            fs.grapnel_on = logic.controls.grapnel;
            fs.riding = logic.riding >= 0;
        }
        {
            const glm::vec3 fwd = glm::vec3(glm::inverse(f.view) * glm::vec4(0, 0, -1, 0));
            fs.camera_yaw = glm::degrees(std::atan2(-fwd.x, fwd.y));
        }
        glm::vec3 point, landing;
        const glm::vec3 forward = glm::normalize(glm::vec3(glm::inverse(f.view) * glm::vec4(0, 0, -1, 0)));
        // A target the scripts locked the grapnel onto is marked wherever the view aims.
        glm::vec3 lock;
        const bool locked = opt.scripts && logic.grapple_lock_at(lock) && logic.controls.grapnel;
        if (locked) point = lock + glm::vec3(0, 0, 0.5f);
        if (locked || hero.grapple_target(player, f.camera, forward, point, landing)) {
            const glm::vec4 c = f.proj * f.view * glm::vec4(point, 1);
            if (c.w > 1e-3f) {
                fs.grapple = true;
                fs.grapple_at = glm::vec2((c.x / c.w * 0.5f + 0.5f) * float(w), (0.5f - c.y / c.w * 0.5f) * float(h));
            }
        }
        // Objective markers above their objects (CHUDDisplay::UpdateObjectivesPosition); off
        // screen (or behind), pinned to the screen's edge, the arrow pointing their way.
        if (opt.scripts)
            for (const LevelLogic::ObjectiveMarker& om : logic.objective_markers) {
                const LevelLogic::Object* o = logic.object(om.id);
                if (!o || o->dead || !o->visible) continue;
                const glm::vec3 at = o->position + glm::vec3(0, 0, 1.9f);   // over the head (an actor is 1.8 m tall)
                FlashHud::State::Marker m;
                m.id = om.id, m.kind = om.text;
                m.distance = glm::distance(at, player.center);
                const glm::vec4 c = f.proj * f.view * glm::vec4(at, 1);
                const glm::vec2 half(float(w) * 0.5f, float(h) * 0.5f);
                glm::vec2 ndc = c.w > 1e-3f ? glm::vec2(c.x, c.y) / c.w : glm::vec2(c.x, c.y) * -1.0f;
                m.on_screen = c.w > 1e-3f && std::abs(ndc.x) < 0.92f && std::abs(ndc.y) < 0.88f;
                if (!m.on_screen) {
                    if (glm::length(ndc) < 1e-4f) ndc = glm::vec2(0, -1);
                    ndc /= std::max(std::abs(ndc.x) / 0.92f, std::abs(ndc.y) / 0.88f);   // onto the edge
                    m.angle = glm::degrees(std::atan2(-ndc.y * half.y, ndc.x * half.x));
                }
                m.at = glm::vec2((ndc.x * 0.5f + 0.5f) * float(w), (0.5f - ndc.y * 0.5f) * float(h));
                fs.markers.push_back(m);
            }
        const float hud_dt = flash_hud_clock < 0 ? 0.0f : std::clamp(clock - flash_hud_clock, 0.0f, 0.25f);
        flash_hud_clock = clock;
        flash_hud.update(hud_dt, fs, w, h);
        if (const int r = flash_hud.take_hack_result(); r && opt.scripts) logic.end_hack(r == 1);
        if (logic.hud_objective.serial != flash_hud_objective) {
            flash_hud_objective = logic.hud_objective.serial;
            flash_hud.objective(logic.hud_objective.shown, logic.hud_objective.text);
        }
        if (opt.swf.empty()) flash_hud.draw(hud_sprites);
        if (storyboard.active()) storyboard.draw(hud_sprites, scene, renderer.white(), w, h);
    }
    if (!opt.swf.empty()) {   // --swf: the movie over everything
        if (!swf_view) start_swf_view(w, h);
        if (swf_view) {
            const float dt = swf_clock < 0 ? 0.0f : std::clamp(clock - swf_clock, 0.0f, 0.25f);
            swf_clock = clock;
            swf_view->update(dt, w, h);
            swf_view->draw(hud_sprites);
        }
    }
    if (opt.scripts && !flash) {
        Hud::State hs;
        hs.visible = !combat.player_dead && logic.hud_enabled && (!logic.player_locked() || logic.interface_in_cinematic) && !menu.open;
        hs.clip = combat.ammo_in_clip(), hs.reserve = combat.ammo_reserve();
        hs.grenades = (combat.grenades_enabled || combat.mines_picked()) && logic.controls.items && !logic.limited_active();
        hs.weapons = logic.controls.weapons && !logic.limited_active();
        hs.grenade_count = combat.mines_picked() ? combat.mines : combat.grenades;
        hs.shield = combat.player_health / std::max(combat.player_max_health, 1.0f);
        hs.core = combat.core;
        if (combat.core >= 0) hs.core_left = (hs.shield - 1) / std::max(combat.core_percent / 100, 1e-3f);
        hs.weapon_sprite = combat.weapon_sprite();
        hs.clip_size = combat.clip_size();
        hud.frame(f, w, h, hs, hud_sprites);
        if (float angle = 0; hs.visible && combat.hit_indicator(angle))   // CLevel::Render2DInterface
            if (const Sprite* cs = hud.sprite("controls.bsprite"))
                hud.draw_frame_rotated(hud_sprites, *cs, 60, {float(w) * 0.5f, float(h) * 0.5f}, hud.sprite_scale("controls.bsprite"),
                                       glm::radians(angle), glm::vec4(1));
        const std::string& qte = logic.qte_kind();
        if (logic.input == InputScheme::Touch && (hs.visible || !qte.empty())) {
            const LevelLogic::Controls& lc = logic.controls;
            touch.shown = {lc.move, lc.move && lc.rush, lc.shoot && combat.shoot_enabled, lc.aim, lc.jump, lc.powers};
            if (!hs.visible) touch.shown = {false, false, qte == "fire", false, false, qte == "repel"};   // a QTE in a cinematic
            touch.layout(w, h);
            touch.power_ready = combat.power_ready();
            touch.power = combat.power();
            touch.draw(hud_sprites);
        } else {
            touch.release_all();
        }
    }
    if (opt.scripts && !logic.qte_kind().empty()) {
        // CQTEFire: tutorial.bsprite's animation 1 over the button to press.
        if (const Sprite* ts = hud.sprite("tutorial.bsprite"); ts) {
            const float scale = hud.sprite_scale("tutorial.bsprite");
            const bool touch_qte = logic.input == InputScheme::Touch;
            const glm::vec2 at = touch_qte ? touch.fire_anchor() : glm::vec2(float(w) * 0.5f, float(h) * 0.72f);
            hud.draw_anim(hud_sprites, *ts, 1, at, scale, logic.qte_time(), glm::vec4(1));
            if (!touch_qte) {
                const std::string key = logic.qte_kind() == "fire" ? "[Left mouse]" : logic.qte_kind() == "repel" ? "[Q]" : "[W][A][S][D]";
                objective_text.draw(hud_sprites, key, at + glm::vec2(0, 70 * scale), w, glm::vec4(1));
            }
        }
    }
    if (opt.scripts && !logic.player_locked() && logic.hud_enabled)
        for (const LevelLogic::ObjectiveMarker& m : logic.objective_markers) {
            const LevelLogic::Object* o = logic.object(m.id);
            if (!o || !o->enabled) continue;
            glm::vec3 at = o->position;
            if (!m.node.empty()) scene.node_position(o->index, m.node, at);
            hud.draw_marker(hud_sprites, objective_text, f.proj * f.view, at, glm::distance(f.camera, at), w, h, settings.metric);
        }
    if (opt.scripts && logic.path_marker.on && !logic.player_locked() && logic.hud_enabled)
        hud.draw_marker(hud_sprites, objective_text, f.proj * f.view, logic.path_marker.position,
                        glm::distance(f.camera, logic.path_marker.position), w, h, settings.metric);
    if (opt.scripts && !logic.player_locked() && logic.hud_enabled)
        for (const glm::vec3& g : combat.grenade_threats()) hud.draw_grenade_warning(hud_sprites, f.view, g, w, h);
    if (opt.scripts && logic.timer >= 0 && !logic.player_locked() && logic.hud_enabled)
        hud.draw_timer(hud_sprites, objective_text, logic.timer, w, h);
    if (const Combat::PickupInfo& pi = combat.pickup_info(); pi.time > 0 && opt.scripts && !logic.player_locked() && logic.hud_enabled)
        hud.draw_pickup(hud_sprites, objective_text, pi.sprite, pi.amount, w, h);
    if (opt.scripts && logic.objectives.alpha > 0 && !logic.player_locked() && !menu.open) {
        std::vector<Hud::ObjectiveLine> lines;
        if (!logic.objectives.primary.empty()) lines.push_back({logic.objectives.primary, true, logic.objectives.primary_done});
        for (const auto& s : logic.objectives.secondary) lines.push_back({s.text, false, s.done});
        hud.draw_objectives(hud_sprites, objective_text, lines, logic.objectives.alpha, w, h);
    }
    if (opt.scripts && !logic.messages().empty() && !menu.open) {
        const LevelLogic::Message& m = logic.messages().back();
        const float a = std::clamp(m.seconds_left / 0.4f, 0.0f, 1.0f);
        if (m.text.find('^') != std::string::npos)   // touch prompts: button glyphs inline
            hud.draw_prompt(hud_sprites, subtitles, m.text, {float(w) * 0.5f, float(h) * 0.8f}, {1, 1, 1, a});
        else if (!m.speaker.empty()) {   // dialogue: "Name: line", left-aligned low on the screen
            const std::string name = m.speaker + ": ";
            const float x = float(w) * 0.082f, nw = dialogue.measure(name, w).x;
            const int wrap = int(float(w) * 0.84f - nw);
            const float y = std::min(float(h) * 0.8f, float(h) * 0.97f - dialogue.measure(m.text, wrap).y);
            dialogue.draw(hud_sprites, name, {x, y}, w, {1.0f, 0.62f, 0.15f, a}, {0.0f, 0.0f});
            dialogue.draw(hud_sprites, m.text, {x + nw, y}, wrap, {1, 1, 1, a}, {0.0f, 0.0f});
        } else {   // from 80% down, raised as needed to keep every line on screen
            const int wrap = int(float(w) * 0.7f);
            const float y = std::min(float(h) * 0.8f, float(h) * 0.97f - subtitles.measure(m.text, wrap).y);
            subtitles.draw(hud_sprites, m.text, {float(w) * 0.5f, y}, wrap, {1, 1, 1, a});
        }
    }
    if (death.open) death.draw(overlay, hud_sprites, hud, subtitles, w, h);
    if (end_screen.open) end_screen.draw(overlay, hud_sprites, hud, menu_title, subtitles, w, h);
    if (menu.open && flash_menus && front_end.open()) {   // the Flash pause menu over the HUD
        front_end.update(menu_dt(), w, h);
        front_end.draw(hud_sprites);
    } else if (menu.open) {
        std::vector<Hud::ObjectiveLine> lines;
        if (!logic.objectives.primary.empty()) lines.push_back({logic.objectives.primary, true, logic.objectives.primary_done});
        for (const auto& o : logic.objectives.secondary) lines.push_back({o.text, false, o.done});
        menu.draw(overlay, hud_sprites, hud, subtitles, objective_text, lines, settings, w, h);
    }
    if (!opt.sprite_sheet.empty()) {   // NAME[:first:count]
        std::string name = opt.sprite_sheet;
        int first = 0, count = -1;
        if (const size_t c = name.find(':'); c != std::string::npos) {
            std::sscanf(name.c_str() + c + 1, "%d:%d", &first, &count);
            name.resize(c);
        }
        if (const Sprite* sp = hud.sprite(name)) {
            fullscreen(overlay, w, h, glm::vec4(0.15f, 0.15f, 0.15f, 1));
            const int total = int(sp->frames.size());
            if (count < 0) count = total - first;
            count = std::max(0, std::min(count, total - first));
            const int cols = std::clamp(count, 1, 6);
            const float cell_w = float(w) / cols, cell_h = float(h) / float(std::max((count + cols - 1) / cols, 1));
            for (int j = 0; j < count; ++j) {
                const int i = first + j;
                const glm::vec4 b = Hud::frame_bounds(*sp, i);
                const float fw = std::max(b.z - b.x, 1.0f), fh = std::max(b.w - b.y, 1.0f);
                const float k = std::min((cell_w - 4) / fw, (cell_h - 16) / fh);
                const glm::vec2 cell(float(j % cols) * cell_w, float(j / cols) * cell_h);
                hud.draw_frame(hud_sprites, *sp, i, cell + glm::vec2(2 - b.x * k, 14 - b.y * k), k, glm::vec4(1));
                objective_text.draw(hud_sprites, std::to_string(i), cell, w, glm::vec4(1, 1, 0, 1), {0.0f, 0.0f});
            }
        }
    }
    f.sprites = &hud_sprites;
}

// A level from its start (the next campaign level, Restart Level); the arsenal carries over.
void Game::load_level(const std::string& level) {
    opt.level = level;
    show_loading(level);
    if (window) SDL_SetWindowTitle(window, ("The Dark Knight Rises - " + opt.level).c_str());
    scene.load_level(opt.level, 1);
    loading.loaded = true;
    player.spawn(scene.start_position, scene.start_yaw);
    hero.start(scene, player);
    logic.start();
    brawl.reset();
    combat.start();
    combat.campaign_loadout(opt.level);
    shop.apply_owned(combat, settings);
    viewmodel.start();
    hud.start();
    flares.start();
}

// Continue (Application::Load): the save's level from its checkpoint (as a respawn
// there), or from its start with the arsenal carried in.
void Game::resume(const SaveGame& g) {
    if (!g.checkpoint.empty() && logic.load_checkpoint_data(g.checkpoint)) {
        opt.level = g.level;
        show_loading(g.level);
        if (window) SDL_SetWindowTitle(window, ("The Dark Knight Rises - " + opt.level).c_str());
        scene.load_level(opt.level, logic.checkpoint_section());
        loading.loaded = true;
        player.spawn(scene.start_position, scene.start_yaw);
        hero.start(scene, player);
        logic.respawn_at_checkpoint();
        brawl.reset();
        shop.apply_owned(combat, settings);
        viewmodel.start();
        hud.start();
        flares.start();
    } else {
        load_level(g.level);
        if (g.has_arsenal) combat.restore_arsenal(g.arsenal), shop.apply_owned(combat, settings);
    }
    std::printf("continued %s%s\n", g.level.c_str(), g.checkpoint.empty() ? "" : " from its checkpoint");
}

// Level logic at a fixed step, so triggers and doors see every position.
// A locked target is an edge: the pull ends with the climb over it, a step beyond.
static glm::vec3 past_edge(const glm::vec3& lock, const glm::vec3& from) {
    glm::vec2 d(lock - from);
    if (glm::length(d) > 1e-3f) d = glm::normalize(d);
    return lock + glm::vec3(d * 1.5f, 0.0f);
}

// --auto-play's way points keep to the floor under them (within 1.5 m up, 4 m down), so the
// walk does not pass through the triggers of the floors above and below.
glm::vec3 Game::on_floor(const glm::vec3& p) const {
    float z = 0;
    return scene.collision().ray_down(p + glm::vec3(0, 0, 1.5f), 5.5f, z) ? glm::vec3(p.x, p.y, z) : p;
}

// --auto-play: every few seconds out of a fight, Batman goes to the first objective marker
// (an enemy: next to it, for --fight-test; something usable: there, and uses it; a place: on
// it), or else to the nearest usable thing.
void Game::auto_play(float dt) {
    if (logic.riding >= 0) return;   // on the Batpod: auto_drive() steers
    // On the way: half a metre a step along the line, through whatever is in between (the
    // triggers on the way fire), then the use.
    if (!auto_play_path.empty()) {
        player.center = auto_play_path.front() + glm::vec3(0, 0, player.radius.z);
        auto_play_path.erase(auto_play_path.begin());
        if (auto_play_path.empty() && auto_play_use >= 0) {
            std::printf("[auto-play] use %d\n", auto_play_use);
            logic.use();
            auto_play_use = -1;
            auto_play_wait = 6.0f;   // what it set going (a conversation) plays out first
        }
        return;
    }
    if ((auto_play_wait -= dt) > 0) return;
    auto_play_wait = 3.0f;
    if (!hero.active() || logic.player_locked() || logic.hack.active || logic.button_qte.object >= 0 || brawl.busy() ||
        !logic.attacking_batman.empty() || player.grappling) {
        if ((auto_play_held += 3.0f) >= 30.0f) {   // say why, every half minute
            auto_play_held = 0;
            std::string who;
            for (int id : logic.attacking_batman) who += " " + std::to_string(id);
            std::printf("[auto-play] held: locked %d, hacking %d, qte %d, busy %d, grappling %d, attacked by%s\n", logic.player_locked(),
                        logic.hack.active, logic.button_qte.object >= 0, brawl.busy(), player.grappling, who.empty() ? " none" : who.c_str());
        }
        return;
    }
    auto_play_held = 0;
    // The markers changing: every place is worth a visit again.
    {
        std::vector<int> ids;
        for (const LevelLogic::ObjectiveMarker& m : logic.objective_markers) ids.push_back(m.id);
        if (ids != auto_play_markers) auto_play_markers = ids, auto_play_stale.clear();
    }
    // The markers, then (none left) the nearest usable thing within 40 m.
    std::vector<LevelLogic::ObjectiveMarker> goals = logic.objective_markers;
    if (goals.empty()) {
        float best = 40;
        for (const auto& [id, o] : logic.objects())
            if (o.interactive && o.enabled && o.interact_enabled && !o.dead)
                if (const float d = glm::distance(o.position, player.center); d < best) best = d, goals = {{id, "usable", ""}};
    }
    for (const LevelLogic::ObjectiveMarker& m : goals) {
        LevelLogic::Object* o = logic.object(m.id);
        if (!o || o->dead) continue;   // (a marker on a disabled object is still a place to go)
        if (auto_play_stale[m.id] >= 6 && goals.size() > 1) continue;   // been there, nothing came of it: the next
        const bool use = o->interactive && o->interact_enabled && o->enabled;
        glm::vec2 back(player.center - o->position);
        back = glm::length(back) > 1e-3f ? glm::normalize(back) : glm::vec2(0, -1);
        // Something to break: from 8 m, a batarang.
        if (logic.destroyable(m.id)) {
            // In sight within 25 m: from here. Else a spot 6-14 m away on Batman's own floor (or
            // the light's), with the light in sight from there.
            const CollisionWorld& world = scene.collision();
            const glm::vec3 feet_now = player.center - glm::vec3(0, 0, player.radius.z);
            float t0 = 1;
            const glm::vec3 eye0 = player.eye(), aim0 = o->position + glm::vec3(0, 0, 0.5f);
            const bool in_sight = !world.raycast(eye0, aim0, t0) || (1 - t0) * glm::distance(eye0, aim0) < 1.5f;
            glm::vec3 stand = in_sight && glm::distance(eye0, aim0) < 25.0f ? feet_now : glm::vec3(0);
            float best = in_sight && glm::distance(eye0, aim0) < 25.0f ? 0.0f : 1e9f;
            for (int k = 0; k < 24 && best > 0; ++k)
                for (float r : {6.0f, 10.0f, 14.0f}) {
                    const float a = glm::radians(15.0f * float(k));
                    const glm::vec3 above = glm::vec3(o->position.x, o->position.y, std::max(o->position.z, feet_now.z)) +
                                            glm::vec3(std::cos(a) * r, std::sin(a) * r, 3.0f);
                    float z = 0, t = 1;
                    if (!world.ray_down(above, 12.0f, z) || (std::abs(z - feet_now.z) > 1.0f && std::abs(z - o->position.z) > 1.0f)) continue;
                    // A floor: level all round under it, room to stand above it.
                    bool floor = true;
                    for (const glm::vec2 d : {glm::vec2(0.6f, 0), glm::vec2(-0.6f, 0), glm::vec2(0, 0.6f), glm::vec2(0, -0.6f)}) {
                        float zz = 0;
                        if (!world.ray_down(above + glm::vec3(d, 0.0f), 12.0f, zz) || std::abs(zz - z) > 0.3f) floor = false;
                    }
                    float head = 1;
                    if (!floor || world.raycast(glm::vec3(above.x, above.y, z + 0.3f), glm::vec3(above.x, above.y, z + 2.0f), head)) continue;
                    const glm::vec3 feet(above.x, above.y, z), eye = feet + glm::vec3(0, 0, 1.6f);
                    if (world.raycast(eye, o->position + glm::vec3(0, 0, 0.5f), t) && (1 - t) * glm::distance(eye, o->position) > 1.5f) continue;
                    if (const float d = glm::distance(feet, player.center); d < best) best = d, stand = feet;
                }
            if (best > 1e8f) stand = o->position + glm::vec3(back * 8.0f, 0.0f);
            if (glm::distance(glm::vec2(stand), glm::vec2(player.center)) > 2.0f) {
                std::printf("[auto-play] to %d (break) at (%.1f %.1f %.1f)\n", m.id, stand.x, stand.y, stand.z);
                const glm::vec3 from = player.center - glm::vec3(0, 0, player.radius.z);
                const glm::vec3 dest = stand + glm::vec3(0, 0, 0.1f);
                const int n = int(glm::distance(dest, from) / 0.5f);
                for (int k = 1; k <= n; ++k) auto_play_path.push_back(on_floor(glm::mix(from, dest, float(k) / float(n))));
                return;
            }
            const glm::vec3 eye = player.eye(), dir = glm::normalize(o->position + glm::vec3(0, 0, 0.5f) - eye);
            const bool thrown = brawl.throw_batarang(eye, dir);
            std::printf("[auto-play] batarang at %d: %s\n", m.id, thrown ? "thrown" : "no shot");
            return;
        }
        // A place gone to four times with the markers unchanged: nothing more to come of it.
        if (!use && !logic.is_enemy(m.id) && ++auto_play_stale[m.id] >= 6) std::printf("[auto-play] nothing at %d: on to the next marker\n", m.id);
        glm::vec3 to = o->position + glm::vec3(back * (use ? 0.8f : logic.is_enemy(m.id) ? 2.5f : 0.0f), 1.0f);   // a place: on it
        if (float z = 0; !use && !logic.is_enemy(m.id) && auto_play_stale[m.id] >= 3 &&   // (on it did nothing: the floor
            scene.collision().ray_down(o->position + glm::vec3(back * 1.5f, -0.5f), 10.0f, z))  // below, short of it)
            to = glm::vec3(o->position.x + back.x * 1.5f, o->position.y + back.y * 1.5f, z + 0.1f);
        hero.face(glm::degrees(std::atan2(back.x, -back.y)));
        // At a place that has not gone away: the nearest trigger with an OnEnter around it, not
        // yet walked into (the way there leads through it in the game).
        if (!use && !logic.is_enemy(m.id) && glm::distance(to, player.center) < 2.0f) {
            float best = 15;
            int through = -1;
            for (const auto& [id, t] : logic.objects())
                if (t.trigger >= 0 && t.enabled && !auto_play_entered.count(id) &&
                    logic.runtime().has_function("Obj" + std::to_string(id) + "_OnEnter"))
                    if (const float d = glm::distance(t.position, o->position); d < best) best = d, through = id;
            if (through >= 0) {
                std::printf("[auto-play] through trigger %d\n", through);
                auto_play_entered.insert(through);
                to = logic.object(through)->position + glm::vec3(0, 0, 1.0f);
            } else {
                auto_play_stale[m.id] += 2;
            }
        }
        const glm::vec3 from = player.center - glm::vec3(0, 0, player.radius.z);
        const int n = int(glm::distance(to, from) / 0.5f);
        if (n > 2) std::printf("[auto-play] to %d (%s) at (%.1f %.1f %.1f)\n", m.id, m.text.c_str(), to.x, to.y, to.z);
        for (int k = 1; k <= n; ++k) auto_play_path.push_back(k == n ? to : on_floor(glm::mix(from, to, float(k) / float(n))));
        auto_play_use = use ? m.id : -1;
        if (auto_play_path.empty() && use) {
            std::printf("[auto-play] use %d\n", m.id);
            logic.use();
            auto_play_wait = 6.0f;
        }
        return;
    }
}

// The Batpod (EnterBatPod): the stick drives it, Batman sits on it (his batpod clips), and
// the player, whose position the scripts measure and whose feet the triggers test, rides
// along; the view swings round behind it.
bool Game::ride_step(const PlayerInput& in, float dt) {
    if (!opt.scripts || logic.riding < 0) {
        if (batpod.object() >= 0) batpod.stop(), hero.stop_riding();
        return false;
    }
    if (batpod.object() != logic.riding) batpod.start(logic, logic.riding);
    batpod.update(logic, scene.collision(), opt.auto_play ? auto_drive() : in, dt);
    player.center = batpod.position() + glm::vec3(0, 0, player.radius.z);
    player.on_ground = true, player.vertical_speed = 0, player.gliding = player.plunging = player.grappling = false;
    const float st = batpod.steer();
    const char* clip = std::abs(batpod.speed()) < 1.0f ? "_751_batpod_idle"
                       : st > 0.4f                     ? "_754_batpod_leanr_c"
                       : st < -0.4f                    ? "_753_batpod_leanl_c"
                                                       : "_752_batpod_ride";
    hero.ride(scene, batpod.position(), batpod.yaw(), clip);
    player.yaw += std::remainder(batpod.yaw() - player.yaw, 360.0f) * std::min(1.0f, dt * 2.5f);
    cam.yaw = player.yaw;
    return true;
}

PlayerInput Game::auto_drive() {
    PlayerInput in;
    const LevelLogic::Object* t = nullptr;
    for (const LevelLogic::ObjectiveMarker& m : logic.objective_markers)
        if ((t = logic.object(m.id)) && !t->dead && m.id != logic.riding) break;
        else t = nullptr;
    if (!t) return in;
    // A new target: a place is reached along the trail so far (the tailed van drove there).
    if (t->id != ride_target && (logic.walking(t->id) || ride_trail.empty())) ride_trail.clear();
    ride_target = t->id;
    if (ride_trail.empty() || glm::distance(ride_trail.back(), t->position) > 3.0f) ride_trail.push_back(t->position);
    const glm::vec3 bike = batpod.position();
    while (ride_trail.size() > 1 && glm::distance(glm::vec2(ride_trail.front()), glm::vec2(bike)) < 5.0f) ride_trail.pop_front();
    // The furthest of the next points in sight (the van's straight legs cut through corners).
    {
        size_t seen = 0;
        const glm::vec3 eye = bike + glm::vec3(0, 0, 0.8f);
        for (size_t i = 0; i < std::min<size_t>(ride_trail.size(), 30); ++i) {
            float t = 1;
            if (glm::distance(ride_trail[i], bike) < 80.0f && !scene.collision().raycast(eye, ride_trail[i] + glm::vec3(0, 0, 0.8f), t)) seen = i;
        }
        ride_trail.erase(ride_trail.begin(), ride_trail.begin() + long(seen));
    }
    const glm::vec2 to(ride_trail.front() - bike);
    float off = std::remainder(glm::degrees(std::atan2(-to.x, to.y)) - batpod.yaw(), 360.0f);
    // Feelers ahead: of the headings (relative) whose 12 m are clear, the one nearest the wanted.
    {
        const glm::vec3 from = bike + glm::vec3(0, 0, 0.6f);
        float best = 1e9f, pick = off;
        for (float rel : {0.0f, -25.0f, 25.0f, -50.0f, 50.0f, -80.0f, 80.0f}) {
            const float y = glm::radians(batpod.yaw() + std::clamp(off, -60.0f, 60.0f) * (rel == 0 ? 1.0f : 0.0f) + rel);
            float t = 1;
            if (scene.collision().raycast(from, from + glm::vec3(-std::sin(y), std::cos(y), 0) * 12.0f, t)) continue;
            const float d = std::abs(std::remainder(glm::degrees(y) - batpod.yaw() - off, 360.0f));
            if (d < best) best = d, pick = std::remainder(glm::degrees(y) - batpod.yaw(), 360.0f);
        }
        off = pick;
    }
    in.right = std::clamp(-off / 30.0f, -1.0f, 1.0f);
    const float dist = glm::distance(t->position, bike);
    // Tailing: brake in time to stay over 40 m back (the brakes take v^2 / 2a).
    const float stopping = batpod.speed() > 0 ? batpod.speed() * batpod.speed() / (2 * (batpod.force + batpod.brake_force) / batpod.mass) : 0.0f;
    if (logic.walking(t->id)) in.forward = dist - stopping < 45.0f ? -1.0f : dist - stopping < 55.0f ? 0.0f : std::abs(off) > 45.0f ? 0.6f : 1.0f;
    else in.forward = dist < 6.0f ? -1.0f : std::abs(off) > 45.0f ? 0.6f : 1.0f;
    // Stuck against something: back up a second, turned the other way.
    // Three tries without getting anywhere: on to the next point of the trail (a test's shortcut,
    // logged).
    ride_stuck = in.forward > 0 && std::abs(batpod.speed()) < 1.0f ? ride_stuck + kPhysicsStep : 0.0f;
    if (glm::distance(bike, ride_progress) > 8.0f) ride_progress = bike, ride_tries = 0;
    if (ride_stuck > 1.0f) ride_backup = 1.8f, ride_stuck = 0, ++ride_tries;
    if (ride_tries >= 3 && ride_trail.size() > 1) {
        ride_trail.pop_front();
        const glm::vec3 to = ride_trail.front();
        std::printf("[auto-play] the Batpod is stuck at (%.1f %.1f %.1f): on to (%.1f %.1f %.1f)\n", bike.x, bike.y, bike.z, to.x, to.y, to.z);
        batpod.place(logic, to, std::remainder(glm::degrees(std::atan2(-to.x + bike.x, to.y - bike.y)), 360.0f));
        ride_tries = 0, ride_backup = 0, ride_progress = to;
    }
    if (ride_backup > 0) {
        ride_backup -= kPhysicsStep;
        in.forward = -1, in.right = in.right > 0 ? -1.0f : 1.0f;
    }
    return in;
}

// A QTE button (QTE_EnableControl) answered: Batman's grapnel at its object.
bool Game::answer_qte() {
    if (!opt.scripts || logic.button_qte.object < 0) return false;
    const LevelLogic::Object* o = logic.object(logic.button_qte.object);
    if (o && hero.active()) brawl.grapnel_at(o->position);
    return logic.press_button_qte();
}

bool Game::fire_grapnel(const glm::vec3& eye, const glm::vec3& forward) {
    if (answer_qte()) return true;
    if (opt.scripts && logic.riding >= 0) return false;   // not from the Batpod
    if (opt.scripts && hero.active())   // at an enemy: the pull
        if (const int t = brawl.pull_target(eye, forward); t >= 0) return brawl.pull(t);
    glm::vec3 lock;
    if (opt.scripts && logic.grapple_lock_at(lock) && !player.grappling && glm::distance(lock, player.center) < hero.grapple_range * 1.5f) {
        player.grapple_to(past_edge(lock, player.center));
        return true;
    }
    return hero.try_grapple(player, eye, forward);
}

void Game::step_logic(float dt) {
    if (!opt.scripts) return;
    logic.update(dt);
    // RequireLoadLevel: the next level, the quests carried over; in a window, once the win
    // screen asked for just before (the Stock Exchange's end) has been closed.
    if (!logic.level_request.file.empty() && !(window && (logic.win.pending || flash_hud.win_open()))) {
        const LevelLogic::LevelRequest r = logic.level_request;
        std::printf("[script] loading %s (spawn %d)\n", r.file.c_str(), r.spawn);
        logic.quests.carry_over = true;
        load_level(r.file);
        if (const LevelLogic::Object* sp = r.spawn >= 0 ? logic.object(r.spawn) : nullptr) logic.teleport_player(*sp);
        return;
    }
    // StartIntroStory: the storyboard over everything, then its callback.
    if (!logic.story_request.name.empty()) {
        const LevelLogic::StoryRequest r = std::exchange(logic.story_request, {});
        std::printf("[script] storyboard %s\n", r.name.c_str());
        if (storyboard.start(vfs, r.name)) story_callback = r.callback;
        else if (!r.callback.empty()) logic.start_script(r.callback, -1);
    }
    if (storyboard.active() && storyboard.update(dt)) {
        std::printf("[script] storyboard ended\n");
        if (!story_callback.empty()) logic.start_script(std::exchange(story_callback, {}), -1);
    }
    if (hero.active()) player.can_glide = logic.controls.glide;   // SetGlideControlEnable
    if (hero.active()) combat.player_invulnerable = true;   // TDKR: Batman's health is the brawl's, not N.O.V.A. 3's
    if (hero.active()) brawl.update(dt);
    if (!std::isnan(logic.face_request)) {   // ActorLookTo(0, ...)
        hero.face(logic.face_request);
        logic.face_request = NAN;
    }
    if (opt.auto_play) auto_play(dt);
    // --fight-test answers a QTE button after a moment, and wins the hacking game after a few seconds.
    if (opt.fight_test && logic.button_qte.object >= 0 && logic.button_qte.left < 3.5f) answer_qte();
    if (opt.fight_test && logic.hack.active && (fight_test_hack += dt) > 4.0f) fight_test_hack = 0, logic.end_hack(true);
    // --fight-test also fires the grapnel when the scripts have locked it onto a target (an enemy: the pull).
    if (opt.fight_test && hero.active() && logic.grapple_lock >= 0 && logic.controls.grapnel && !logic.player_locked() &&
        player.on_ground && !player.grappling && !brawl.busy()) {
        glm::vec3 lock;
        if (const int t = brawl.pull_target(player.eye(), player.view_forward()); t >= 0) brawl.pull(t);
        else if (logic.grapple_lock_at(lock) && glm::distance(lock, player.center) > 3 && fight_test_lock != logic.grapple_lock)
            player.grapple_to(past_edge(lock, player.center)), fight_test_lock = logic.grapple_lock;   // once per lock
    }
    if (logic.grapple_lock < 0) fight_test_lock = -1;
    if (hero.active()) scene.set_object_visible(hero.object(), !logic.player_hidden);
    if (logic.pending_section) {   // LoadLevelSections: reload, keep the player
        const uint32_t section = logic.pending_section;
        const glm::vec3 feet = player.center - glm::vec3(0, 0, player.radius.z);
        const float yaw = player.yaw;
        std::printf("[script] loading level section mask %u\n", section);
        show_loading(opt.level, true);   // a black screen with the rings; no press awaited
        scene.load_level(opt.level, section);
        loading.active = false;
        player.spawn(feet, yaw);
        hero.start(scene, player);
        logic.start(true);
        combat.start(true);
        viewmodel.start();
        hud.start();
        flares.start();
    }
    if (logic.respawn_pending) {   // the player died: the level again, at the checkpoint
        std::printf("[script] respawning at checkpoint %d\n", logic.checkpoint());
        show_loading(opt.level, true);
        scene.load_level(opt.level, logic.checkpoint_section());
        loading.active = false;
        logic.respawn_at_checkpoint();
        shop.apply_owned(combat, settings);   // what was bought for good stays
        viewmodel.start();
        hud.start();
        flares.start();
    }
    // EndLevel: a moment on the faded screen, then the next campaign level (the
    // same one again after a failure). The arsenal carries over.
    if (!logic.level_ended) {
        ended_for = 0;
    } else if ((ended_for += dt) > 3.0f) {
        // Complete: the end-of-level screen first; its Continue comes back here.
        if (logic.level_success && !headless && !end_screen.open && !end_screen_done) {
            const bool finished = next_level(opt.level).empty();
            end_screen.show(end_stats(), end_rules(), finished);
            if (finished) settings.hard_unlocked = true;   // Hard is offered from now on
            // CUpgradesManager::GiveUpgradePoints, kept with the settings.
            settings.credits = std::max(settings.credits + end_screen.earned().total, 0);
            settings.save(settings_path);
            std::printf("end of level: %d credits (%d in all)\n", end_screen.earned().total, settings.credits);
        }
        if (end_screen.open) return;
        end_screen_done = false;
        const std::string next = logic.level_success ? next_level(opt.level) : opt.level;
        if (next.empty()) {
            SaveGame::remove(save_path);   // Application::DeleteCampaign
            logic.message("The campaign is complete.", 10);
            logic.level_ended = false;
            return;
        }
        std::printf("[script] level over (%s): loading %s\n", logic.level_success ? "complete" : "failed", next.c_str());
        for (size_t c = 0; c < campaign.size(); ++c)   // the next chapter is reached
            if (next.find("/" + campaign[c].file + ".lvc") != std::string::npos && int(c) + 1 > settings.chapters)
                settings.chapters = int(c) + 1, settings.save(settings_path);
        if (logic.level_success && saving(next)) {   // GS_EndLevel: Application::Save(next level)
            SaveGame g;
            g.level = next, g.has_arsenal = true, g.arsenal = combat.save();
            g.write(save_path);
        }
        load_level(next);
    }
}

// StartJump / StartLanding: the weapon's own clips (ironsight ones while aiming).
void Game::fire_step(float dt, const CombatInput& in, const glm::vec3& eye, const glm::vec3& aim, bool aiming) {
    glm::vec3 at;
    combat.set_muzzle(viewmodel.muzzle(at) ? &at : nullptr);
    const size_t shots = combat.stats().shots;
    combat.update(dt, in, eye, aim);
    if (combat.stats().shots != shots) viewmodel.flash(aiming);
}

void Game::movement_states(bool aim) {
    if (player.jumps != view_jumps) view_jumps = player.jumps, viewmodel.movement_state(aim ? "AimJump" : "Jump");
    static const char* const kLanding[2][3] = {{"Landing", "LandingMed", "LandingHi"}, {"LandingAim", "LandingAimMed", "LandingAimHi"}};
    if (const int l = combat.take_landing(); l >= 0) viewmodel.movement_state(kLanding[aim][l]);
}

void Game::arms_distort() {
    const LevelLogic::ArmsDistort& d = logic.arms_distort;
    if (d.serial == distort_serial) return;
    distort_serial = d.serial;
    viewmodel.arms_distort(d.on);
    if (d.on) viewmodel.arms_distort_blend(d.target, d.ms);
}

void Game::start_swf_view(int w, int h) {
    swf_view = std::make_unique<FlashScreen>(vfs, renderer, &main_strings);
    swf_view->on_native = [](const std::string& name, swf::CallArgs& a) {
        std::string args;
        for (const swf::Value& v : a.args) args += (args.empty() ? "" : ", ") + (v.type == swf::Value::String ? v.s : std::string("…"));
        std::fprintf(stderr, "swf native: %s(%s)\n", name.c_str(), args.c_str());
        return swf::Value();
    };
    if (!swf_view->load(opt.swf, w, h)) {
        swf_view.reset();
        return;
    }
    swf_view->update(0, w, h);
    for (const std::string& step : opt.swf_steps) {
        std::string error;
        if (!swf::run_step(*swf_view->player(), step, &error)) std::fprintf(stderr, "%s\n", error.c_str());
    }
}

int Game::run() {
    view_spawns = player.spawns, view_jumps = player.jumps;
    cam.position = opt.have_eye ? glm::vec3(opt.eye) : view_position(scene.start_yaw, opt.pitch);
    cam.yaw = opt.have_eye ? opt.eye.w : scene.start_yaw;
    cam.pitch = opt.pitch;
    if (!opt.focus.empty() && !focus_camera(scene, gol, opt.focus, cam.position, cam.yaw, cam.pitch))
        throw std::runtime_error("no placement matching " + opt.focus);
    for (const std::string& name : opt.effects) {
        const glm::vec3 at = cam.position + cam.forward() * 6.0f;
        if (scene.particles().start(name, glm::translate(glm::mat4(1), at)) < 0)
            std::fprintf(stderr, "no effect named %s\n", name.c_str());
    }
    if (opt.audit > 0) return run_audit();
    if (opt.walk_test > 0) run_walk_test();
    if (headless || opt.walk_test > 0) return opt.screenshot.empty() ? 0 : run_screenshot();
    run_window();
    return 0;
}

}  // namespace tdkr
