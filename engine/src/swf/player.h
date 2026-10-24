// The Flash player: movie clip instances built from the timelines, ActionScript run through
// the VM, pointer input, and the frame drawn as textured triangles in screen pixels for the
// renderer (no GPU code here). The host API follows gameswf's CharacterHandle / RenderFX,
// which TDKR's CHUDDisplay and menus use (find a clip by path, gotoAndPlay, setVisible,
// setMember, invokeMethod).
#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "swf/as.h"
#include "swf/movie.h"
#include "swf/tess.h"

namespace tdkr::swf {

class Player;

// What a triangle samples: nothing (solid colour), an atlas bitmap, a gradient, or a text run.
struct TextureRef {
    enum Kind : uint8_t { White, Bitmap, Gradient, Text } kind = White;
    const MovieDef* movie = nullptr;       // Bitmap: whose <name>_atlas<id>.tga
    uint16_t bitmap = 0;
    const FillStyle* gradient = nullptr;   // Gradient: its stops (u: ratio; radial: uv from the centre)
    int text = -1;                         // Text: DrawList::texts index
    bool smooth = true, repeat = false;
    bool operator==(const TextureRef& o) const {
        return kind == o.kind && movie == o.movie && bitmap == o.bitmap && gradient == o.gradient && text == o.text;
    }
};

struct DrawVertex {
    glm::vec2 pos;          // screen pixels
    glm::vec2 uv;           // Bitmap: bitmap pixels; Gradient: 0..1; Text: 0..1 of the run's box
    glm::vec4 color;        // multiplies the texture
};

// A line of device-font text the renderer rasterises (SDL_ttf) and maps onto its quad.
struct TextRun {
    std::string text;       // UTF-8, '\n' separates lines
    std::string font;       // SWF font name ("Accidental Presidency")
    float size = 12;        // pixels at the run's scale
    glm::vec4 color{1.0f};
    int align = 0;          // 0 left, 1 right, 2 center
    float width = 0, height = 0;   // the box, pixels
    bool wrap = false;
};

struct DrawBatch {
    TextureRef texture;
    std::vector<DrawVertex> vertices;   // triangles
};

// A mask as the convex outline (screen pixels, clockwise on screen) of what it covers.
struct ClipShape {
    std::vector<glm::vec2> pts;
    Rect box;
    bool empty() const { return pts.size() < 3; }
};

struct DrawList {
    std::vector<DrawBatch> batches;
    std::vector<TextRun> texts;
    void clear() { batches.clear(), texts.clear(); }
};

class Instance : public Object {
public:
    enum Kind : uint8_t { Clip, Shape, Text };
    Instance(Player& player, Kind kind, const MovieDef* movie, const CharacterDef* def, ObjectPtr proto);
    ~Instance() override;

    Player& player;
    Kind kind;
    const MovieDef* movie;                  // characters are looked up here (loadMovie swaps it)
    const CharacterDef* def;                // SpriteDef / ShapeDef / EditTextDef
    Instance* parent_ = nullptr;
    std::string name;
    int depth = 0;                          // ActionScript depth (timeline depth - 16384)
    uint16_t character = 0;
    Matrix matrix;
    CxForm cxform;
    bool visible = true;
    bool accept_moves = true;               // the timeline still moves it (scripts have not)
    uint16_t clip_depth = 0;                // mask over the following depths (timeline depth)
    uint16_t ratio = 0;
    bool removed = false;
    bool constructed = false;
    float rot_x = 0, rot_y = 0, z = 0;      // gameswf 3D extensions (_xrotation, _yrotation, _z)

    // Clips
    std::vector<std::shared_ptr<Instance>> children;   // by depth
    int frame = -1;
    bool playing = true;
    std::shared_ptr<Instance> mask;         // setMask
    std::vector<ClipAction> clip_events;
    std::vector<ShapeGroup> drawing;        // drawing API
    glm::vec2 pen{0}, fill_start{0};        // drawing API state (twips)
    bool filling = false;
    LineStyle line_style;
    bool has_line = false;
    std::shared_ptr<SpriteDef> own_sprite;  // createEmptyMovieClip
    std::shared_ptr<EditTextDef> own_text;  // createTextField

