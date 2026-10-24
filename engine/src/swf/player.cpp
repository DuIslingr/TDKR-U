#include "swf/player.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace tdkr::swf {

namespace {

constexpr float kTwips = 20.0f;
constexpr int kTimelineDepth = 16384;   // ActionScript depth = tag depth - 16384
constexpr float kRadToDeg = 57.29577951308232f;

std::shared_ptr<Instance> share(Instance* i) { return std::static_pointer_cast<Instance>(i->shared_from_this()); }

const SpriteDef& empty_sprite() {
    static SpriteDef s = [] {
        SpriteDef d;
        d.frame_count = 1;
        d.frames.resize(1);
        return d;
    }();
    return s;
}

Rect transform_rect(const Rect& r, const Matrix& m) {
    const glm::vec2 c[4] = {m.apply({r.xmin, r.ymin}), m.apply({r.xmax, r.ymin}), m.apply({r.xmin, r.ymax}),
                            m.apply({r.xmax, r.ymax})};
    Rect o{c[0].x, c[0].x, c[0].y, c[0].y};
    for (const auto& p : c) {
        o.xmin = std::min(o.xmin, p.x), o.xmax = std::max(o.xmax, p.x);
        o.ymin = std::min(o.ymin, p.y), o.ymax = std::max(o.ymax, p.y);
    }
    return o;
}

void grow(Rect& r, const Rect& b, bool& any) {
    if (b.empty()) return;
    if (!any) {
        r = b;
        any = true;
        return;
    }
    r.xmin = std::min(r.xmin, b.xmin), r.xmax = std::max(r.xmax, b.xmax);
    r.ymin = std::min(r.ymin, b.ymin), r.ymax = std::max(r.ymax, b.ymax);
}

Rect shape_bounds(const std::vector<ShapeGroup>& groups) {
    Rect r;
    bool any = false;
    for (const auto& g : groups)
        for (const auto* list : {&g.fill_edges, &g.line_edges})
            for (const auto& edges : *list)
                for (const Edge& e : edges) {
                    for (glm::vec2 p : {e.p0, e.control, e.p1}) grow(r, Rect{p.x, p.x + 0.01f, p.y, p.y + 0.01f}, any);
                }
    return r;
}

// Strip HTML to text; take the first colour / size / alignment it sets.
struct HtmlText {
    std::string text;
    int align = -1;
    float size = 0;
    bool has_color = false;
    glm::vec4 color{0, 0, 0, 1};
};

std::string attribute(const std::string& tag, const std::string& name) {
    const std::string lt = to_lower(tag);
    size_t p = lt.find(name + "=");
    if (p == std::string::npos) return {};
    p += name.size() + 1;
    if (p >= tag.size()) return {};
    const char q = tag[p];
    if (q == '"' || q == '\'') {
        const size_t e = tag.find(q, p + 1);
        return tag.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    const size_t e = tag.find_first_of(" >", p);
    return tag.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

HtmlText parse_html(const std::string& html) {
    HtmlText out;
    bool first_p = true;
    for (size_t i = 0; i < html.size();) {
        if (html[i] == '<') {
            const size_t e = html.find('>', i);
            if (e == std::string::npos) break;
            const std::string tag = html.substr(i + 1, e - i - 1);
            const std::string lt = to_lower(tag);
            if (lt.rfind("br", 0) == 0) out.text += '\n';
            else if (lt.rfind("p", 0) == 0 && (lt.size() == 1 || lt[1] == ' ')) {
                if (!first_p) out.text += '\n';
                first_p = false;
                const std::string a = to_lower(attribute(tag, "align"));
                if (out.align < 0 && !a.empty()) out.align = a == "right" ? 1 : a == "center" ? 2 : 0;
            } else if (lt.rfind("font", 0) == 0) {
                const std::string c = attribute(tag, "color"), s = attribute(tag, "size");
                if (!out.has_color && c.size() == 7 && c[0] == '#') {
                    const unsigned v = unsigned(std::strtoul(c.c_str() + 1, nullptr, 16));
                    out.color = glm::vec4(float(v >> 16 & 255) / 255.0f, float(v >> 8 & 255) / 255.0f, float(v & 255) / 255.0f, 1.0f);
                    out.has_color = true;
                }
                if (out.size == 0 && !s.empty()) out.size = float(std::atof(s.c_str()));
            }
            i = e + 1;
        } else if (html[i] == '&') {
            const size_t e = html.find(';', i);
            const std::string ent = e == std::string::npos ? "" : html.substr(i + 1, e - i - 1);
            if (ent == "lt") out.text += '<';
            else if (ent == "gt") out.text += '>';
            else if (ent == "amp") out.text += '&';
            else if (ent == "quot") out.text += '"';
            else if (ent == "apos") out.text += '\'';
            else if (ent == "nbsp") out.text += ' ';
            else if (!ent.empty() && ent[0] == '#') out.text += utf8_from_code_points(std::u32string(1, char32_t(std::atoi(ent.c_str() + 1))));
            else {
                out.text += '&';
                i += 1;
                continue;
            }
            i = e + 1;
        } else {
            out.text += html[i++];
        }
    }
    return out;
}

// Which side of the clip edge a -> b a point is on (>= 0 inside for the hull's winding).
float side(glm::vec2 a, glm::vec2 b, glm::vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

// Sutherland-Hodgman against the convex mask outline.
void clip_triangle(const DrawVertex* t, const ClipShape& c, std::vector<DrawVertex>& out) {
    std::vector<DrawVertex> poly(t, t + 3), next;
    for (size_t e = 0; e < c.pts.size() && !poly.empty(); ++e) {
        const glm::vec2 a = c.pts[e], b = c.pts[(e + 1) % c.pts.size()];
        next.clear();
        for (size_t i = 0; i < poly.size(); ++i) {
            const DrawVertex& p = poly[i];
            const DrawVertex& q = poly[(i + 1) % poly.size()];
            const float sp = side(a, b, p.pos), sq = side(a, b, q.pos);
            if (sp >= 0) next.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
                const float k = sp / (sp - sq);
                next.push_back({p.pos + (q.pos - p.pos) * k, p.uv + (q.uv - p.uv) * k, p.color + (q.color - p.color) * k});
            }
        }
        poly.swap(next);
    }
    for (size_t i = 1; i + 1 < poly.size(); ++i) out.insert(out.end(), {poly[0], poly[i], poly[i + 1]});
}

bool inside(const ClipShape& c, glm::vec2 p) {
    for (size_t e = 0; e < c.pts.size(); ++e)
        if (side(c.pts[e], c.pts[(e + 1) % c.pts.size()], p) < 0) return false;
    return true;
}

// Convex hull (monotone chain), wound so that side() >= 0 is inside.
ClipShape hull(std::vector<glm::vec2> pts) {
    ClipShape c;
    std::sort(pts.begin(), pts.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return c;
    std::vector<glm::vec2> h(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && side(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && side(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    c.pts = std::move(h);
    c.box = Rect{c.pts[0].x, c.pts[0].x, c.pts[0].y, c.pts[0].y};
    for (glm::vec2 p : c.pts) {
        c.box.xmin = std::min(c.box.xmin, p.x), c.box.xmax = std::max(c.box.xmax, p.x);
        c.box.ymin = std::min(c.box.ymin, p.y), c.box.ymax = std::max(c.box.ymax, p.y);
    }
    return c;
}

DrawBatch& batch_for(DrawList& out, const TextureRef& t) {
    if (out.batches.empty() || !(out.batches.back().texture == t)) out.batches.push_back({t, {}});
    return out.batches.back();
}

void emit(DrawList& out, const TextureRef& t, const DrawVertex* v, size_t n, const ClipShape* clip) {
    DrawBatch& b = batch_for(out, t);
    if (!clip) {
        b.vertices.insert(b.vertices.end(), v, v + n);
        return;
    }
    if (clip->empty()) return;
    const Rect& r = clip->box;
    for (size_t i = 0; i + 2 < n; i += 3) {
        const DrawVertex* tri = v + i;
        const float x0 = std::min({tri[0].pos.x, tri[1].pos.x, tri[2].pos.x}), x1 = std::max({tri[0].pos.x, tri[1].pos.x, tri[2].pos.x});
        const float y0 = std::min({tri[0].pos.y, tri[1].pos.y, tri[2].pos.y}), y1 = std::max({tri[0].pos.y, tri[1].pos.y, tri[2].pos.y});
        if (x1 < r.xmin || x0 > r.xmax || y1 < r.ymin || y0 > r.ymax) continue;
        if (inside(*clip, tri[0].pos) && inside(*clip, tri[1].pos) && inside(*clip, tri[2].pos))
            b.vertices.insert(b.vertices.end(), tri, tri + 3);
        else
            clip_triangle(tri, *clip, b.vertices);
    }
}

}  // namespace

// --- Instance ------------------------------------------------------------------------------------

Instance::Instance(Player& p, Kind k, const MovieDef* m, const CharacterDef* d, ObjectPtr proto)
    : Object(std::move(proto)), player(p), kind(k), movie(m), def(d) {}

Instance::~Instance() = default;

const SpriteDef* Instance::sprite() const {
    if (kind != Clip) return nullptr;
    return def && def->kind == CharacterDef::Sprite ? static_cast<const SpriteDef*>(def) : &empty_sprite();
}

int Instance::total_frames() const {
    const SpriteDef* s = sprite();
    return s ? std::max<int>(1, int(s->frames.size())) : 1;
}

int Instance::find_label(const std::string& label) const {
    const SpriteDef* s = sprite();
    return s ? s->label_frame(label) : -1;
}

std::string Instance::target_path() const {
    if (!parent_) return "_level0";
    return parent_->target_path() + "." + name;
}

Instance* Instance::child_at(int d) const {
    for (const auto& c : children)
        if (c->depth == d) return c.get();
    return nullptr;
}

Instance* Instance::child_named(const std::string& n) const {
    for (const auto& c : children)
        if (c->name == n && !c->removed) return c.get();
    return nullptr;
}

Matrix Instance::world_matrix() const { return parent_ ? parent_->world_matrix() * matrix : matrix; }
CxForm Instance::world_cxform() const { return parent_ ? parent_->world_cxform() * cxform : cxform; }
bool Instance::world_visible() const { return visible && !removed && (!parent_ || parent_->world_visible()); }

Rect Instance::bounds(const Matrix& m) const {
    Rect r;
    bool any = false;
    switch (kind) {
        case Shape: return transform_rect(static_cast<const ShapeDef*>(def)->bounds, m);
        case Text: {
            const auto* t = static_cast<const EditTextDef*>(def);
            Rect b = t->bounds;
            b.xmax = std::max(b.xmax, b.xmin + text_width * kTwips);
            b.ymax = std::max(b.ymax, b.ymin + text_height * kTwips);
            return transform_rect(b, m);
        }
        case Clip:
            if (!drawing.empty()) grow(r, transform_rect(shape_bounds(drawing), m), any);
            for (const auto& c : children)
                if (!c->removed) grow(r, c->bounds(m * c->matrix), any);
            return r;
    }
    return r;
}

void Instance::goto_frame(int f, bool play) {
    if (kind != Clip) return;
    playing = play;
    f = std::clamp(f, 0, total_frames() - 1);
    if (f == frame) return;
    if (f > frame) {
        for (int i = frame + 1; i < f; ++i) player.run_frame(this, i | 0x40000000);
        player.run_frame(this, f);
    } else {
        player.rebuild(this, f);
    }
}

void Instance::duplicate(const std::string& n, int d) {
    if (!parent_) return;
    auto copy = player.create(parent_, movie, character, d, n);
    if (!copy) return;
    copy->matrix = matrix;
    copy->cxform = cxform;
    copy->visible = visible;
    copy->clip_events = clip_events;
    player.construct(copy);
}

void Instance::remove_self() {
    if (parent_) player.remove_depth(parent_, depth);
}

void Instance::set_text(const std::string& value, bool as_html) {
    html_text = value;
    if (as_html) {
        const HtmlText h = parse_html(value);
        text = h.text;
        if (h.align >= 0) text_align = h.align;
        if (h.has_color) text_color = h.color;
        if (h.size > 0) text_size = h.size;
    } else {
        text = value;
    }
    if (player.measure_text && !text_font.empty()) {
        TextRun run;
        run.text = text;
        run.font = text_font;
        run.size = text_size;
        run.wrap = word_wrap;
        run.width = text_width;
        const glm::vec2 sz = player.measure_text(run);
        if (auto_size) {
            const auto* t = static_cast<const EditTextDef*>(def);
            const float box = (t->bounds.xmax - t->bounds.xmin) / kTwips;
            if (!word_wrap) {
                text_width = std::max(box, sz.x + 4);
            }
            text_height = std::max(text_height, sz.y + 4);
        }
    }
}

bool Instance::get_special(VM& vm, const std::string& n, Value& out) {
    if (n.empty() || find_own(n)) return false;
    if (n[0] == '_') {
        if (n == "_x") out = Value(double(matrix.tx / kTwips));
        else if (n == "_y") out = Value(double(matrix.ty / kTwips));
        else if (n == "_xscale") out = Value(double(matrix.x_scale() * 100));
        else if (n == "_yscale") out = Value(double(matrix.y_scale() * 100));
        else if (n == "_rotation") out = Value(double(matrix.rotation() * kRadToDeg));
        else if (n == "_alpha") out = Value(double(cxform.mul.a * 100));
        else if (n == "_visible") out = Value(visible);
        else if (n == "_width" || n == "_height") {
            const Rect b = bounds(matrix);
            out = Value(double(b.empty() ? 0 : (n == "_width" ? b.xmax - b.xmin : b.ymax - b.ymin) / kTwips));
        } else if (n == "_currentframe") out = Value(frame + 1);
        else if (n == "_totalframes" || n == "_framesloaded") out = Value(total_frames());
        else if (n == "_name") out = Value(name);
        else if (n == "_target") {
            std::string p;
            for (const Instance* i = this; i && i->parent_; i = i->parent_) p = "/" + i->name + p;
            out = Value(p.empty() ? std::string("/") : p);
        } else if (n == "_parent") out = parent_ ? Value(parent_->shared_from_this()) : Value();
        else if (n == "_root" || n == "_level0") out = Value(player.root());
        else if (n == "_xmouse" || n == "_ymouse") {
            const glm::vec2 p = world_matrix().inverse().apply(player.pointer() * kTwips) / kTwips;
            out = Value(double(n == "_xmouse" ? p.x : p.y));
        } else if (n == "_url") out = Value(movie ? movie->name + ".swf" : std::string());
        else if (n == "_quality") out = Value("HIGH");
        else if (n == "_highquality") out = Value(1);
        else if (n == "_focusrect") out = Value(false);
        else if (n == "_droptarget") out = Value(std::string());
        else if (n == "_lockroot") out = Value(false);
        else if (n == "_soundbuftime") out = Value(5);
        else if (n == "_xrotation") out = Value(double(rot_x));
        else if (n == "_yrotation") out = Value(double(rot_y));
        else if (n == "_z") out = Value(double(z));
        else if (n == "_depth") out = Value(depth);
        else return false;
        return true;
    }
    if (kind == Text) {
        if (n == "text") out = Value(text);
        else if (n == "htmlText") out = Value(html ? html_text : text);
        else if (n == "html") out = Value(html);
        else if (n == "textColor") {
            const glm::ivec4 c = glm::ivec4(text_color * 255.0f + 0.5f);
            out = Value(double(c.r << 16 | c.g << 8 | c.b));
        } else if (n == "textWidth" || n == "textHeight") {
            glm::vec2 sz(text.size() * text_size * 0.5f, text_size * 1.2f);
            if (player.measure_text) {
                TextRun run;
                run.text = text;
                run.font = text_font;
                run.size = text_size;
                run.wrap = word_wrap;
                run.width = text_width;
                sz = player.measure_text(run);
            }
            out = Value(double(n == "textWidth" ? sz.x : sz.y));
        } else if (n == "length") out = vm.get_member(Value(text), "length");
        else if (n == "multiline") out = Value(multiline);
        else if (n == "wordWrap") out = Value(word_wrap);
        else if (n == "autoSize") out = Value(auto_size ? std::string("left") : std::string("none"));
        else if (n == "embedFonts") out = Value(true);
        else return false;
        return true;
    }
    if (kind == Clip) {
        if (Instance* c = child_named(n)) {
            out = Value(c->shared_from_this());
            return true;
        }
        if (n == "enabled" || n == "useHandCursor") {
            out = Value(true);
            return true;
        }
    }
    return false;
}

bool Instance::set_special(VM& vm, const std::string& n, const Value& v) {
    if (n.empty()) return false;
    if (n[0] == '_') {
        const double d = vm.to_number(v);
        const bool num = !std::isnan(d);
        if (n == "_x") { if (num) matrix.tx = float(d) * kTwips; accept_moves = false; }
        else if (n == "_y") { if (num) matrix.ty = float(d) * kTwips; accept_moves = false; }
        else if (n == "_xscale") {
            if (num) matrix.set_scale_rotation(float(d / 100), matrix.y_scale(), matrix.rotation());
            accept_moves = false;
        } else if (n == "_yscale") {
            if (num) matrix.set_scale_rotation(matrix.x_scale(), float(d / 100), matrix.rotation());
            accept_moves = false;
        } else if (n == "_rotation") {
            if (num) matrix.set_scale_rotation(matrix.x_scale(), matrix.y_scale(), float(d) / kRadToDeg);
            accept_moves = false;
        } else if (n == "_alpha") { if (num) cxform.mul.a = float(d / 100); accept_moves = false; }
        else if (n == "_visible") visible = VM::to_boolean(v);
        else if (n == "_width" || n == "_height") {
            if (!num) return true;
            const Rect b = bounds(matrix);
            const float cur = (n == "_width" ? b.xmax - b.xmin : b.ymax - b.ymin) / kTwips;
            if (kind == Text) {
                // a text field's box itself
                (n == "_width" ? text_width : text_height) = float(d);
            } else if (cur > 0) {
                const float k = float(d) / cur;
                if (n == "_width") matrix.set_scale_rotation(matrix.x_scale() * k, matrix.y_scale(), matrix.rotation());
                else matrix.set_scale_rotation(matrix.x_scale(), matrix.y_scale() * k, matrix.rotation());
            }
            accept_moves = false;
        } else if (n == "_name") name = vm.to_string(v);
        else if (n == "_xrotation") rot_x = float(d);
        else if (n == "_yrotation") rot_y = float(d);
        else if (n == "_z") z = float(d);
        else if (n == "_currentframe" || n == "_totalframes" || n == "_target" || n == "_parent" || n == "_url" ||
                 n == "_xmouse" || n == "_ymouse" || n == "_framesloaded" || n == "_droptarget")
            ;   // read only
        else return false;
        return true;
    }
    if (kind == Text) {
        if (n == "text") set_text(vm.to_string(v), false);
        else if (n == "htmlText") set_text(vm.to_string(v), html);
        else if (n == "html") html = VM::to_boolean(v);
        else if (n == "textColor") {
            const uint32_t c = uint32_t(vm.to_number(v));
            text_color = glm::vec4(float(c >> 16 & 255) / 255.0f, float(c >> 8 & 255) / 255.0f, float(c & 255) / 255.0f, 1.0f);
        } else if (n == "multiline") multiline = VM::to_boolean(v);
        else if (n == "wordWrap") word_wrap = VM::to_boolean(v);
        else if (n == "autoSize") {
            auto_size = v.type == Value::String ? v.s != "none" : VM::to_boolean(v);
            set_text(html ? html_text : text, html);
        } else return false;
        return true;
    }
    return false;
}

void Instance::special_names(std::vector<std::string>& out) const {
    for (const auto& c : children)
        if (!c->removed && !c->name.empty()) out.push_back(c->name);
}

// --- Player: building the display list -------------------------------------------------------------

Player::Player(std::shared_ptr<MovieDef> m, Loader l, glm::vec2 screen, const std::function<void(Player&)>& setup)
    : loader(std::move(l)), movie_(std::move(m)) {
    vm.version = movie_->version;
    vm.root = [this] { return ObjectPtr(root_); };
    vm.trace = [this](const std::string& s) {
        if (log) log("[as] " + s);
    };
    install_classes();
    set_viewport(screen.x > 0 ? screen.x : movie_->width(), screen.y > 0 ? screen.y : movie_->height());
    if (setup) setup(*this);
    root_ = std::make_shared<Instance>(*this, Instance::Clip, movie_.get(), &movie_->root, vm.movieclip_proto);
    root_->name = "_level0";
    construct(root_);
    run_queue();
}

Player::~Player() {
    // Break the parent links' reference cycles through scripts.
    vm.timers.clear();
    queue_.clear();
}

void Player::set_viewport(float width, float height) {
    screen_ = glm::vec2(width, height);
    const float mw = movie_->width(), mh = movie_->height();
    const float s = std::min(width / mw, height / mh);
    if (std::abs(s - scale_) > scale_ * 0.25f) mesh_cache_.clear();
    scale_ = s;
    offset_ = glm::vec2((width - mw * s) * 0.5f, (height - mh * s) * 0.5f);
    auto vp = vm.new_object();
    vp->set_own("xMin", Value(double(-offset_.x / s)));
    vp->set_own("yMin", Value(double(-offset_.y / s)));
    vp->set_own("xMax", Value(double((width - offset_.x) / s)));
    vp->set_own("yMax", Value(double((height - offset_.y) / s)));
    vm.global->set_own("viewport", Value(vp));
}

std::shared_ptr<Instance> Player::create(Instance* parent, const MovieDef* m, uint16_t id, int depth, const std::string& name) {
    const CharacterDef* def = m->character(id);
    if (!def) return nullptr;
    std::shared_ptr<Instance> inst;
    switch (def->kind) {
        case CharacterDef::Sprite:
            inst = std::make_shared<Instance>(*this, Instance::Clip, m, def, vm.movieclip_proto);
            break;
        case CharacterDef::Shape:
            inst = std::make_shared<Instance>(*this, Instance::Shape, m, def, vm.object_proto);
            break;
        case CharacterDef::EditText: {
            inst = std::make_shared<Instance>(*this, Instance::Text, m, def, vm.textfield_proto);
            const auto* t = static_cast<const EditTextDef*>(def);
            inst->text_color = t->color;
            inst->text_size = t->height / kTwips;
            inst->text_align = t->align == 3 ? 0 : t->align;
            inst->word_wrap = t->word_wrap;
            inst->multiline = t->multiline;
            inst->html = t->html;
            inst->auto_size = t->auto_size;
            inst->text_width = (t->bounds.xmax - t->bounds.xmin) / kTwips;
            inst->text_height = (t->bounds.ymax - t->bounds.ymin) / kTwips;
            if (const CharacterDef* f = m->character(t->font); f && f->kind == CharacterDef::Font)
                inst->text_font = static_cast<const FontDef*>(f)->name;
            inst->set_text(t->text, t->html);
            break;
        }
        default: return nullptr;
    }
    inst->character = id;
    inst->depth = depth;
    inst->name = name;
    inst->parent_ = parent;
    if (parent) {
        auto it = std::lower_bound(parent->children.begin(), parent->children.end(), depth,
                                   [](const std::shared_ptr<Instance>& c, int d) { return c->depth < d; });
        parent->children.insert(it, inst);
    }
    return inst;
}

void Player::construct(const std::shared_ptr<Instance>& inst, const Value& init) {
    if (inst->kind != Instance::Clip) {
        inst->constructed = true;
        return;
    }
    ObjectPtr ctor;
    if (inst->parent_ && inst->movie) {
        const std::string link = inst->movie->export_name(inst->character);
        if (!link.empty())
            if (auto it = vm.registered.find(link); it != vm.registered.end()) ctor = it->second;
    }
    if (ctor) {
        const Value proto = vm.get_member(Value(ctor), "prototype");
        if (proto.is_object()) inst->proto = proto.o;
    }
    if (init.is_object())
        for (const std::string& k : init.o->own_names())
            if (const Property* p = init.o->find_own(k)) vm.set_member(Value(ObjectPtr(inst)), k, p->value);
    inst->frame = -1;
    run_frame(inst.get(), 0);
    inst->constructed = true;
    if (ctor) {
        auto* f = ctor->as_function();
        if (f) {
            const Value proto = vm.get_member(Value(ctor), "prototype");
            vm.call(Value(ctor), Value(ObjectPtr(inst)), {}, proto.is_object() ? proto.o : nullptr);
        }
    }
    std::weak_ptr<Instance> weak = inst;
    for (const ClipAction& ca : inst->clip_events)
        if (ca.events & 0x201)   // load / initialize
            queue([this, weak, code = ca.code] {
                if (auto i = weak.lock(); i && !i->removed) vm.run_actions(code, i);
            });
    queue([this, weak] {
        if (auto i = weak.lock(); i && !i->removed) dispatch(i.get(), "onLoad");
    });
}

void Player::run_frame(Instance* clip, int f) {
    const bool actions = !(f & 0x40000000);
    f &= ~0x40000000;
    const SpriteDef* s = clip->sprite();
    clip->frame = f;
    if (!s || f >= int(s->frames.size())) return;
    const Frame& fr = s->frames[size_t(f)];
    for (const auto& [id, code] : fr.init_actions) {
        if (!init_done_.insert(code.get()).second) continue;
        vm.run_actions(code, root_ ? ObjectPtr(root_) : ObjectPtr(share(clip)));
    }
    if (actions) {
        std::weak_ptr<Instance> weak = share(clip);
        for (const ActionBuffer& code : fr.actions)
            queue([this, weak, code] {
                if (auto i = weak.lock(); i && !i->removed) vm.run_actions(code, i);
            });
    }
    for (const Control& c : fr.controls) {
        if (c.kind == Control::Remove) remove_depth(clip, int(c.place.depth) - kTimelineDepth);
        else place(clip, c.place);
    }
}

void Player::place(Instance* clip, const PlaceTag& tag) {
    const int d = int(tag.depth) - kTimelineDepth;
    Instance* existing = clip->child_at(d);
    auto apply = [&](Instance* i, bool all) {
        if (tag.has_matrix && (all || i->accept_moves)) i->matrix = tag.matrix;
        if (tag.has_cxform && (all || i->accept_moves)) i->cxform = tag.cxform;
        if (tag.has_ratio) i->ratio = tag.ratio;
        if (tag.has_name) i->name = tag.name;
        if (tag.clip_depth) i->clip_depth = tag.clip_depth;
        if (tag.visible >= 0) i->visible = tag.visible != 0;
    };
    if (tag.move && existing && (!tag.has_char || tag.character == existing->character)) {
        apply(existing, false);
        return;
    }
    if (!tag.has_char) return;
    Matrix keep_m;
    CxForm keep_cx;
    std::string keep_name;
    const bool replace = tag.move && existing;
    if (replace) keep_m = existing->matrix, keep_cx = existing->cxform, keep_name = existing->name;
    if (existing) remove_depth(clip, d);
    static int counter = 0;
    const std::string name = tag.has_name ? tag.name : replace ? keep_name : "instance" + std::to_string(++counter);
    auto inst = create(clip, clip->movie, tag.character, d, name);
    if (!inst) return;
    if (replace) inst->matrix = keep_m, inst->cxform = keep_cx;
    apply(inst.get(), true);
    inst->clip_events = tag.clip_actions;
    construct(inst);
}

void Player::remove_depth(Instance* clip, int d) {
    for (size_t i = 0; i < clip->children.size(); ++i)
        if (clip->children[i]->depth == d) {
            auto c = clip->children[i];
            clip->children.erase(clip->children.begin() + long(i));
            unload(c.get());
            return;
        }
}

void Player::unload(Instance* inst) {
    if (inst->removed) return;
    if (inst->kind == Instance::Clip) {
        Value fn;
        if (vm.lookup(inst, "onUnload", fn, Value(inst->shared_from_this())) && fn.function())
            vm.call(fn, Value(inst->shared_from_this()), {});
    }
    inst->removed = true;
    for (auto& c : inst->children) unload(c.get());
}

void Player::rebuild(Instance* clip, int f) {
    const SpriteDef* s = clip->sprite();
    if (!s) return;
    // The timeline's display list at `f`, replayed from the start.
    std::map<int, PlaceTag> want;
    for (int i = 0; i <= f && i < int(s->frames.size()); ++i)
        for (const Control& c : s->frames[size_t(i)].controls) {
            const int d = int(c.place.depth) - kTimelineDepth;
            if (c.kind == Control::Remove) {
                want.erase(d);
                continue;
            }
            const PlaceTag& t = c.place;
            auto it = want.find(d);
            if (!t.move || it == want.end() || (t.has_char && t.character != it->second.character)) {
                if (t.has_char) want[d] = t;
                continue;
            }
            PlaceTag& w = it->second;
            if (t.has_matrix) w.matrix = t.matrix, w.has_matrix = true;
            if (t.has_cxform) w.cxform = t.cxform, w.has_cxform = true;
            if (t.has_ratio) w.ratio = t.ratio, w.has_ratio = true;
            if (t.has_name) w.name = t.name, w.has_name = true;
            if (t.clip_depth) w.clip_depth = t.clip_depth;
        }
    std::vector<int> drop;
    for (const auto& c : clip->children) {
        if (c->depth >= 0) continue;     // made by script
        auto it = want.find(c->depth);
        if (it == want.end() || it->second.character != c->character) drop.push_back(c->depth);
    }
    for (int d : drop) remove_depth(clip, d);
    clip->frame = f;
    std::weak_ptr<Instance> weak = share(clip);
    for (const ActionBuffer& code : s->frames[size_t(f)].actions)
        queue([this, weak, code] {
            if (auto i = weak.lock(); i && !i->removed) vm.run_actions(code, i);
        });
    for (auto& [d, t] : want) {
        if (Instance* e = clip->child_at(d)) {
            if (e->accept_moves) {
                e->matrix = t.has_matrix ? t.matrix : Matrix{};
                e->cxform = t.has_cxform ? t.cxform : CxForm{};
            }
            if (t.has_ratio) e->ratio = t.ratio;
            continue;
        }
        PlaceTag fresh = t;
        fresh.move = false;
        place(clip, fresh);
    }
}

void Player::run_queue() {
    for (size_t guard = 0; !queue_.empty() && guard < 100000; ++guard) {
        auto q = std::move(queue_);
        queue_.clear();
        for (auto& fn : q) fn();
    }
}

void Player::each_clip(Instance* inst, std::vector<std::shared_ptr<Instance>>& out) {
    if (inst->kind != Instance::Clip || inst->removed) return;
    out.push_back(share(inst));
    for (const auto& c : inst->children) each_clip(c.get(), out);
}

void Player::dispatch(Instance* inst, const char* event) {
    Value fn;
    const Value self(inst->shared_from_this());
    if (vm.lookup(inst, event, fn, self) && fn.function()) vm.call(fn, self, {});
}

void Player::advance(float dt) {
    time_ += dt;
    frame_time_ += dt;
    const double step = 1.0 / std::max(1.0f, movie_->frame_rate);
    int n = 0;
    while (frame_time_ >= step && n < 4) {
        frame_time_ -= step;
        step_frame();
        ++n;
    }
    if (frame_time_ > step) frame_time_ = 0;
}

void Player::step_frame() {
    vm.update_timers(time_ * 1000.0);
    std::vector<std::shared_ptr<Instance>> clips;
    each_clip(root_.get(), clips);
    for (const auto& c : clips) {
        if (c->removed) continue;
        dispatch(c.get(), "onEnterFrame");
        for (const ClipAction& ca : c->clip_events)
            if (ca.events & 0x02) vm.run_actions(ca.code, c);
    }
    run_queue();
    for (const auto& c : clips) {
        if (c->removed || !c->constructed || !c->playing) continue;
        const int total = c->total_frames();
        if (total <= 1) continue;
        const int next = c->frame + 1;
        if (next >= total) rebuild(c.get(), 0);
        else run_frame(c.get(), next);
    }
    run_queue();
}

// --- input ---------------------------------------------------------------------------------------------

Instance* Player::hit(Instance* inst, glm::vec2 p) {
    if (!inst->visible || inst->removed || inst->kind != Instance::Clip) return nullptr;
    for (auto it = inst->children.rbegin(); it != inst->children.rend(); ++it) {
        Instance* c = it->get();
        if (c->kind != Instance::Clip || !c->visible || c->removed) continue;
        if (Instance* h = hit(c, p)) return h;
        Value handler;
        const Value self(c->shared_from_this());
        const bool button = (vm.lookup(c, "onPress", handler, self) && handler.function()) ||
                            (vm.lookup(c, "onRelease", handler, self) && handler.function());
        if (!button) continue;
        Value enabled = vm.get_member(self, "enabled");
        if (!enabled.undefined() && !VM::to_boolean(enabled)) continue;
        Value area = vm.get_member(self, "hitArea");
        Instance* target = area.instance() ? area.instance() : c;
        const Rect b = target->bounds(target->world_matrix());
        if (p.x * kTwips >= b.xmin && p.x * kTwips <= b.xmax && p.y * kTwips >= b.ymin && p.y * kTwips <= b.ymax) return c;
    }
    return nullptr;
}

void Player::pointer_move(float x, float y) {
    pointer_ = (glm::vec2(x, y) - offset_) / scale_;
    Instance* h = hit(root_.get(), pointer_);
    auto old = hover_.lock();
    if (old.get() != h) {
        if (old && !old->removed) dispatch(old.get(), pointer_down_ ? "onDragOut" : "onRollOut");
        if (h) dispatch(h, pointer_down_ ? "onDragOver" : "onRollOver");
        hover_ = h ? share(h) : nullptr;
    }
    std::vector<std::shared_ptr<Instance>> clips;
    each_clip(root_.get(), clips);
    for (const auto& c : clips) dispatch(c.get(), "onMouseMove");
    run_queue();
}

void Player::pointer_down(float x, float y) {
    pointer_move(x, y);
    pointer_down_ = true;
    std::vector<std::shared_ptr<Instance>> clips;
    each_clip(root_.get(), clips);
    for (const auto& c : clips) dispatch(c.get(), "onMouseDown");
    if (Instance* h = hit(root_.get(), pointer_)) {
        pressed_ = share(h);
        dispatch(h, "onPress");
    }
    run_queue();
}

void Player::pointer_up(float x, float y) {
    pointer_move(x, y);
    pointer_down_ = false;
    std::vector<std::shared_ptr<Instance>> clips;
    each_clip(root_.get(), clips);
    for (const auto& c : clips) dispatch(c.get(), "onMouseUp");
    if (auto p = pressed_.lock(); p && !p->removed) {
        dispatch(p.get(), hit(root_.get(), pointer_) == p.get() ? "onRelease" : "onReleaseOutside");
    }
    pressed_.reset();
    run_queue();
}

// --- host API ----------------------------------------------------------------------------------------------

Instance* Player::find(const std::string& path) const {
    Instance* cur = root_.get();
    size_t i = 0;
    while (cur && i <= path.size()) {
        size_t j = path.find('.', i);
        if (j == std::string::npos) j = path.size();
        const std::string part = path.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == "_root" || part == "_level0") continue;
        Value v = const_cast<VM&>(vm).get_member(Value(cur->shared_from_this()), part);
        cur = v.instance();
    }
    return cur;
}

Value Player::invoke(const std::string& path, const std::string& method, std::vector<Value> args) {
    Instance* i = find(path);
    if (!i) return {};
    Value r = vm.call_method(Value(i->shared_from_this()), method, std::move(args));
    run_queue();
    return r;
}

void Player::set_member(const std::string& path, const std::string& member, const Value& v) {
    if (Instance* i = find(path)) vm.set_member(Value(i->shared_from_this()), member, v);
}

Value Player::get_member(const std::string& path, const std::string& member) {
    Instance* i = find(path);
    return i ? vm.get_member(Value(i->shared_from_this()), member) : Value();
}

bool Player::goto_and_play(const std::string& path, const std::string& label) {
    Instance* i = find(path);
    if (!i) return false;
    const int f = i->find_label(label);
    i->goto_frame(f >= 0 ? f : std::atoi(label.c_str()) - 1, true);
    run_queue();
    return true;
}

bool Player::goto_and_stop(const std::string& path, const std::string& label) {
    Instance* i = find(path);
    if (!i) return false;
    const int f = i->find_label(label);
    i->goto_frame(f >= 0 ? f : std::atoi(label.c_str()) - 1, false);
    run_queue();
    return true;
}

bool Player::set_visible(const std::string& path, bool v) {
    Instance* i = find(path);
    if (i) i->visible = v;
    return i != nullptr;
}

bool Player::set_text(const std::string& path, const std::string& text, bool html) {
    Instance* i = find(path);
    if (!i || i->kind != Instance::Text) return false;
    i->set_text(text, html);
    return true;
}

bool Player::load_movie(const std::string& path, const std::string& file) {
    Instance* i = find(path);
    if (!i || i->kind != Instance::Clip || !loader) return false;
    auto m = loader(file);
    if (!m) return false;
    movies.push_back(m);
    for (auto& c : i->children) unload(c.get());
    i->children.clear();
    i->movie = m.get();
    i->def = &m->root;
    i->frame = -1;
    i->playing = true;
    run_frame(i, 0);
    run_queue();
    return true;
}

// --- drawing -----------------------------------------------------------------------------------------------

const std::vector<Mesh>& Player::meshes(const ShapeDef* shape, const std::vector<ShapeGroup>* groups) {
    const void* key = shape ? static_cast<const void*>(shape) : static_cast<const void*>(groups);
    auto it = mesh_cache_.find(key);
    if (it != mesh_cache_.end()) return it->second;
    const float px = kTwips / std::max(scale_, 0.1f);    // one screen pixel in twips at scale 1
    return mesh_cache_[key] = tessellate(shape ? shape->groups : *groups, px * 0.3f, px);
}

void Player::draw_shape(const std::vector<Mesh>& ms, const MovieDef* m, const Matrix& mat, const CxForm& cx,
                        DrawList& out, const ClipShape* clip) {
    std::vector<DrawVertex> verts;
    for (const Mesh& mesh : ms) {
        TextureRef tex;
        glm::vec4 color;
        Matrix uvm;
        const FillStyle* fs = mesh.fill;
        if (!fs) {
            color = cx.apply(mesh.line_color);
        } else if (fs->kind == FillStyle::Solid) {
            color = cx.apply(fs->color);
        } else if (fs->kind == FillStyle::Bitmap) {
            tex.kind = TextureRef::Bitmap;
            tex.movie = m;
            tex.bitmap = fs->bitmap;
            tex.smooth = fs->smooth;
            tex.repeat = fs->repeat;
            color = glm::clamp(cx.mul, 0.0f, 1.0f);
            uvm = fs->matrix.inverse();
        } else {
            tex.kind = TextureRef::Gradient;
            tex.gradient = fs;
            color = glm::clamp(cx.mul, 0.0f, 1.0f);
            uvm = fs->matrix.inverse();
        }
        if (color.a <= 0.002f) continue;
        verts.clear();
        verts.reserve(mesh.triangles.size());
        for (const glm::vec2& p : mesh.triangles) {
            glm::vec2 uv(0);
            if (tex.kind == TextureRef::Bitmap) {
                uv = uvm.apply(p);
            } else if (tex.kind == TextureRef::Gradient) {
                const glm::vec2 g = uvm.apply(p);
                uv = fs->kind == FillStyle::Linear ? glm::vec2((g.x + 16384.0f) / 32768.0f, 0.5f)
                                                   : (g + 16384.0f) / 32768.0f;
            }
            verts.push_back({to_screen(mat, p), uv, color});
        }
        emit(out, tex, verts.data(), verts.size(), clip);
    }
}

void Player::draw_text(Instance* inst, const Matrix& m, const CxForm& cx, DrawList& out, const ClipShape* clip) {
    if (inst->text.empty()) return;
    const auto* t = static_cast<const EditTextDef*>(inst->def);
    const glm::vec4 color = cx.apply(inst->text_color);
    if (color.a <= 0.002f) return;
    const float gutter = 2 * kTwips;
    const float x0 = t->bounds.xmin + gutter, y0 = t->bounds.ymin + gutter;
    const float w = std::max(inst->text_width * kTwips - 2 * gutter, kTwips);
    const float h = std::max(inst->text_height * kTwips - 2 * gutter, kTwips);

    // Embedded outlines (DefineFont3 glyphs) when the font has every character.
    const CharacterDef* fd = inst->movie ? inst->movie->character(t->font) : nullptr;
    const auto* font = fd && fd->kind == CharacterDef::Font ? static_cast<const FontDef*>(fd) : nullptr;
    const std::u32string cps = code_points(inst->text);
    bool outlines = font && !font->glyphs.empty();
    for (char32_t c : cps)
        if (outlines && c != '\n' && c != '\r' && c != ' ' && !font->index.count(c)) outlines = false;
    if (outlines) {
        const float size = inst->text_size * kTwips;
        const float k = size / font->em;
        auto advance = [&](char32_t c) {
            auto it = font->index.find(c);
            if (it == font->index.end()) return size * 0.3f;
            return size_t(it->second) < font->advances.size() ? font->advances[size_t(it->second)] * k : size * 0.5f;
        };
        std::vector<std::u32string> lines(1);
        for (char32_t c : cps) {
            if (c == '\r' || c == '\n') lines.emplace_back();
            else lines.back().push_back(c);
        }
        if (inst->word_wrap) {
            std::vector<std::u32string> wrapped;
            for (const auto& l : lines) {
                std::u32string cur;
                float cw = 0;
                size_t last_space = std::u32string::npos;
                for (char32_t c : l) {
                    cur.push_back(c);
                    cw += advance(c);
                    if (c == ' ') last_space = cur.size() - 1;
                    if (cw > w && last_space != std::u32string::npos) {
                        wrapped.push_back(cur.substr(0, last_space));
                        cur = cur.substr(last_space + 1);
                        cw = 0;
                        for (char32_t d : cur) cw += advance(d);
                        last_space = std::u32string::npos;
                    }
                }
                wrapped.push_back(cur);
            }
            lines = std::move(wrapped);
        }
        const float ascent = font->ascent > 0 ? font->ascent * k : size * 0.8f;
        const float line_h = (font->ascent + font->descent > 0 ? (font->ascent + font->descent + font->leading) * k : size * 1.15f) + t->leading;
        float y = y0 + ascent;
        for (const auto& l : lines) {
            float lw = 0;
            for (char32_t c : l) lw += advance(c);
            float x = x0;
            if (inst->text_align == 1) x = x0 + w - lw;
            else if (inst->text_align == 2) x = x0 + (w - lw) * 0.5f;
            for (char32_t c : l) {
                auto it = font->index.find(c);
                if (it != font->index.end()) {
                    const ShapeGroup& g = font->glyphs[size_t(it->second)];
                    if (!g.fill_edges.empty() && !g.fill_edges[0].empty()) {
                        static std::unordered_map<const ShapeGroup*, std::vector<ShapeGroup>> wrapped_groups;
                        auto& wg = wrapped_groups[&g];
                        if (wg.empty()) {
                            wg.push_back(g);
                            wg[0].fills.assign(1, FillStyle{});
                            wg[0].fills[0].color = glm::vec4(1.0f);
                        }
                        const Matrix gm = m * Matrix{k, 0, 0, k, x, y};
                        std::vector<Mesh> ms = meshes(nullptr, &wg);
                        CxForm tint;
                        tint.mul = color;
                        draw_shape(ms, inst->movie, gm, tint, out, clip);
                    }
                }
                x += advance(c);
            }
            y += line_h;
        }
        return;
    }

    // The box on screen (a trapezoid under a 3D clip): the text is rasterised at its size.
    const glm::vec2 a = to_screen(m, {x0, y0}), b = to_screen(m, {x0 + w, y0}), c = to_screen(m, {x0 + w, y0 + h}), d = to_screen(m, {x0, y0 + h});
    const float wpx = std::max(glm::length(b - a), glm::length(c - d)), hpx = std::max(glm::length(d - a), glm::length(c - b));
    TextRun run;
    run.text = inst->text;
    run.font = inst->text_font;
    run.size = inst->text_size * kTwips * hpx / h;
    run.color = color;
    run.align = inst->text_align;
    run.width = wpx;
    run.height = hpx;
    run.wrap = inst->word_wrap;
    if (run.width < 1 || run.height < 1 || run.width > 8192 || run.height > 8192) return;
    const int idx = int(out.texts.size());
    out.texts.push_back(std::move(run));
    TextureRef tex;
    tex.kind = TextureRef::Text;
    tex.text = idx;
    // The run is rasterised white; its colour (with the clip's alpha) tints the quad.
    const DrawVertex q[6] = {{a, {0, 0}, color}, {b, {1, 0}, color}, {c, {1, 1}, color},
                             {a, {0, 0}, color}, {c, {1, 1}, color}, {d, {0, 1}, color}};
    emit(out, tex, q, 6, clip);
}

glm::vec2 Player::Persp::project(glm::vec2 local) const {
    const glm::vec2 v = linear.apply(local);
    // rotation about x (sx, cx_), then about y (sy, cy); z points into the screen
    const float y1 = v.y * cx_, z1 = v.y * sx;
    const float x2 = v.x * cy + z1 * sy, z2 = -v.x * sy + z1 * cy;
    const glm::vec3 p = origin + glm::vec3(x2, y1, z2);
    const float k = focal / std::max(focal + p.z, focal * 0.05f);
    return screen.apply(center + (glm::vec2(p) - center) * k);
}

void Player::draw(Instance* inst, const Matrix& pm, const CxForm& pcx, DrawList& out, const ClipShape* clip) {
    if (!inst->visible || inst->removed) return;
    Matrix m = pm * inst->matrix;
    Persp persp;
    const bool is3d = !persp_ && inst->kind == Instance::Clip && (inst->rot_x != 0 || inst->rot_y != 0 || inst->z != 0);
    if (is3d) {
        persp.screen = pm;
        persp.linear = inst->matrix;
        persp.linear.tx = persp.linear.ty = 0;
        persp.origin = glm::vec3(inst->matrix.tx, inst->matrix.ty, inst->z * kTwips);
        persp.sy = std::sin(glm::radians(inst->rot_y)), persp.cy = std::cos(glm::radians(inst->rot_y));
        persp.sx = std::sin(glm::radians(inst->rot_x)), persp.cx_ = std::cos(glm::radians(inst->rot_x));
        const Matrix parent_world = inst->parent_ ? inst->parent_->world_matrix() : Matrix{};
        persp.center = parent_world.inverse().apply(glm::vec2(movie_->width(), movie_->height()) * (0.5f * kTwips));
        const float focal = movie_->width() * 0.5f / std::tan(glm::radians(27.5f)) * kTwips;
        persp.focal = focal / std::max(parent_world.x_scale(), 1e-3f);
        m = Matrix{};
        persp_ = &persp;
    }
    struct Restore {
        const Persp*& p;
        bool on;
        ~Restore() {
            if (on) p = nullptr;
        }
    } restore{persp_, is3d};
    const CxForm cx = pcx * inst->cxform;
    if (cx.mul.a <= 0.002f && cx.add.a <= 0.002f) return;
    switch (inst->kind) {
        case Instance::Shape: draw_shape(meshes(static_cast<const ShapeDef*>(inst->def)), inst->movie, m, cx, out, clip); return;
        case Instance::Text: draw_text(inst, m, cx, out, clip); return;
        case Instance::Clip: break;
    }
    ClipShape set_mask;
    if (inst->mask && !inst->mask->removed) {
        const Matrix base{scale_ / kTwips, 0, 0, scale_ / kTwips, offset_.x, offset_.y};
        const Instance* mp = inst->mask->parent_;
        set_mask = mask_shape(inst->mask.get(), base * (mp ? mp->world_matrix() : Matrix{}), clip);
        clip = &set_mask;
    }
    if (!inst->drawing.empty()) draw_shape(meshes(nullptr, &inst->drawing), inst->movie, m, cx, out, clip);
    const auto children = inst->children;   // scripts may change the list while drawing text
    for (size_t i = 0; i < children.size(); ++i) {
        Instance* c = children[i].get();
        if (c->clip_depth) {
            // A mask layer: what it covers (as its convex outline) clips the depths up to clip_depth.
            const ClipShape shape = mask_shape(c, m, clip);
            const int last = int(c->clip_depth) - kTimelineDepth;
            size_t j = i + 1;
            for (; j < children.size() && children[j]->depth <= last; ++j)
                if (c->visible && !shape.empty()) draw(children[j].get(), m, cx, out, &shape);
            i = j - 1;
            continue;
        }
        if (inst->mask.get() == c) continue;
        draw(c, m, cx, out, clip);
    }
}

ClipShape Player::mask_shape(Instance* mask, const Matrix& m, const ClipShape* within) {
    DrawList tmp;
    const bool was = mask->visible;
    mask->visible = true;
    draw(mask, m, CxForm{}, tmp, nullptr);
    mask->visible = was;
    std::vector<glm::vec2> pts;
    for (const DrawBatch& b : tmp.batches)
        for (const DrawVertex& v : b.vertices) pts.push_back(v.pos);
    ClipShape c = hull(std::move(pts));
    if (within && !c.empty()) {
        // Both convex: clip one outline by the other.
        std::vector<DrawVertex> poly;
        for (glm::vec2 p : c.pts) poly.push_back({p, {}, {}});
        std::vector<DrawVertex> next;
        for (size_t e = 0; e < within->pts.size() && !poly.empty(); ++e) {
            const glm::vec2 a = within->pts[e], b = within->pts[(e + 1) % within->pts.size()];
            next.clear();
            for (size_t i = 0; i < poly.size(); ++i) {
                const glm::vec2 p = poly[i].pos, q = poly[(i + 1) % poly.size()].pos;
                const float sp = side(a, b, p), sq = side(a, b, q);
                if (sp >= 0) next.push_back({p, {}, {}});
                if ((sp >= 0) != (sq >= 0)) next.push_back({p + (q - p) * (sp / (sp - sq)), {}, {}});
            }
            poly.swap(next);
        }
        pts.clear();
        for (const DrawVertex& v : poly) pts.push_back(v.pos);
        c = hull(std::move(pts));
    }
    return c;
}

void Player::render(DrawList& out) {
    out.clear();
    const Matrix base{scale_ / kTwips, 0, 0, scale_ / kTwips, offset_.x, offset_.y};
    draw(root_.get(), base, CxForm{}, out, nullptr);
}

}  // namespace tdkr::swf
