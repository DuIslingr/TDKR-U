// TDKR's Flash UI files (gameswf_*/*.swf): Flash 8-10 movies with ActionScript 2, as
// Gameloft's gameswf plays them. This is the definition side: characters, timelines and
// action buffers parsed from the file (tools/swf.py is the Python reference).
//
// Gameloft's exporter moved the bitmaps into texture atlases beside the movie
// (<movie>_atlas<id>.tga): the SWF keeps an empty DefineBitsLossless per atlas id and the
// bitmap fills' matrices place the image in the atlas.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "core/bytes.h"

namespace tdkr::swf {

// 2D affine transform, twips: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    glm::vec2 apply(glm::vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Matrix operator*(const Matrix& n) const;   // this after n
    Matrix inverse() const;
    float x_scale() const;
    float y_scale() const;
    float rotation() const;                    // radians
    void set_scale_rotation(float xs, float ys, float rot);
};

// Colour transform: c' = c * mul + add (add in 0..1 units).
struct CxForm {
    glm::vec4 mul{1.0f}, add{0.0f};
    CxForm operator*(const CxForm& inner) const {
        return {mul * inner.mul, inner.add * mul + add};
    }
    glm::vec4 apply(const glm::vec4& c) const { return glm::clamp(c * mul + add, 0.0f, 1.0f); }
};

struct Rect {
    float xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    bool empty() const { return xmax <= xmin || ymax <= ymin; }
};

struct FillStyle {
    enum Kind : uint8_t { Solid, Linear, Radial, Focal, Bitmap } kind = Solid;
    glm::vec4 color{0, 0, 0, 1};
    Matrix matrix;                                         // gradient square / bitmap pixels -> shape
    std::vector<std::pair<float, glm::vec4>> stops;        // ratio 0..1
    uint8_t spread = 0;                                    // 0 pad, 1 reflect, 2 repeat
    float focal = 0;
    uint16_t bitmap = 0;
    bool smooth = true, repeat = true;
};

struct LineStyle {
    float width = 0;                                       // twips
    glm::vec4 color{0, 0, 0, 1};
};

struct Edge {
    glm::vec2 p0, control, p1;
    bool curve = false;
};

// A shape's edges grouped by the style arrays they index (each NewStyles starts a group).
struct ShapeGroup {
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    std::vector<std::vector<Edge>> fill_edges;             // [style - 1]: fill1 edges, fill0 edges reversed
    std::vector<std::vector<Edge>> line_edges;             // [style - 1], record order
};

struct CharacterDef {
    enum Kind : uint8_t { Shape, Sprite, Bitmap, Font, EditText } kind;
    uint16_t id = 0;
    explicit CharacterDef(Kind k) : kind(k) {}
    virtual ~CharacterDef() = default;
};

struct ShapeDef : CharacterDef {
    ShapeDef() : CharacterDef(Shape) {}
    Rect bounds;
    std::vector<ShapeGroup> groups;
};

struct BitmapDef : CharacterDef {
    BitmapDef() : CharacterDef(Bitmap) {}
    int width = 0, height = 0;
    Bytes rgba;                                            // empty: in <movie>_atlas<id>.tga
};

struct FontDef : CharacterDef {
    FontDef() : CharacterDef(Font) {}
    std::string name;
    bool bold = false, italic = false;
    float em = 1024;                                       // glyph units per em (DefineFont3: 1024*20)
    std::vector<ShapeGroup> glyphs;                        // empty for device fonts
    std::unordered_map<uint32_t, int> index;               // code point -> glyph
    float ascent = 0, descent = 0, leading = 0;            // glyph units
    std::vector<float> advances;
};

struct EditTextDef : CharacterDef {
    EditTextDef() : CharacterDef(EditText) {}
    Rect bounds;
    bool word_wrap = false, multiline = false, password = false, read_only = false, html = false,
         use_outlines = false, auto_size = false, border = false, no_select = false;
    uint16_t font = 0;
    float height = 240;                                    // twips
    glm::vec4 color{0, 0, 0, 1};
    int max_length = 0;
    uint8_t align = 0;                                     // 0 left, 1 right, 2 center, 3 justify
    float left_margin = 0, right_margin = 0, indent = 0, leading = 0;
    std::string variable, text;
};

using ActionBuffer = std::shared_ptr<const Bytes>;

struct ClipAction {
    uint32_t events = 0;
    uint8_t key = 0;
    ActionBuffer code;
};

struct PlaceTag {
    uint16_t depth = 0;
    bool move = false;
    bool has_char = false, has_matrix = false, has_cxform = false, has_ratio = false, has_name = false;
    uint16_t character = 0;
    Matrix matrix;
    CxForm cxform;
    uint16_t ratio = 0;
    std::string name;
    uint16_t clip_depth = 0;                               // 0: not a mask
    int8_t visible = -1;                                   // PlaceObject3; -1 unset
    uint8_t blend = 0;
    std::vector<ClipAction> clip_actions;
};

struct Control {
    enum Kind : uint8_t { Place, Remove } kind = Place;
    PlaceTag place;                                        // Remove: place.depth
};

struct Frame {
    std::string label;
    std::vector<Control> controls;
    std::vector<ActionBuffer> actions;                     // DoAction, in order
    std::vector<std::pair<uint16_t, ActionBuffer>> init_actions;   // DoInitAction (sprite id, code)
};

struct SpriteDef : CharacterDef {
    SpriteDef() : CharacterDef(Sprite) {}
    uint16_t frame_count = 0;
    std::vector<Frame> frames;
    int label_frame(const std::string& label) const;      // -1 if none (case-insensitive)
};

struct MovieDef {
    std::string name;                                      // file stem (atlas names)
    int version = 0;
    Rect frame_size;
    float frame_rate = 20;
    glm::vec4 background{1.0f};
    SpriteDef root;
    std::unordered_map<uint16_t, std::shared_ptr<CharacterDef>> characters;
    std::unordered_map<std::string, uint16_t> exports;     // linkage name -> id

    const CharacterDef* character(uint16_t id) const;
    // A linkage name's character id (exact, then case-insensitive); 0 if none.
    uint16_t exported(const std::string& name) const;
    std::string export_name(uint16_t id) const;
    float width() const { return (frame_size.xmax - frame_size.xmin) / 20.0f; }
    float height() const { return (frame_size.ymax - frame_size.ymin) / 20.0f; }
};

// Parse a .swf (FWS or zlib CWS). `name`: the movie's file stem.
std::shared_ptr<MovieDef> load_movie(const Bytes& data, const std::string& name);

}  // namespace tdkr::swf