    // Text fields
    std::string text;                       // plain
    std::string html_text;
    bool html = false;
    glm::vec4 text_color{0, 0, 0, 1};
    float text_size = 12;                   // pixels (movie units)
    std::string text_font;
    int text_align = 0;
    bool word_wrap = false, multiline = false, auto_size = false;
    float text_width = 0, text_height = 0;  // the box (movie pixels)

    Instance* as_instance() override { return this; }
    const char* type_of() const override { return kind == Clip ? "movieclip" : "object"; }
    bool get_special(VM& vm, const std::string& name, Value& out) override;
    bool set_special(VM& vm, const std::string& name, const Value& v) override;
    void special_names(std::vector<std::string>& out) const override;

    Instance* parent() const { return parent_; }
    std::string target_path() const;        // _level0.a.b
    const SpriteDef* sprite() const;
    int total_frames() const;
    int current_frame() const { return frame; }
    int find_label(const std::string& label) const;
    void goto_frame(int frame, bool play);
    void set_playing(bool play) { playing = play; }
    void duplicate(const std::string& name, int depth);
    void remove_self();
    Instance* child_at(int depth) const;
    Instance* child_named(const std::string& name) const;

    Matrix world_matrix() const;
    CxForm world_cxform() const;
    bool world_visible() const;
    Rect bounds(const Matrix& m) const;     // twips, under `m`
    void set_text(const std::string& value, bool as_html);
};

class Player {
public:
    // `loader`: other movies by file name (loadMovie, the minimaps); may be null.
    using Loader = std::function<std::shared_ptr<MovieDef>(const std::string& name)>;
    // `screen`: the viewport (pixels) the scripts see while the first frame builds (0: the
    // movie's own size).
    // `setup` runs before the first frame (to set the host hooks: strings, natives, text).
    Player(std::shared_ptr<MovieDef> movie, Loader loader = nullptr, glm::vec2 screen = glm::vec2(0),
           const std::function<void(Player&)>& setup = nullptr);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    VM vm;
    const MovieDef& movie() const { return *movie_; }
    std::shared_ptr<Instance> root() const { return root_; }

    // The screen rectangle (pixels) the movie is shown in, scaled to fit it whole ("show all",
    // centred); scripts see the visible part as _global.viewport (movie units).
    void set_viewport(float width, float height);
    float scale() const { return scale_; }
    glm::vec2 offset() const { return offset_; }

    // Run the movie for `dt` seconds (frames at its frame rate).
    void advance(float dt);
    void step_frame();
    void render(DrawList& out);

    // Pointer input in screen pixels.
    void pointer_move(float x, float y);
    void pointer_down(float x, float y);
    void pointer_up(float x, float y);

    // Keyboard focus: the clips a pointer could press (onPress / onRelease), visible and enabled,
    // with their hit area's centre on screen; hover one (roll over, the last one rolls out) and
    // press it (press, release).
    struct Button {
        Instance* clip;
        glm::vec2 center;   // screen pixels
    };
    std::vector<Button> buttons();
    void hover(Instance* clip);
    void press(Instance* clip);

    // The host API (paths like "mMenuStack.menu" from the root).
    Instance* find(const std::string& path) const;
    Value invoke(const std::string& path, const std::string& method, std::vector<Value> args = {});
    void set_member(const std::string& path, const std::string& member, const Value& v);   // path "_global": a global
    Value get_member(const std::string& path, const std::string& member);
    bool goto_and_play(const std::string& path, const std::string& label);
    bool goto_and_stop(const std::string& path, const std::string& label);
    bool set_visible(const std::string& path, bool visible);
    bool set_text(const std::string& path, const std::string& text, bool html = false);
    bool load_movie(const std::string& path, const std::string& file);

    // Internals used by instances and the VM.
    std::shared_ptr<Instance> create(Instance* parent, const MovieDef* movie, uint16_t id, int depth,
                                     const std::string& name);
    void construct(const std::shared_ptr<Instance>& inst, const Value& init = {});
    void run_frame(Instance* clip, int frame);      // controls + queued actions of one frame
    void rebuild(Instance* clip, int frame);        // display list as of `frame` (goto backwards)
    void place(Instance* clip, const PlaceTag& tag);
    void remove_depth(Instance* clip, int depth);
    void queue(std::function<void()> fn) { queue_.push_back(std::move(fn)); }
    void run_queue();
    void unload(Instance* inst);
    const std::vector<Mesh>& meshes(const ShapeDef* shape, const std::vector<ShapeGroup>* groups = nullptr);
    glm::vec2 pointer() const { return pointer_; }   // movie units
    std::function<void(const std::string&)> log;
    Loader loader;
    std::vector<std::shared_ptr<MovieDef>> movies;   // loaded with loadMovie (kept alive)
    std::function<glm::vec2(const TextRun&)> measure_text;   // pixels; null: rough estimate
    // The game's native functions the scripts call on _global (CMenuManager::Init registers 25:
    // NativeSwitchMenu, registerInGameEvent, ...): those without a built-in answer report here.
    std::function<Value(const std::string& name, CallArgs& args)> on_native;

private:
    void install_classes();
    void each_clip(Instance* inst, std::vector<std::shared_ptr<Instance>>& out);
    void draw(Instance* inst, const Matrix& m, const CxForm& cx, DrawList& out, const ClipShape* clip);
    void draw_shape(const std::vector<Mesh>& meshes, const MovieDef* movie, const Matrix& m, const CxForm& cx,
                    DrawList& out, const ClipShape* clip);
    void draw_text(Instance* inst, const Matrix& m, const CxForm& cx, DrawList& out, const ClipShape* clip);
    ClipShape mask_shape(Instance* mask, const Matrix& m, const ClipShape* within);
    Instance* hit(Instance* inst, glm::vec2 p);
    bool is_button(Instance* c);
    void collect_buttons(Instance* inst, std::vector<Button>& out);
    void dispatch(Instance* inst, const char* event);

    // gameswf's 3D clips (_xrotation / _yrotation / _z): their content is drawn in their own
    // space, rotated about their registration point and projected with Flash's default
    // perspective (55 degrees across the stage, centred on it).
    struct Persp {
        Matrix screen;              // parent space -> screen
        Matrix linear;              // the clip's own 2D matrix without its translation
        glm::vec3 origin;           // the clip's position in its parent (twips), z included
        float sx, cx_, sy, cy;      // rotation sines/cosines (y, x)
        glm::vec2 center;           // projection centre, parent space
        float focal;                // parent space
        glm::vec2 project(glm::vec2 local) const;
    };
    const Persp* persp_ = nullptr;
    Persp make_persp(const Instance* inst, const Matrix& parent_screen) const;
    // A local rectangle's corners on screen, through the 3D clips above it as drawn.
    void screen_corners(const Instance* inst, const Rect& r, glm::vec2 out[4]) const;
    glm::vec2 screen_pointer_{0};
    glm::vec2 to_screen(const Matrix& m, glm::vec2 p) const { return persp_ ? persp_->project(m.apply(p)) : m.apply(p); }

    std::shared_ptr<MovieDef> movie_;
    std::shared_ptr<Instance> root_;
    std::vector<std::function<void()>> queue_;
    std::unordered_map<const void*, std::vector<Mesh>> mesh_cache_;
    std::set<const void*> init_done_;            // DoInitAction blocks already run
    float scale_ = 1;
    glm::vec2 offset_{0};
    glm::vec2 screen_{960, 640};
    double time_ = 0, frame_time_ = 0;
    glm::vec2 pointer_{0};
    bool pointer_down_ = false;
    std::weak_ptr<Instance> hover_, pressed_;
};

}  // namespace tdkr::swf
